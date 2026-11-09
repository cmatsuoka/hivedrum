/*
 * play_keys.c - play the Hivedrum kit from the keyboard through OpenAL.
 *
 * Keys:
 *   S - Kick
 *   D - Snare
 *   O - Tom 1
 *   I - Tom 2
 *   U - Tom 3
 *   R - Cymbal
 *   F - Hi-hat (closed)
 *   G - Hi-hat (open)
 *   Q or Esc - quit
 *
 * Audio is streamed through OpenAL as a queue of short buffers. Four 256-frame
 * buffers keep the added latency around 20 ms at 48 kHz while leaving enough
 * headroom for ordinary operating-system scheduling jitter.
 *
 * When no voice is sounding the process blocks on the keyboard instead of
 * rendering and streaming silence. The queued tail is left to play out
 * naturally; the stream is restarted on the next trigger.
 */

#include <AL/al.h>
#include <AL/alc.h>

#include <ctype.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "h5.h"

#define BUFFER_FRAMES 256u
#define BUFFER_COUNT 4u
#define POLL_TIMEOUT_MS 2

static struct termios saved_termios;
static volatile sig_atomic_t terminal_is_raw = 0;

static void restore_terminal(void)
{
    if (terminal_is_raw) {
        (void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
        terminal_is_raw = 0;
    }
}

static void on_fatal_signal(int sig)
{
    restore_terminal();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_terminal_guards(void)
{
    static const int signals[] = {
        SIGINT, SIGTERM, SIGHUP, SIGQUIT,
        SIGABRT, SIGSEGV, SIGFPE, SIGBUS, SIGILL
    };
    size_t i;

    (void)atexit(restore_terminal);
    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
        (void)signal(signals[i], on_fatal_signal);
    }
}

static int set_raw_mode(void)
{
    struct termios raw;

    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "This example needs an interactive terminal.\n");
        return -1;
    }
    if (tcgetattr(STDIN_FILENO, &saved_termios) != 0) {
        perror("tcgetattr");
        return -1;
    }

    raw = saved_termios;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        perror("tcsetattr");
        return -1;
    }
    terminal_is_raw = 1;
    return 0;
}

static int handle_key(h5_context kit, int key)
{
    switch (tolower((unsigned char)key)) {
    case 's':
        (void)h5_trigger(kit, H5_INSTR_KICK);
        break;
    case 'd':
        (void)h5_trigger(kit, H5_INSTR_SNARE);
        break;
    case 'o':
        (void)h5_trigger(kit, H5_INSTR_TOM_1);
        break;
    case 'i':
        (void)h5_trigger(kit, H5_INSTR_TOM_2);
        break;
    case 'u':
        (void)h5_trigger(kit, H5_INSTR_TOM_3);
        break;
    case 'r':
        (void)h5_trigger(kit, H5_INSTR_CYMBAL);
        break;
    case 'f':
        (void)h5_trigger(kit, H5_INSTR_HIHAT_CLOSED);
        break;
    case 'g':
        (void)h5_trigger(kit, H5_INSTR_HIHAT_OPEN);
        break;
    case 'q':
    case 27: /* Esc */
    case 3:  /* Ctrl-C, with ISIG disabled */
        return 1;
    default:
        break;
    }
    return 0;
}

static void fill_buffer(h5_context kit, ALuint buffer, uint32_t sample_rate)
{
    int16_t pcm[BUFFER_FRAMES * 2u];

    h5_process(kit, pcm, BUFFER_FRAMES);
    alBufferData(buffer, AL_FORMAT_STEREO16, pcm,
        (ALsizei)(BUFFER_FRAMES * 2u * sizeof(int16_t)),
        (ALsizei)sample_rate);
}

static void service_source(h5_context kit, ALuint source, uint32_t sample_rate)
{
    ALint processed = 0;

    alGetSourcei(source, AL_BUFFERS_PROCESSED, &processed);
    while (processed > 0) {
        ALuint buffer = 0;

        alSourceUnqueueBuffers(source, 1, &buffer);
        if (buffer == 0) {
            break;
        }
        fill_buffer(kit, buffer, sample_rate);
        alSourceQueueBuffers(source, 1, &buffer);
        processed--;
    }
}

/* Reset a stopped source and fill a fresh stream. Only used when the source is
 * not playing, so there is no queued audio to lose. */
static void start_stream(h5_context kit, ALuint source, const ALuint *buffers,
    uint32_t sample_rate)
{
    unsigned int i;

    alSourceStop(source);
    alSourcei(source, AL_BUFFER, 0);
    for (i = 0; i < BUFFER_COUNT; i++) {
        fill_buffer(kit, buffers[i], sample_rate);
        alSourceQueueBuffers(source, 1, &buffers[i]);
    }
    alSourcePlay(source);
}

int main(void)
{
    ALCdevice *device = 0;
    ALCcontext *al_context = 0;
    h5_context kit = 0;
    ALuint source = 0;
    ALuint buffers[BUFFER_COUNT] = {0};
    uint32_t sample_rate = H5_DEFAULT_SAMPLE_RATE;
    int quit = 0;
    int status = EXIT_FAILURE;

    printf("Hivedrum keyboard drums\n");
    printf("  S kick   D snare   O tom 1   I tom 2   U tom 3\n");
    printf("  Q or Esc to quit\n\n");
    fflush(stdout);

    if (set_raw_mode() != 0) {
        return EXIT_FAILURE;
    }
    install_terminal_guards();

    device = alcOpenDevice(0);
    if (device == 0) {
        fprintf(stderr, "Could not open an OpenAL device.\n");
        goto cleanup;
    }
    al_context = alcCreateContext(device, 0);
    if (al_context == 0) {
        fprintf(stderr, "Could not create an OpenAL context.\n");
        goto cleanup;
    }
    if (!alcMakeContextCurrent(al_context)) {
        fprintf(stderr, "Could not make the OpenAL context current.\n");
        goto cleanup;
    }

    {
        ALCint frequency = 0;

        alcGetIntegerv(device, ALC_FREQUENCY, 1, &frequency);
        if (frequency > 0) {
            sample_rate = (uint32_t)frequency;
        }
    }

    kit = h5_create_context(sample_rate);
    if (kit == 0) {
        fprintf(stderr, "Could not create the Hivedrum context.\n");
        goto cleanup;
    }

    alGenSources(1, &source);
    if (alGetError() != AL_NO_ERROR) {
        source = 0;
        fprintf(stderr, "Could not create an OpenAL source.\n");
        goto cleanup;
    }
    alGenBuffers(BUFFER_COUNT, buffers);
    if (alGetError() != AL_NO_ERROR) {
        fprintf(stderr, "Could not create OpenAL buffers.\n");
        goto cleanup;
    }

    printf("Sample rate: %u Hz (about %u ms of queued audio).\n",
        (unsigned)sample_rate,
        (unsigned)((BUFFER_FRAMES * BUFFER_COUNT * 1000u) / sample_rate));
    fflush(stdout);

    while (!quit) {
        struct pollfd pfd;
        int active = h5_is_active(kit);
        int ready;

        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;
        /* While idle, block on the keyboard instead of waking every couple of
         * milliseconds to feed silence to the audio device. */
        ready = poll(&pfd, 1, active ? POLL_TIMEOUT_MS : -1);

        if (ready > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            unsigned char keys[64];
            ssize_t count = read(STDIN_FILENO, keys, sizeof(keys));
            ssize_t k;

            for (k = 0; k < count; k++) {
                if (handle_key(kit, keys[k])) {
                    quit = 1;
                    break;
                }
            }
            if (ready > 0 && (pfd.revents & (POLLHUP | POLLERR)) &&
                count <= 0) {
                quit = 1;
            }
        }

        if (!active && h5_is_active(kit)) {
            ALint state = 0;

            alGetSourcei(source, AL_SOURCE_STATE, &state);
            if (state == AL_PLAYING || state == AL_PAUSED) {
                service_source(kit, source, sample_rate);
                alSourcePlay(source);
            } else {
                start_stream(kit, source, buffers, sample_rate);
            }
        } else if (active) {
            ALint state = 0;

            service_source(kit, source, sample_rate);
            alGetSourcei(source, AL_SOURCE_STATE, &state);
            if (state != AL_PLAYING) {
                alSourcePlay(source);
            }
        }
    }

    status = EXIT_SUCCESS;

cleanup:
    if (source != 0) {
        alSourceStop(source);
    }
    if (buffers[0] != 0) {
        alDeleteBuffers((ALsizei)BUFFER_COUNT, buffers);
    }
    if (source != 0) {
        alDeleteSources(1, &source);
    }
    if (kit != 0) {
        h5_destroy_context(kit);
    }
    if (al_context != 0) {
        alcMakeContextCurrent(0);
        alcDestroyContext(al_context);
    }
    if (device != 0) {
        alcCloseDevice(device);
    }
    if (terminal_is_raw) {
        restore_terminal();
        printf("\n");
    }
    return status;
}
