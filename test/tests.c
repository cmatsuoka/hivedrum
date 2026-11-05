/*
 * tests.c - per-voice render checks.
 *
 * Each command triggers one voice through the public API, renders a single
 * hit, writes a WAV file for listening and checks the envelope, the tail and
 * the direction of the main controls.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h5.h"
#include "h5_internal.h"
#include "tests.h"
#include "wav_writer.h"

#define TEST_RATE 44100u

static int failures = 0;

static void fail(const char *what)
{
    printf("FAIL: %s\n", what);
    failures++;
}

static void ok(const char *what)
{
    printf("ok  : %s\n", what);
}

/* The metallic instruments exist only when their engine is compiled in: the
 * Cymbal behind LIBHIVE_ENGINE_CYMBAL and the Hi-hat behind
 * LIBHIVE_ENGINE_HIHAT. A trimmed build keeps the instruments in the API
 * but they render silence, so the checks skip them. */
static int instrument_available(h5_instrument instrument)
{
    switch (instrument) {
    case H5_INSTR_CYMBAL:
        return H5_TEST_CYMBAL;
    case H5_INSTR_HIHAT_CLOSED:
    case H5_INSTR_HIHAT_OPEN:
        return H5_TEST_HIHAT;
    default:
        return 1;
    }
}

static size_t frames_for_ms(uint32_t sample_rate, uint32_t ms)
{
    return (size_t)((uint64_t)sample_rate * ms / 1000u);
}

static int32_t sample_abs(int16_t v)
{
    return (v < 0) ? -(int32_t)v : (int32_t)v;
}

static int32_t peak_abs(const int16_t *buffer, size_t begin, size_t end)
{
    int32_t peak = 0;
    size_t i;

    for (i = begin; i < end; i++) {
        int32_t v = sample_abs(buffer[2u * i]);

        if (v > peak) {
            peak = v;
        }
    }
    return peak;
}

static size_t first_above(const int16_t *buffer, size_t frames, int32_t level)
{
    size_t i;

    for (i = 0u; i < frames; i++) {
        if (sample_abs(buffer[2u * i]) > level) {
            return i;
        }
    }
    return frames;
}

static size_t last_above(const int16_t *buffer, size_t frames, int32_t level)
{
    size_t i = frames;

    while (i > 0u) {
        i--;
        if (sample_abs(buffer[2u * i]) > level) {
            return i;
        }
    }
    return 0u;
}

static int64_t energy(const int16_t *buffer, size_t begin, size_t end)
{
    int64_t sum = 0;
    size_t i;

    for (i = begin; i < end; i++) {
        int32_t v = buffer[2u * i];

        sum += (int64_t)v * v;
    }
    return sum;
}

static unsigned zero_crossings(const int16_t *buffer, size_t begin, size_t end)
{
    unsigned crossings = 0;
    int previous = 0;
    size_t i;

    for (i = begin; i < end; i++) {
        int current = buffer[2u * i];

        if (current == 0) {
            continue;
        }
        if (previous != 0 && ((previous < 0) != (current < 0))) {
            crossings++;
        }
        previous = current;
    }
    return crossings;
}

static int16_t *render(h5_context ctx, size_t frames)
{
    int16_t *buffer = (int16_t *)malloc(frames * 2u * sizeof(int16_t));

    if (buffer == 0) {
        return 0;
    }
    h5_process(ctx, buffer, frames);
    return buffer;
}

static void set_tom(h5_context ctx, h5_param param, int32_t value)
{
    h5_set_param(ctx, H5_VOICE_TOM_2, param, value);
}

static void set_tom_voice(h5_context ctx, h5_voice voice, h5_param param,
    int32_t value)
{
    h5_set_param(ctx, voice, param, value);
}

static void configure_tom_for_mix(h5_context ctx, h5_voice voice,
    int32_t pitch_hz)
{
    set_tom_voice(ctx, voice, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    set_tom_voice(ctx, voice, H5_PARAM_CLICK_TONE, 0);
    set_tom_voice(ctx, voice, H5_PARAM_BEND, 0);
    set_tom_voice(ctx, voice, H5_PARAM_DECAY, H5_Q16_ONE);
    set_tom_voice(ctx, voice, H5_PARAM_TONE_PITCH, pitch_hz * H5_Q16_ONE);
}

static int command_tom_hit(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 1500u);
    const size_t check_start = frames_for_ms(TEST_RATE, 1200u);
    int16_t *buffer;
    size_t first, last;
    int32_t peak, tail_peak;
    unsigned crossings;

    /* Tom 2 tuning for the hit test: lower pitch, moderate decay and a
     * smaller noise contribution than the default voice. */
    set_tom(ctx, H5_PARAM_TONE_PITCH, 130 * H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_NOISE_PITCH, 7000 * H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_NOISE_TONE, 1 * H5_Q16_ONE / 2);
    set_tom(ctx, H5_PARAM_BEND, 10 * H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_DECAY, 8 * H5_Q16_ONE / 10);

    h5_trigger(ctx, H5_INSTR_TOM_2);
    buffer = render(ctx, frames);
    if (buffer == 0) {
        fail("out of memory");
        return -1;
    }

    first = first_above(buffer, frames, 16);
    last = last_above(buffer, frames, 16);
    peak = peak_abs(buffer, 0u, frames);
    tail_peak = peak_abs(buffer, check_start, frames);
    crossings = zero_crossings(buffer, 0u, frames_for_ms(TEST_RATE, 100u));

    if (first > frames_for_ms(TEST_RATE, 10u)) {
        fail("the Tom hit does not start promptly");
    } else {
        ok("the Tom hit starts promptly");
    }

    if (tail_peak > 1) {
        fail("the Tom default tail does not decay to silence");
    } else {
        ok("the Tom default tail decays to silence");
    }

    if (peak < 1000 || last <= first) {
        fail("the Tom default hit is silent or degenerate");
    } else {
        ok("the Tom default hit is audible");
    }

    if (crossings < 5u || crossings > 2000u) {
        fail("the Tom default timbre is outside the expected band");
    } else {
        ok("the Tom default timbre is in the expected band");
    }

    if (wav_write_stereo16("build/tom.wav", buffer, frames, TEST_RATE) != 0) {
        fail("could not write build/tom.wav");
    } else {
        printf("ok  : wrote build/tom.wav (%lu frames)\n",
            (unsigned long)frames);
    }

    free(buffer);
    return 0;
}

static int command_tom_pitch(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 250u);
    const size_t window = frames_for_ms(TEST_RATE, 150u);
    int16_t *low;
    int16_t *high;
    unsigned zc_low, zc_high;

    h5_reset(ctx);
    set_tom(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_CLICK_TONE, 0);
    set_tom(ctx, H5_PARAM_BEND, 0);
    set_tom(ctx, H5_PARAM_DECAY, 1 * H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_TONE_PITCH, 100 * H5_Q16_ONE);
    h5_trigger(ctx, H5_INSTR_TOM_2);
    low = render(ctx, frames);

    h5_reset(ctx);
    set_tom(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_CLICK_TONE, 0);
    set_tom(ctx, H5_PARAM_BEND, 0);
    set_tom(ctx, H5_PARAM_DECAY, 1 * H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_TONE_PITCH, 400 * H5_Q16_ONE);
    h5_trigger(ctx, H5_INSTR_TOM_2);
    high = render(ctx, frames);

    if (low == 0 || high == 0) {
        fail("out of memory");
        free(low);
        free(high);
        return -1;
    }

    zc_low = zero_crossings(low, 0u, window);
    zc_high = zero_crossings(high, 0u, window);

    if (zc_high < 2u * zc_low) {
        fail("Tom TonePitch does not raise the tone frequency");
    } else {
        ok("Tom TonePitch raises the tone frequency");
    }

    free(low);
    free(high);
    return 0;
}

static int command_tom_decay(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 2000u);
    int16_t *short_hit;
    int16_t *long_hit;
    size_t short_end, long_end;

    h5_reset(ctx);
    set_tom(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_CLICK_TONE, 0);
    set_tom(ctx, H5_PARAM_BEND, 0);
    set_tom(ctx, H5_PARAM_DECAY, H5_Q16_ONE / 5);
    h5_trigger(ctx, H5_INSTR_TOM_2);
    short_hit = render(ctx, frames);

    h5_reset(ctx);
    set_tom(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_CLICK_TONE, 0);
    set_tom(ctx, H5_PARAM_BEND, 0);
    set_tom(ctx, H5_PARAM_DECAY, 3 * H5_Q16_ONE / 2);
    h5_trigger(ctx, H5_INSTR_TOM_2);
    long_hit = render(ctx, frames);

    if (short_hit == 0 || long_hit == 0) {
        fail("out of memory");
        free(short_hit);
        free(long_hit);
        return -1;
    }

    short_end = last_above(short_hit, frames, 8);
    long_end = last_above(long_hit, frames, 8);

    if (long_end <= short_end + frames_for_ms(TEST_RATE, 300u)) {
        fail("Tom Decay does not lengthen the tail");
    } else {
        ok("Tom Decay lengthens the tail");
    }

    free(short_hit);
    free(long_hit);
    return 0;
}

static int command_tom_noise(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 250u);
    const size_t window = frames_for_ms(TEST_RATE, 150u);
    int16_t *tone;
    int16_t *noise;
    unsigned zc_tone, zc_noise;

    h5_reset(ctx);
    set_tom(ctx, H5_PARAM_TONE_PITCH, 200 * H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_BEND, 0);
    set_tom(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_CLICK_TONE, 0);
    h5_trigger(ctx, H5_INSTR_TOM_2);
    tone = render(ctx, frames);

    h5_reset(ctx);
    set_tom(ctx, H5_PARAM_TONE_PITCH, 200 * H5_Q16_ONE);
    set_tom(ctx, H5_PARAM_BEND, 0);
    set_tom(ctx, H5_PARAM_NOISE_TONE, 0);
    set_tom(ctx, H5_PARAM_CLICK_TONE, 0);
    h5_trigger(ctx, H5_INSTR_TOM_2);
    noise = render(ctx, frames);

    if (tone == 0 || noise == 0) {
        fail("out of memory");
        free(tone);
        free(noise);
        return -1;
    }

    zc_tone = zero_crossings(tone, 0u, window);
    zc_noise = zero_crossings(noise, 0u, window);

    if (zc_noise < 2u * zc_tone) {
        fail("Tom NoiseTone does not add high-frequency noise");
    } else {
        ok("Tom NoiseTone adds high-frequency noise");
    }

    /* NoiseTone is the tone amount; the noise side carries the 0.3
     * headroom factor, so a fully tone-weighted hit is at least as loud as a
     * fully noise-weighted hit. */
    if (energy(tone, 0u, window) <= energy(noise, 0u, window)) {
        fail("Tom tone path is quieter than the noise path");
    } else {
        ok("Tom tone path is not quieter than the noise path");
    }

    free(tone);
    free(noise);
    return 0;
}

static int command_tom_simultaneous(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 1000u);
    const size_t bytes = frames * 2u * sizeof(int16_t);
    const h5_voice voices[3] = {
        H5_VOICE_TOM_1, H5_VOICE_TOM_2, H5_VOICE_TOM_3
    };
    const h5_instrument instrs[3] = {
        H5_INSTR_TOM_1, H5_INSTR_TOM_2, H5_INSTR_TOM_3
    };
    const int32_t pitches[3] = { 150, 250, 400 };
    int16_t *solo[3] = { 0, 0, 0 };
    int16_t *all;
    int64_t solo_energy[3];
    int64_t all_energy;
    int32_t value;
    int result = 0;
    size_t i;

    for (i = 0u; i < 3u; i++) {
        h5_reset(ctx);
        configure_tom_for_mix(ctx, voices[i], pitches[i]);
        h5_trigger(ctx, instrs[i]);
        solo[i] = render(ctx, frames);
        if (solo[i] == 0) {
            fail("out of memory");
            free(solo[0]);
            free(solo[1]);
            free(solo[2]);
            return -1;
        }
        if (h5_get_param(ctx, voices[i], H5_PARAM_TONE_PITCH, &value) != H5_OK
            || value != pitches[i] * H5_Q16_ONE) {
            fail("a Tom voice did not keep its own TonePitch");
            result = -1;
        }
    }

    h5_reset(ctx);
    for (i = 0u; i < 3u; i++) {
        configure_tom_for_mix(ctx, voices[i], pitches[i]);
    }
    for (i = 0u; i < 3u; i++) {
        h5_trigger(ctx, instrs[i]);
    }
    all = render(ctx, frames);
    if (all == 0) {
        fail("out of memory");
        free(solo[0]);
        free(solo[1]);
        free(solo[2]);
        return -1;
    }

    all_energy = energy(all, 0u, frames);
    for (i = 0u; i < 3u; i++) {
        solo_energy[i] = energy(solo[i], 0u, frames);
        if (peak_abs(solo[i], 0u, frames) < 100) {
            fail("a solo Tom is silent");
            result = -1;
        }
        if (memcmp(all, solo[i], bytes) == 0) {
            fail("the three-Tom mix equals a solo Tom");
            result = -1;
        }
        if (all_energy <= solo_energy[i]) {
            fail("the three-Tom mix is not louder than a solo Tom");
            result = -1;
        }
    }

    if (result == 0) {
        ok("Tom 1, Tom 2 and Tom 3 sound simultaneously with independent state");
    }

    free(solo[0]);
    free(solo[1]);
    free(solo[2]);
    free(all);
    return result;
}

int command_tom(void)
{
    h5_context ctx = h5_create_context(TEST_RATE);

    if (ctx == 0) {
        fail("could not create a context");
        return 1;
    }

    if (command_tom_hit(ctx) != 0 ||
        command_tom_pitch(ctx) != 0 ||
        command_tom_decay(ctx) != 0 ||
        command_tom_noise(ctx) != 0 ||
        command_tom_simultaneous(ctx) != 0) {
        h5_destroy_context(ctx);
        return 1;
    }

    h5_destroy_context(ctx);
    return failures == 0 ? 0 : 1;
}

static void set_kick(h5_context ctx, h5_param param, int32_t value)
{
    h5_set_param(ctx, H5_VOICE_KICK, param, value);
}

static int command_kick_hit(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 1500u);
    const size_t check_start = frames_for_ms(TEST_RATE, 1200u);
    int16_t *buffer;
    size_t first, last;
    int32_t peak, tail_peak;
    unsigned crossings;

    h5_reset(ctx);
    set_kick(ctx, H5_PARAM_TONE_PITCH, 60 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_NOISE_PITCH, 6000 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE / 2);
    set_kick(ctx, H5_PARAM_BEND, 36 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_DECAY, H5_Q16_ONE / 2);

    h5_trigger(ctx, H5_INSTR_KICK);
    buffer = render(ctx, frames);
    if (buffer == 0) {
        fail("out of memory");
        return -1;
    }

    first = first_above(buffer, frames, 16);
    last = last_above(buffer, frames, 16);
    peak = peak_abs(buffer, 0u, frames);
    tail_peak = peak_abs(buffer, check_start, frames);
    crossings = zero_crossings(buffer, 0u, frames_for_ms(TEST_RATE, 100u));

    if (first > frames_for_ms(TEST_RATE, 10u)) {
        fail("the Kick hit does not start promptly");
    } else {
        ok("the Kick hit starts promptly");
    }

    if (tail_peak > 1) {
        fail("the Kick default tail does not decay to silence");
    } else {
        ok("the Kick default tail decays to silence");
    }

    if (peak < 1000 || last <= first) {
        fail("the Kick default hit is silent or degenerate");
    } else {
        ok("the Kick default hit is audible");
    }

    if (crossings < 5u || crossings > 3000u) {
        fail("the Kick default timbre is outside the expected band");
    } else {
        ok("the Kick default timbre is in the expected band");
    }

    if (wav_write_stereo16("build/kick.wav", buffer, frames, TEST_RATE) != 0) {
        fail("could not write build/kick.wav");
    } else {
        printf("ok  : wrote build/kick.wav (%lu frames)\n",
            (unsigned long)frames);
    }

    free(buffer);
    return 0;
}

static int command_kick_pitch(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 250u);
    const size_t window = frames_for_ms(TEST_RATE, 150u);
    int16_t *low;
    int16_t *high;
    unsigned zc_low, zc_high;

    h5_reset(ctx);
    set_kick(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_CLICK_TONE, 0);
    set_kick(ctx, H5_PARAM_BEND, 0);
    set_kick(ctx, H5_PARAM_DECAY, 1 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_TONE_PITCH, 50 * H5_Q16_ONE);
    h5_trigger(ctx, H5_INSTR_KICK);
    low = render(ctx, frames);

    h5_reset(ctx);
    set_kick(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_CLICK_TONE, 0);
    set_kick(ctx, H5_PARAM_BEND, 0);
    set_kick(ctx, H5_PARAM_DECAY, 1 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_TONE_PITCH, 200 * H5_Q16_ONE);
    h5_trigger(ctx, H5_INSTR_KICK);
    high = render(ctx, frames);

    if (low == 0 || high == 0) {
        fail("out of memory");
        free(low);
        free(high);
        return -1;
    }

    zc_low = zero_crossings(low, 0u, window);
    zc_high = zero_crossings(high, 0u, window);

    if (zc_high < 2u * zc_low) {
        fail("Kick TonePitch does not raise the tone frequency");
    } else {
        ok("Kick TonePitch raises the tone frequency");
    }

    free(low);
    free(high);
    return 0;
}

static int command_kick_bend(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 250u);
    /* The pitch exponent is env^3, so the bend is concentrated at the
     * start; the comparison window stays short enough that the bent hit's
     * initial register is dominant. */
    const size_t window = frames_for_ms(TEST_RATE, 50u);
    int16_t *flat;
    int16_t *fall;
    unsigned zc_flat, zc_fall;

    h5_reset(ctx);
    set_kick(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_CLICK_TONE, 0);
    set_kick(ctx, H5_PARAM_DECAY, 1 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_TONE_PITCH, 30 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_BEND, 0);
    h5_trigger(ctx, H5_INSTR_KICK);
    flat = render(ctx, frames);

    h5_reset(ctx);
    set_kick(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_CLICK_TONE, 0);
    set_kick(ctx, H5_PARAM_DECAY, 1 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_TONE_PITCH, 30 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_BEND, 36 * H5_Q16_ONE);
    h5_trigger(ctx, H5_INSTR_KICK);
    fall = render(ctx, frames);

    if (flat == 0 || fall == 0) {
        fail("out of memory");
        free(flat);
        free(fall);
        return -1;
    }

    zc_flat = zero_crossings(flat, 0u, window);
    zc_fall = zero_crossings(fall, 0u, window);

    if (zc_fall <= zc_flat) {
        fail("Kick Bend does not lower the pitch over the hit");
    } else {
        ok("Kick Bend lowers the pitch over the hit");
    }

    free(flat);
    free(fall);
    return 0;
}

static int command_kick_decay(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 2000u);
    int16_t *short_hit;
    int16_t *long_hit;
    size_t short_end, long_end;

    h5_reset(ctx);
    set_kick(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE / 2);
    set_kick(ctx, H5_PARAM_CLICK_TONE, 0);
    set_kick(ctx, H5_PARAM_BEND, 0);
    set_kick(ctx, H5_PARAM_DECAY, H5_Q16_ONE / 5);
    h5_trigger(ctx, H5_INSTR_KICK);
    short_hit = render(ctx, frames);

    h5_reset(ctx);
    set_kick(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE / 2);
    set_kick(ctx, H5_PARAM_CLICK_TONE, 0);
    set_kick(ctx, H5_PARAM_BEND, 0);
    set_kick(ctx, H5_PARAM_DECAY, 3 * H5_Q16_ONE / 2);
    h5_trigger(ctx, H5_INSTR_KICK);
    long_hit = render(ctx, frames);

    if (short_hit == 0 || long_hit == 0) {
        fail("out of memory");
        free(short_hit);
        free(long_hit);
        return -1;
    }

    short_end = last_above(short_hit, frames, 8);
    long_end = last_above(long_hit, frames, 8);

    if (long_end <= short_end + frames_for_ms(TEST_RATE, 300u)) {
        fail("Kick Decay does not lengthen the tail");
    } else {
        ok("Kick Decay lengthens the tail");
    }

    free(short_hit);
    free(long_hit);
    return 0;
}

static int command_kick_noise(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 250u);
    const size_t window = frames_for_ms(TEST_RATE, 150u);
    int16_t *tone;
    int16_t *noise;
    unsigned zc_tone, zc_noise;

    /* The noise path is gated by (1-Click)^0.7 as well as NoiseTone, so keep
     * a non-zero Click to compare the two balance extremes. The noise
     * envelope releases over Decay, so the chosen Decay keeps it audible
     * across the analysis window (0.3 s is the Kick preset default). */
    h5_reset(ctx);
    set_kick(ctx, H5_PARAM_TONE_PITCH, 60 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_BEND, 0);
    set_kick(ctx, H5_PARAM_CLICK_TONE, H5_Q16_ONE / 2);
    set_kick(ctx, H5_PARAM_DECAY, 3 * H5_Q16_ONE / 10);
    set_kick(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    h5_trigger(ctx, H5_INSTR_KICK);
    tone = render(ctx, frames);

    h5_reset(ctx);
    set_kick(ctx, H5_PARAM_TONE_PITCH, 60 * H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_BEND, 0);
    set_kick(ctx, H5_PARAM_CLICK_TONE, H5_Q16_ONE / 2);
    set_kick(ctx, H5_PARAM_DECAY, 3 * H5_Q16_ONE / 10);
    set_kick(ctx, H5_PARAM_NOISE_TONE, 0);
    h5_trigger(ctx, H5_INSTR_KICK);
    noise = render(ctx, frames);

    if (tone == 0 || noise == 0) {
        fail("out of memory");
        free(tone);
        free(noise);
        return -1;
    }

    zc_tone = zero_crossings(tone, 0u, window);
    zc_noise = zero_crossings(noise, 0u, window);

    if (zc_noise < 2u * zc_tone) {
        fail("Kick NoiseTone does not add high-frequency noise");
    } else {
        ok("Kick NoiseTone adds high-frequency noise");
    }

    free(tone);
    free(noise);
    return 0;
}

static int16_t *render_kick_click(int32_t click, size_t frames)
{
    h5_context ctx = h5_create_context(TEST_RATE);
    int16_t *buffer;

    if (ctx == 0) {
        return 0;
    }
    h5_reset(ctx);
    set_kick(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_BEND, 0);
    set_kick(ctx, H5_PARAM_DECAY, H5_Q16_ONE);
    set_kick(ctx, H5_PARAM_CLICK_TONE, click);
    h5_trigger(ctx, H5_INSTR_KICK);
    buffer = render(ctx, frames);
    h5_destroy_context(ctx);
    return buffer;
}

static int command_kick_click(void)
{
    const size_t frames = frames_for_ms(TEST_RATE, 500u);
    const size_t bytes = frames * 2u * sizeof(int16_t);
    int16_t *a = render_kick_click(0, frames);
    int16_t *b = render_kick_click(H5_Q16_ONE, frames);

    if (a == 0 || b == 0) {
        fail("out of memory");
        free(a);
        free(b);
        return -1;
    }
    if (memcmp(a, b, bytes) == 0) {
        fail("Kick Click does not change the output");
    } else {
        ok("Kick Click changes the output");
    }
    free(a);
    free(b);
    return 0;
}

int command_kick(void)
{
    h5_context ctx = h5_create_context(TEST_RATE);

    if (ctx == 0) {
        fail("could not create a context");
        return 1;
    }

    if (command_kick_hit(ctx) != 0 ||
        command_kick_pitch(ctx) != 0 ||
        command_kick_bend(ctx) != 0 ||
        command_kick_decay(ctx) != 0 ||
        command_kick_noise(ctx) != 0 ||
        command_kick_click() != 0) {
        h5_destroy_context(ctx);
        return 1;
    }

    h5_destroy_context(ctx);
    return failures == 0 ? 0 : 1;
}

static void set_snare(h5_context ctx, h5_param param, int32_t value)
{
    h5_set_param(ctx, H5_VOICE_SNARE, param, value);
}

static int command_snare_hit(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 1500u);
    const size_t check_start = frames_for_ms(TEST_RATE, 1200u);
    int16_t *buffer;
    size_t first, last;
    int32_t peak, tail_peak;
    unsigned crossings;

    /* Standard snare tuning for the hit test. */
    set_snare(ctx, H5_PARAM_TONE_PITCH, 130 * H5_Q16_ONE);
    set_snare(ctx, H5_PARAM_NOISE_PITCH, 7000 * H5_Q16_ONE);
    set_snare(ctx, H5_PARAM_NOISE_TONE, 6 * H5_Q16_ONE / 10);
    set_snare(ctx, H5_PARAM_BEND, 8 * H5_Q16_ONE);
    set_snare(ctx, H5_PARAM_DECAY, 7 * H5_Q16_ONE / 10);

    h5_trigger(ctx, H5_INSTR_SNARE);
    buffer = render(ctx, frames);
    if (buffer == 0) {
        fail("out of memory");
        return -1;
    }

    first = first_above(buffer, frames, 16);
    last = last_above(buffer, frames, 16);
    peak = peak_abs(buffer, 0u, frames);
    tail_peak = peak_abs(buffer, check_start, frames);
    crossings = zero_crossings(buffer, 0u, frames_for_ms(TEST_RATE, 100u));

    if (first > frames_for_ms(TEST_RATE, 10u)) {
        fail("the Snare hit does not start promptly");
    } else {
        ok("the Snare hit starts promptly");
    }

    if (tail_peak > 1) {
        fail("the Snare default tail does not decay to silence");
    } else {
        ok("the Snare default tail decays to silence");
    }

    if (peak < 1000 || last <= first) {
        fail("the Snare default hit is silent or degenerate");
    } else {
        ok("the Snare default hit is audible");
    }

    if (crossings < 5u || crossings > 3000u) {
        fail("the Snare default timbre is outside the expected band");
    } else {
        ok("the Snare default timbre is in the expected band");
    }

    if (wav_write_stereo16("build/snare.wav", buffer, frames, TEST_RATE) != 0) {
        fail("could not write build/snare.wav");
    } else {
        printf("ok  : wrote build/snare.wav (%lu frames)\n",
            (unsigned long)frames);
    }

    free(buffer);
    return 0;
}

static int command_snare_pitch(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 250u);
    const size_t window = frames_for_ms(TEST_RATE, 150u);
    int16_t *low;
    int16_t *high;
    unsigned zc_low, zc_high;

    h5_reset(ctx);
    set_snare(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    set_snare(ctx, H5_PARAM_CLICK_TONE, 0);
    set_snare(ctx, H5_PARAM_BEND, 0);
    set_snare(ctx, H5_PARAM_DECAY, 1 * H5_Q16_ONE);
    set_snare(ctx, H5_PARAM_TONE_PITCH, 100 * H5_Q16_ONE);
    h5_trigger(ctx, H5_INSTR_SNARE);
    low = render(ctx, frames);

    h5_reset(ctx);
    set_snare(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    set_snare(ctx, H5_PARAM_CLICK_TONE, 0);
    set_snare(ctx, H5_PARAM_BEND, 0);
    set_snare(ctx, H5_PARAM_DECAY, 1 * H5_Q16_ONE);
    set_snare(ctx, H5_PARAM_TONE_PITCH, 400 * H5_Q16_ONE);
    h5_trigger(ctx, H5_INSTR_SNARE);
    high = render(ctx, frames);

    if (low == 0 || high == 0) {
        fail("out of memory");
        free(low);
        free(high);
        return -1;
    }

    zc_low = zero_crossings(low, 0u, window);
    zc_high = zero_crossings(high, 0u, window);

    if (zc_high < 2u * zc_low) {
        fail("Snare TonePitch does not raise the tone frequency");
    } else {
        ok("Snare TonePitch raises the tone frequency");
    }

    free(low);
    free(high);
    return 0;
}

static int command_snare_decay(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 2000u);
    int16_t *short_hit;
    int16_t *long_hit;
    size_t short_end, long_end;

    h5_reset(ctx);
    set_snare(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    set_snare(ctx, H5_PARAM_CLICK_TONE, 0);
    set_snare(ctx, H5_PARAM_BEND, 0);
    set_snare(ctx, H5_PARAM_DECAY, H5_Q16_ONE / 5);
    h5_trigger(ctx, H5_INSTR_SNARE);
    short_hit = render(ctx, frames);

    h5_reset(ctx);
    set_snare(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    set_snare(ctx, H5_PARAM_CLICK_TONE, 0);
    set_snare(ctx, H5_PARAM_BEND, 0);
    set_snare(ctx, H5_PARAM_DECAY, 3 * H5_Q16_ONE / 2);
    h5_trigger(ctx, H5_INSTR_SNARE);
    long_hit = render(ctx, frames);

    if (short_hit == 0 || long_hit == 0) {
        fail("out of memory");
        free(short_hit);
        free(long_hit);
        return -1;
    }

    short_end = last_above(short_hit, frames, 8);
    long_end = last_above(long_hit, frames, 8);

    if (long_end <= short_end + frames_for_ms(TEST_RATE, 300u)) {
        fail("Snare Decay does not lengthen the tail");
    } else {
        ok("Snare Decay lengthens the tail");
    }

    free(short_hit);
    free(long_hit);
    return 0;
}

static int command_snare_noise(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 250u);
    const size_t window = frames_for_ms(TEST_RATE, 150u);
    int16_t *tone;
    int16_t *noise;
    unsigned zc_tone, zc_noise;

    h5_reset(ctx);
    set_snare(ctx, H5_PARAM_TONE_PITCH, 100 * H5_Q16_ONE);
    set_snare(ctx, H5_PARAM_BEND, 0);
    set_snare(ctx, H5_PARAM_CLICK_TONE, 0);
    set_snare(ctx, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    h5_trigger(ctx, H5_INSTR_SNARE);
    tone = render(ctx, frames);

    h5_reset(ctx);
    set_snare(ctx, H5_PARAM_TONE_PITCH, 100 * H5_Q16_ONE);
    set_snare(ctx, H5_PARAM_BEND, 0);
    set_snare(ctx, H5_PARAM_CLICK_TONE, 0);
    set_snare(ctx, H5_PARAM_NOISE_TONE, 0);
    h5_trigger(ctx, H5_INSTR_SNARE);
    noise = render(ctx, frames);

    if (tone == 0 || noise == 0) {
        fail("out of memory");
        free(tone);
        free(noise);
        return -1;
    }

    zc_tone = zero_crossings(tone, 0u, window);
    zc_noise = zero_crossings(noise, 0u, window);

    if (zc_noise < 2u * zc_tone) {
        fail("Snare NoiseTone does not add high-frequency rattle");
    } else {
        ok("Snare NoiseTone adds high-frequency rattle");
    }

    /* The rattle path uses four weighted band-pass filters, so its total
     * energy can be lower than a pure sine tone. Verify it is still audible
     * and carries broadband high-frequency content. */
    if (peak_abs(noise, 0u, window) < 100) {
        fail("Snare rattle path is silent");
    } else {
        ok("Snare rattle path is audible");
    }

    free(tone);
    free(noise);
    return 0;
}

static int16_t *render_snare_variant(int32_t noise_tone, int32_t click,
    size_t frames)
{
    h5_context ctx = h5_create_context(TEST_RATE);
    int16_t *buffer;

    if (ctx == 0) {
        return 0;
    }
    h5_reset(ctx);
    set_snare(ctx, H5_PARAM_NOISE_TONE, noise_tone);
    set_snare(ctx, H5_PARAM_BEND, 0);
    set_snare(ctx, H5_PARAM_DECAY, H5_Q16_ONE);
    set_snare(ctx, H5_PARAM_CLICK_TONE, click);
    h5_trigger(ctx, H5_INSTR_SNARE);
    buffer = render(ctx, frames);
    h5_destroy_context(ctx);
    return buffer;
}

static int command_snare_controls(void)
{
    const size_t frames = frames_for_ms(TEST_RATE, 500u);
    const size_t bytes = frames * 2u * sizeof(int16_t);
    int16_t *a;
    int16_t *b;

    a = render_snare_variant(H5_Q16_ONE, 0, frames);
    b = render_snare_variant(H5_Q16_ONE, H5_Q16_ONE, frames);
    if (a == 0 || b == 0) {
        fail("out of memory");
        free(a);
        free(b);
        return -1;
    }
    if (memcmp(a, b, bytes) == 0) {
        fail("Snare Click does not change the output");
    } else {
        ok("Snare Click changes the output");
    }
    free(a);
    free(b);

    return 0;
}

int command_snare(void)
{
    h5_context ctx = h5_create_context(TEST_RATE);

    if (ctx == 0) {
        fail("could not create a context");
        return 1;
    }

    if (command_snare_hit(ctx) != 0 ||
        command_snare_pitch(ctx) != 0 ||
        command_snare_decay(ctx) != 0 ||
        command_snare_noise(ctx) != 0 ||
        command_snare_controls() != 0) {
        h5_destroy_context(ctx);
        return 1;
    }

    h5_destroy_context(ctx);
    return failures == 0 ? 0 : 1;
}

static int16_t *render_with_mix(h5_context ctx, h5_instrument instrument,
    int32_t volume, int32_t pan, size_t frames)
{
    h5_reset(ctx);
    h5_set_instrument_volume(ctx, instrument, volume);
    h5_set_instrument_pan(ctx, instrument, pan);
    h5_trigger(ctx, instrument);
    return render(ctx, frames);
}

static void channel_energies(const int16_t *buffer, size_t frames,
    int64_t *left, int64_t *right)
{
    size_t i;

    *left = 0;
    *right = 0;
    for (i = 0u; i < frames; i++) {
        int32_t l = buffer[2u * i];
        int32_t r = buffer[2u * i + 1u];

        *left += (int64_t)l * l;
        *right += (int64_t)r * r;
    }
}

static int command_mixer_pan(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 300u);
    int16_t *buffer;
    int64_t left, right;
    size_t i;
    int equal = 1;

    buffer = render_with_mix(ctx, H5_INSTR_TOM_1, H5_Q16_ONE, 0, frames);
    if (buffer == 0) {
        fail("out of memory");
        return -1;
    }
    channel_energies(buffer, frames, &left, &right);
    if (left == 0 || right != 0) {
        fail("pan left does not send the voice to the left channel only");
    } else {
        ok("pan left sends the voice to the left channel only");
    }
    free(buffer);

    buffer = render_with_mix(ctx, H5_INSTR_TOM_1, H5_Q16_ONE, H5_Q16_ONE,
        frames);
    if (buffer == 0) {
        fail("out of memory");
        return -1;
    }
    channel_energies(buffer, frames, &left, &right);
    if (right == 0 || left != 0) {
        fail("pan right does not send the voice to the right channel only");
    } else {
        ok("pan right sends the voice to the right channel only");
    }
    free(buffer);

    buffer = render_with_mix(ctx, H5_INSTR_TOM_1, H5_Q16_ONE, H5_Q16_ONE / 2,
        frames);
    if (buffer == 0) {
        fail("out of memory");
        return -1;
    }
    channel_energies(buffer, frames, &left, &right);
    for (i = 0u; i < frames; i++) {
        if (buffer[2u * i] != buffer[2u * i + 1u]) {
            equal = 0;
            break;
        }
    }
    if (left == 0 || !equal) {
        fail("centre pan does not split the voice equally");
    } else {
        ok("centre pan splits the voice equally between both channels");
    }
    free(buffer);

    return 0;
}

static int command_mixer_volume(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 300u);
    int16_t *buffer;
    int64_t silent_left, silent_right;
    int64_t half_left, half_right;
    int64_t full_left, full_right;

    buffer = render_with_mix(ctx, H5_INSTR_TOM_1, 0, H5_Q16_ONE / 2, frames);
    if (buffer == 0) {
        fail("out of memory");
        return -1;
    }
    channel_energies(buffer, frames, &silent_left, &silent_right);
    if (silent_left != 0 || silent_right != 0) {
        fail("volume zero does not silence the instrument");
    } else {
        ok("volume zero silences the instrument");
    }
    free(buffer);

    buffer = render_with_mix(ctx, H5_INSTR_TOM_1, H5_Q16_ONE / 2,
        H5_Q16_ONE / 2, frames);
    if (buffer == 0) {
        fail("out of memory");
        return -1;
    }
    channel_energies(buffer, frames, &half_left, &half_right);
    free(buffer);

    buffer = render_with_mix(ctx, H5_INSTR_TOM_1, H5_Q16_ONE,
        H5_Q16_ONE / 2, frames);
    if (buffer == 0) {
        fail("out of memory");
        return -1;
    }
    channel_energies(buffer, frames, &full_left, &full_right);
    free(buffer);

    if (full_left <= half_left || full_right <= half_right) {
        fail("volume one is not louder than volume one half");
    } else {
        ok("volume one is louder than volume one half");
    }

    return 0;
}

static int command_mixer_levels(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 300u);
    const h5_instrument instruments[H5_INSTR_COUNT] = {
        H5_INSTR_KICK, H5_INSTR_SNARE,
        H5_INSTR_TOM_1, H5_INSTR_TOM_2, H5_INSTR_TOM_3,
        H5_INSTR_CYMBAL, H5_INSTR_HIHAT_CLOSED, H5_INSTR_HIHAT_OPEN
    };
    int32_t levels[H5_INSTR_COUNT];
    h5_status status;
    size_t i;

    /* Validation. */
    status = h5_get_instrument_levels(0, levels);
    if (status != H5_ERR_INVALID_CONTEXT) {
        fail("h5_get_instrument_levels(NULL, ...) did not reject a null context");
    } else {
        ok("h5_get_instrument_levels() rejects a null context");
    }

    status = h5_get_instrument_levels(ctx, 0);
    if (status != H5_ERR_INVALID_CONTEXT) {
        fail("h5_get_instrument_levels() did not reject a null array");
    } else {
        ok("h5_get_instrument_levels() rejects a null array");
    }

    /* The level array reflects the most recent h5_process() block. h5_reset()
     * cuts any still-ringing voices and zeroes the levels, giving a clean
     * "no activity" baseline. */
    h5_reset(ctx);

    memset(levels, 0xA5, sizeof(levels));
    if (h5_get_instrument_levels(ctx, levels) != H5_OK ||
        levels[H5_INSTR_KICK] != 0 || levels[H5_INSTR_SNARE] != 0 ||
        levels[H5_INSTR_TOM_1] != 0 || levels[H5_INSTR_TOM_2] != 0 ||
        levels[H5_INSTR_TOM_3] != 0 || levels[H5_INSTR_CYMBAL] != 0 ||
        levels[H5_INSTR_HIHAT_CLOSED] != 0 || levels[H5_INSTR_HIHAT_OPEN] != 0) {
        fail("levels are non-zero when no voice is active");
    } else {
        ok("all levels are zero when no voice is active");
    }

    /* Each instrument reports activity only on itself. */
    for (i = 0u; i < (size_t)H5_INSTR_COUNT; i++) {
        h5_instrument target = instruments[i];
        size_t j;
        int others_silent = 1;
        int target_audible = 0;

        if (!instrument_available(target)) {
            continue;
        }

        h5_reset(ctx);
        h5_trigger(ctx, target);
        free(render(ctx, frames));

        if (h5_get_instrument_levels(ctx, levels) != H5_OK) {
            fail("h5_get_instrument_levels() failed after a trigger");
            return -1;
        }
        for (j = 0u; j < (size_t)H5_INSTR_COUNT; j++) {
            /* The two Hi-hat instruments share one voice, so a Hi-hat hit
             * reports on both; every other instrument stays silent. */
            int shared = (instruments[j] == H5_INSTR_HIHAT_CLOSED
                    || instruments[j] == H5_INSTR_HIHAT_OPEN)
                && (target == H5_INSTR_HIHAT_CLOSED
                    || target == H5_INSTR_HIHAT_OPEN);

            if (instruments[j] == target) {
                if (levels[j] > 0) {
                    target_audible = 1;
                }
            } else if (!shared && levels[j] != 0) {
                others_silent = 0;
            }
        }
        if (!target_audible) {
            fail("the triggered instrument reports no level");
            return -1;
        }
        if (!others_silent) {
            fail("an untriggered instrument reports a non-zero level");
            return -1;
        }
    }
    ok("only the triggered instrument reports a non-zero level");

    /* Level follows the instrument volume before pan. */
    {
        int32_t level_full, level_half, level_zero;
        int32_t level_left, level_right;

        configure_tom_for_mix(ctx, H5_VOICE_TOM_1, 220);

        h5_reset(ctx);
        h5_set_instrument_volume(ctx, H5_INSTR_TOM_1, H5_Q16_ONE);
        h5_set_instrument_pan(ctx, H5_INSTR_TOM_1, H5_Q16_ONE / 2);
        h5_trigger(ctx, H5_INSTR_TOM_1);
        free(render(ctx, frames));
        if (h5_get_instrument_levels(ctx, levels) != H5_OK) {
            fail("could not read full-volume levels");
            return -1;
        }
        level_full = levels[H5_INSTR_TOM_1];

        h5_reset(ctx);
        h5_set_instrument_volume(ctx, H5_INSTR_TOM_1, H5_Q16_ONE / 2);
        h5_set_instrument_pan(ctx, H5_INSTR_TOM_1, H5_Q16_ONE / 2);
        h5_trigger(ctx, H5_INSTR_TOM_1);
        free(render(ctx, frames));
        if (h5_get_instrument_levels(ctx, levels) != H5_OK) {
            fail("could not read half-volume levels");
            return -1;
        }
        level_half = levels[H5_INSTR_TOM_1];

        h5_reset(ctx);
        h5_set_instrument_volume(ctx, H5_INSTR_TOM_1, 0);
        h5_set_instrument_pan(ctx, H5_INSTR_TOM_1, H5_Q16_ONE / 2);
        h5_trigger(ctx, H5_INSTR_TOM_1);
        free(render(ctx, frames));
        if (h5_get_instrument_levels(ctx, levels) != H5_OK) {
            fail("could not read zero-volume levels");
            return -1;
        }
        level_zero = levels[H5_INSTR_TOM_1];

        if (level_zero != 0 || level_half <= 0 || level_full <= level_half) {
            fail("instrument level does not scale with volume");
            return -1;
        }
        ok("instrument level scales with volume");

        /* Pan does not change the mono instrument level. */
        h5_reset(ctx);
        h5_set_instrument_volume(ctx, H5_INSTR_TOM_1, H5_Q16_ONE);
        h5_set_instrument_pan(ctx, H5_INSTR_TOM_1, 0);
        h5_trigger(ctx, H5_INSTR_TOM_1);
        free(render(ctx, frames));
        if (h5_get_instrument_levels(ctx, levels) != H5_OK) {
            fail("could not read left-pan levels");
            return -1;
        }
        level_left = levels[H5_INSTR_TOM_1];

        h5_reset(ctx);
        h5_set_instrument_volume(ctx, H5_INSTR_TOM_1, H5_Q16_ONE);
        h5_set_instrument_pan(ctx, H5_INSTR_TOM_1, H5_Q16_ONE);
        h5_trigger(ctx, H5_INSTR_TOM_1);
        free(render(ctx, frames));
        if (h5_get_instrument_levels(ctx, levels) != H5_OK) {
            fail("could not read right-pan levels");
            return -1;
        }
        level_right = levels[H5_INSTR_TOM_1];

        if (level_left != level_right) {
            fail("instrument level depends on pan");
            return -1;
        }
        ok("instrument level is independent of pan");
    }

    return 0;
}

static int command_mixer_saturation(void)
{
    int ok_status = 1;

    if (h5_mix_to_q15(0) != 0) {
        fail("the output conversion does not preserve silence");
        ok_status = 0;
    }
    if (h5_mix_to_q15((int64_t)12345 << 16) != 12345) {
        fail("the output conversion does not preserve a Q15 sample");
        ok_status = 0;
    }
    if (h5_mix_to_q15((int64_t)4 << 31) != 32767 ||
        h5_mix_to_q15(-((int64_t)4 << 31)) != -32768) {
        fail("the output conversion does not saturate the mix bus");
        ok_status = 0;
    }
    if (ok_status) {
        ok("the output conversion rounds and saturates once at the mix bus");
    }
    return ok_status ? 0 : -1;
}

static int command_mixer_render(h5_context ctx)
{
    const size_t frames = frames_for_ms(TEST_RATE, 800u);
    const h5_instrument instruments[H5_INSTR_COUNT] = {
        H5_INSTR_KICK, H5_INSTR_SNARE,
        H5_INSTR_TOM_1, H5_INSTR_TOM_2, H5_INSTR_TOM_3,
        H5_INSTR_CYMBAL, H5_INSTR_HIHAT_CLOSED, H5_INSTR_HIHAT_OPEN
    };
    int64_t solo_energy[H5_INSTR_COUNT];
    int64_t all_energy;
    int16_t *buffer;
    int result = 0;
    size_t i;

    for (i = 0u; i < (size_t)H5_INSTR_COUNT; i++) {
        if (!instrument_available(instruments[i])) {
            solo_energy[i] = 0;
            continue;
        }

        h5_reset(ctx);
        h5_trigger(ctx, instruments[i]);
        buffer = render(ctx, frames);
        if (buffer == 0) {
            fail("out of memory");
            return -1;
        }
        solo_energy[i] = energy(buffer, 0u, frames);
        if (peak_abs(buffer, 0u, frames) < 100) {
            fail("an instrument is silent in its solo render");
            result = -1;
        }
        free(buffer);
    }

    h5_reset(ctx);
    for (i = 0u; i < (size_t)H5_INSTR_COUNT; i++) {
        h5_trigger(ctx, instruments[i]);
    }
    buffer = render(ctx, frames);
    if (buffer == 0) {
        fail("out of memory");
        return -1;
    }

    all_energy = energy(buffer, 0u, frames);
    for (i = 0u; i < (size_t)H5_INSTR_COUNT; i++) {
        if (all_energy <= solo_energy[i]) {
            fail("the full kit mix is not louder than a solo instrument");
            result = -1;
            break;
        }
    }

    if (result == 0) {
        ok("all the available instruments sum into one stereo mix");
    }

    if (wav_write_stereo16("build/mix.wav", buffer, frames, TEST_RATE) != 0) {
        fail("could not write build/mix.wav");
        result = -1;
    } else {
        printf("ok  : wrote build/mix.wav (%lu frames)\n",
            (unsigned long)frames);
    }

    free(buffer);
    return result;
}

/*
 * Metallic voices (Hi-hat and Cymbal).
 *
 * Each command renders hits through the public API and checks the envelope,
 * the shared-voice open/closed behaviour and the balance control. The
 * engines are optional, so the checks are compiled in only for the engines
 * the library actually contains.
 */

#if H5_TEST_HIHAT || H5_TEST_CYMBAL

static void set_metallic(h5_context ctx, h5_voice voice, h5_param param,
    int32_t value)
{
    h5_set_param(ctx, voice, param, value);
}

/* A hit renders, swings and decays to true silence once the envelope has
 * played out. */
static int metallic_hit(h5_voice voice, h5_instrument instrument,
    const char *wav_path, int32_t decay_q16, size_t frames)
{
    h5_context ctx = h5_create_context(TEST_RATE);
    int16_t *buffer;
    int32_t peak;
    size_t tail_start;
    int64_t tail_energy;
    int result = 0;

    if (ctx == 0) {
        fail("could not create a context");
        return -1;
    }
    h5_reset(ctx);
    set_metallic(ctx, voice, H5_PARAM_DECAY, decay_q16);
    h5_trigger(ctx, instrument);
    buffer = render(ctx, frames);
    if (buffer == 0) {
        fail("out of memory");
        h5_destroy_context(ctx);
        return -1;
    }

    peak = peak_abs(buffer, 0u, frames);
    if (peak < 1000) {
        fail("the metallic voice is silent in its solo render");
        result = -1;
    } else {
        ok("the metallic voice renders a hit");
    }

    /* The envelope ends exactly at attack + release, so the tail is
     * silence. */
    tail_start = frames - frames_for_ms(TEST_RATE, 100u);
    tail_energy = energy(buffer, tail_start, frames);
    if (tail_energy != 0) {
        fail("the metallic voice does not decay to silence");
        result = -1;
    } else {
        ok("the metallic voice decays to silence");
    }

    if (wav_write_stereo16(wav_path, buffer, frames, TEST_RATE) != 0) {
        fail("could not write the metallic WAV file");
        result = -1;
    } else {
        printf("ok  : wrote %s (%lu frames)\n", wav_path,
            (unsigned long)frames);
    }

    free(buffer);
    h5_destroy_context(ctx);
    return result;
}

/* The Noise/Tone control is the tone amount of the noise path: at 1 the
 * voice is the metallic bank alone, at 0 the noise alone. */
static int metallic_balance(h5_voice voice, h5_instrument instrument)
{
    h5_context ctx = h5_create_context(TEST_RATE);
    const size_t frames = frames_for_ms(TEST_RATE, 300u);
    int16_t *metallic;
    int16_t *noisy;
    int differs;
    int32_t metallic_peak;
    int32_t noisy_peak;

    if (ctx == 0) {
        fail("could not create a context");
        return -1;
    }

    h5_reset(ctx);
    set_metallic(ctx, voice, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    h5_trigger(ctx, instrument);
    metallic = render(ctx, frames);

    h5_reset(ctx);
    set_metallic(ctx, voice, H5_PARAM_NOISE_TONE, 0);
    h5_trigger(ctx, instrument);
    noisy = render(ctx, frames);

    if (metallic == 0 || noisy == 0) {
        fail("out of memory");
        free(metallic);
        free(noisy);
        h5_destroy_context(ctx);
        return -1;
    }

    differs = memcmp(metallic, noisy, frames * 2u * sizeof(int16_t)) != 0;
    metallic_peak = peak_abs(metallic, 0u, frames);
    noisy_peak = peak_abs(noisy, 0u, frames);

    if (!differs) {
        fail("the Noise/Tone control does not change the balance");
    } else if (metallic_peak < 1000 || noisy_peak < 1000) {
        fail("an end of the Noise/Tone range is silent");
    } else {
        ok("the Noise/Tone control changes the metallic balance");
    }

    free(metallic);
    free(noisy);
    h5_destroy_context(ctx);
    return 0;
}

/* The Bell/Cymbal control is the balance between the metallic bank and the
 * noise component. */
static int metallic_bell_cymbal(h5_voice voice, h5_instrument instrument)
{
    h5_context ctx = h5_create_context(TEST_RATE);
    const size_t frames = frames_for_ms(TEST_RATE, 300u);
    int16_t *bell;
    int16_t *cymbal;
    int differs;
    int32_t bell_peak;
    int32_t cymbal_peak;

    if (ctx == 0) {
        fail("could not create a context");
        return -1;
    }

    h5_reset(ctx);
    set_metallic(ctx, voice, H5_PARAM_BELL_CYMBAL, H5_Q16_ONE);
    set_metallic(ctx, voice, H5_PARAM_NOISE_TONE, 0);
    h5_trigger(ctx, instrument);
    bell = render(ctx, frames);

    h5_reset(ctx);
    set_metallic(ctx, voice, H5_PARAM_BELL_CYMBAL, 0);
    set_metallic(ctx, voice, H5_PARAM_NOISE_TONE, 0);
    h5_trigger(ctx, instrument);
    cymbal = render(ctx, frames);

    if (bell == 0 || cymbal == 0) {
        fail("out of memory");
        free(bell);
        free(cymbal);
        h5_destroy_context(ctx);
        return -1;
    }

    differs = memcmp(bell, cymbal, frames * 2u * sizeof(int16_t)) != 0;
    bell_peak = peak_abs(bell, 0u, frames);
    cymbal_peak = peak_abs(cymbal, 0u, frames);

    if (!differs) {
        fail("the Bell/Cymbal control does not change the balance");
    } else if (bell_peak < 1000 || cymbal_peak < 1000) {
        fail("an end of the Bell/Cymbal range is silent");
    } else {
        ok("the Bell/Cymbal control changes the metallic balance");
    }

    free(bell);
    free(cymbal);
    h5_destroy_context(ctx);
    return 0;
}

/* The Bend control sweeps the bank of the metallic voices. */
static int metallic_bend(h5_voice voice, h5_instrument instrument)
{
    h5_context ctx = h5_create_context(TEST_RATE);
    const size_t frames = frames_for_ms(TEST_RATE, 300u);
    int16_t *flat;
    int16_t *bent;
    int differs;
    int32_t flat_peak;
    int32_t bent_peak;

    if (ctx == 0) {
        fail("could not create a context");
        return -1;
    }

    h5_reset(ctx);
    set_metallic(ctx, voice, H5_PARAM_BEND, 0);
    h5_trigger(ctx, instrument);
    flat = render(ctx, frames);

    h5_reset(ctx);
    set_metallic(ctx, voice, H5_PARAM_BEND, 12 * H5_Q16_ONE);
    h5_trigger(ctx, instrument);
    bent = render(ctx, frames);

    if (flat == 0 || bent == 0) {
        fail("out of memory");
        free(flat);
        free(bent);
        h5_destroy_context(ctx);
        return -1;
    }

    differs = memcmp(flat, bent, frames * 2u * sizeof(int16_t)) != 0;
    flat_peak = peak_abs(flat, 0u, frames);
    bent_peak = peak_abs(bent, 0u, frames);

    if (!differs) {
        fail("the Bend control does not sweep the metallic bank");
    } else if (flat_peak < 1000 || bent_peak < 1000) {
        fail("an end of the Bend range is silent");
    } else {
        ok("the Bend control sweeps the metallic bank");
    }

    free(flat);
    free(bent);
    h5_destroy_context(ctx);
    return 0;
}

/* The open Hi-hat rings for longer than the closed one; both are the same
 * voice. */
#if H5_TEST_HIHAT
static int command_hihat_open_close(void)
{
    h5_context ctx = h5_create_context(TEST_RATE);
    const size_t frames = frames_for_ms(TEST_RATE, 1500u);
    int16_t *closed;
    int16_t *open;
    size_t closed_end;
    size_t open_end;

    if (ctx == 0) {
        fail("could not create a context");
        return -1;
    }

    h5_reset(ctx);
    set_metallic(ctx, H5_VOICE_HIHAT, H5_PARAM_DECAY,
        (int32_t)(H5_Q16_ONE * 3 / 10));
    h5_trigger(ctx, H5_INSTR_HIHAT_CLOSED);
    closed = render(ctx, frames);

    h5_reset(ctx);
    set_metallic(ctx, H5_VOICE_HIHAT, H5_PARAM_DECAY,
        (int32_t)(H5_Q16_ONE * 3 / 10));
    h5_trigger(ctx, H5_INSTR_HIHAT_OPEN);
    open = render(ctx, frames);

    if (closed == 0 || open == 0) {
        fail("out of memory");
        free(closed);
        free(open);
        h5_destroy_context(ctx);
        return -1;
    }

    closed_end = last_above(closed, frames, 1000);
    open_end = last_above(open, frames, 1000);
    if (open_end <= closed_end) {
        fail("the open Hi-hat does not ring longer than the closed one");
    } else {
        ok("the open Hi-hat rings longer than the closed one");
    }

    free(closed);
    free(open);
    h5_destroy_context(ctx);
    return 0;
}

/* The Hi-hat trims its Bell/Cymbal and Noise/Tone values as the strike level
 * falls, on both the closed and the open instrument. The Noise/Tone trim is
 * the stronger of the two, and the Cymbal keeps both values. */
static int command_hihat_velocity_balance(void)
{
    struct h5_context_s *ctx =
        (struct h5_context_s *)h5_create_context(TEST_RATE);
    int32_t bell_full, bell_soft, bell_open;
    int32_t tone_full, tone_soft, tone_open;
    int32_t bell_drop, tone_drop;
    int result = 0;

    if (ctx == 0) {
        fail("could not create a context");
        return -1;
    }

    h5_reset((h5_context)ctx);
    h5_trigger_velocity((h5_context)ctx, H5_INSTR_HIHAT_CLOSED, 127u);
    bell_full = ctx->hihat.bell_hit_q16;
    tone_full = ctx->hihat.noise_tone_hit_q16;

    h5_trigger_velocity((h5_context)ctx, H5_INSTR_HIHAT_CLOSED, 32u);
    bell_soft = ctx->hihat.bell_hit_q16;
    tone_soft = ctx->hihat.noise_tone_hit_q16;

    if (bell_soft >= bell_full || tone_soft >= tone_full) {
        fail("a soft closed Hi-hat does not trim its balances");
        result = -1;
    } else {
        bell_drop = ((bell_full - bell_soft) * H5_Q16_ONE) / bell_full;
        tone_drop = ((tone_full - tone_soft) * H5_Q16_ONE) / tone_full;
        if (tone_drop <= bell_drop) {
            fail("the soft-hit Noise/Tone trim is not stronger than the"
                " Bell/Cymbal trim");
            result = -1;
        } else if (bell_drop > H5_Q16_ONE / 5) {
            fail("the soft-hit Bell/Cymbal trim is too strong");
            result = -1;
        } else {
            ok("a soft Hi-hat trims Noise/Tone more than Bell/Cymbal");
        }
    }

    /* The open instrument follows the same law at the same strike level. */
    h5_trigger_velocity((h5_context)ctx, H5_INSTR_HIHAT_OPEN, 32u);
    bell_open = ctx->hihat.bell_hit_q16;
    tone_open = ctx->hihat.noise_tone_hit_q16;
    if (bell_open != bell_soft || tone_open != tone_soft) {
        fail("the Open Hi-hat does not trim its balances like the closed one");
        result = -1;
    } else {
        ok("the Open Hi-hat follows the same soft-hit balance trim");
    }

#if H5_TEST_CYMBAL
    /* The Cymbal is not a Hi-hat and keeps its balances at every velocity. */
    h5_trigger_velocity((h5_context)ctx, H5_INSTR_CYMBAL, 32u);
    if (ctx->cymbal.bell_hit_q16 != ctx->cymbal.bell_q16
        || ctx->cymbal.noise_tone_hit_q16 != ctx->cymbal.noise_tone_q16) {
        fail("a soft Cymbal trims its balances");
        result = -1;
    } else {
        ok("the Cymbal keeps its balances at every velocity");
    }
#endif

    h5_destroy_context((h5_context)ctx);
    return result;
}

#endif /* H5_TEST_HIHAT */

#endif /* H5_TEST_HIHAT || H5_TEST_CYMBAL */

#if H5_TEST_HIHAT
int command_hihat(void)
{
    const size_t frames = frames_for_ms(TEST_RATE, 1500u);
    int result = 0;

    if (metallic_hit(H5_VOICE_HIHAT, H5_INSTR_HIHAT_CLOSED,
            "build/hihat.wav", (int32_t)(H5_Q16_ONE / 2), frames) != 0) {
        result = -1;
    }
    if (metallic_balance(H5_VOICE_HIHAT, H5_INSTR_HIHAT_CLOSED) != 0) {
        result = -1;
    }
    if (metallic_bell_cymbal(H5_VOICE_HIHAT, H5_INSTR_HIHAT_CLOSED) != 0) {
        result = -1;
    }
    if (metallic_bend(H5_VOICE_HIHAT, H5_INSTR_HIHAT_CLOSED) != 0) {
        result = -1;
    }
    if (command_hihat_open_close() != 0) {
        result = -1;
    }
    if (command_hihat_velocity_balance() != 0) {
        result = -1;
    }
    return result;
}
#else
int command_hihat(void)
{
    printf("skip: the Hi-hat engine is not compiled in\n");
    return 0;
}
#endif

#if H5_TEST_CYMBAL
int command_cymbal(void)
{
    const size_t frames = frames_for_ms(TEST_RATE, 4000u);
    int result = 0;

    if (metallic_hit(H5_VOICE_CYMBAL, H5_INSTR_CYMBAL,
            "build/cymbal.wav", H5_Q16_ONE, frames) != 0) {
        result = -1;
    }
    if (metallic_balance(H5_VOICE_CYMBAL, H5_INSTR_CYMBAL) != 0) {
        result = -1;
    }
    if (metallic_bell_cymbal(H5_VOICE_CYMBAL, H5_INSTR_CYMBAL) != 0) {
        result = -1;
    }
    if (metallic_bend(H5_VOICE_CYMBAL, H5_INSTR_CYMBAL) != 0) {
        result = -1;
    }
    return result;
}
#else
int command_cymbal(void)
{
    printf("skip: the Cymbal engine is not compiled in\n");
    return 0;
}
#endif

/*
 * Velocity response.
 *
 * The strike level of a hit follows the MIDI velocity: it scales the level
 * of the whole hit, the depth of the pitch envelope, the noise cutoff sweep
 * and the weight of the transient. The per-voice Sensitivity knob scales how
 * much of that response the velocity gets, and at 0 the velocity is ignored.
 */

/* Render one hit at a given velocity and sensitivity. */
static int16_t *render_velocity(h5_instrument instrument, uint8_t velocity,
    int32_t sensitivity, size_t frames)
{
    h5_context ctx = h5_create_context(TEST_RATE);
    int16_t *buffer;

    if (ctx == 0) {
        return 0;
    }
    h5_reset(ctx);
    h5_set_param(ctx, h5_voice_from_instrument(instrument),
        H5_PARAM_SENSITIVITY, sensitivity);
    h5_trigger_velocity(ctx, instrument, velocity);
    buffer = render(ctx, frames);
    h5_destroy_context(ctx);
    return buffer;
}

/* Render a Kick whose tone sweeps the full bend range, so the pitch
 * envelope is visible in the first moments of the hit. */
static int16_t *render_kick_sweep(uint8_t velocity, size_t frames)
{
    h5_context ctx = h5_create_context(TEST_RATE);
    int16_t *buffer;

    if (ctx == 0) {
        return 0;
    }
    h5_reset(ctx);
    h5_set_param(ctx, H5_VOICE_KICK, H5_PARAM_TONE_PITCH, 40 * H5_Q16_ONE);
    h5_set_param(ctx, H5_VOICE_KICK, H5_PARAM_BEND, 62 * H5_Q16_ONE);
    h5_set_param(ctx, H5_VOICE_KICK, H5_PARAM_NOISE_TONE, H5_Q16_ONE);
    h5_set_param(ctx, H5_VOICE_KICK, H5_PARAM_CLICK_TONE, 0);
    h5_set_param(ctx, H5_VOICE_KICK, H5_PARAM_DECAY, H5_Q16_ONE);
    h5_trigger_velocity(ctx, H5_INSTR_KICK, velocity);
    buffer = render(ctx, frames);
    h5_destroy_context(ctx);
    return buffer;
}

/* A soft hit is quieter than a hard one, and a hit at sensitivity 0 sounds
 * the same at every velocity. */
static int command_velocity_level(void)
{
    const size_t frames = frames_for_ms(TEST_RATE, 300u);
    const size_t bytes = frames * 2u * sizeof(int16_t);
    int16_t *full;
    int16_t *soft;
    int16_t *muted;
    int16_t *silent;

    full = render_velocity(H5_INSTR_KICK, 127u, H5_Q16_ONE, frames);
    soft = render_velocity(H5_INSTR_KICK, 32u, H5_Q16_ONE, frames);
    muted = render_velocity(H5_INSTR_KICK, 32u, 0, frames);
    silent = render_velocity(H5_INSTR_KICK, 0u, H5_Q16_ONE, frames);
    if (full == 0 || soft == 0 || muted == 0 || silent == 0) {
        fail("out of memory");
        free(full);
        free(soft);
        free(muted);
        free(silent);
        return -1;
    }

    if (peak_abs(soft, 0u, frames) >= peak_abs(full, 0u, frames)) {
        fail("a soft hit is not quieter than a hard one");
    } else {
        ok("a soft hit is quieter than a hard one");
    }

    if (memcmp(muted, full, bytes) != 0) {
        fail("sensitivity 0 does not ignore the velocity");
    } else {
        ok("sensitivity 0 ignores the velocity");
    }

    if (peak_abs(silent, 0u, frames) != 0) {
        fail("a hit at velocity 0 is not silent");
    } else {
        ok("a hit at velocity 0 is silent");
    }

    free(full);
    free(soft);
    free(muted);
    free(silent);
    return 0;
}

/* The pitch envelope sweeps less far on a soft hit, so the tone starts
 * lower. */
static int command_velocity_bend(void)
{
    const size_t frames = frames_for_ms(TEST_RATE, 200u);
    const size_t window = frames_for_ms(TEST_RATE, 20u);
    int16_t *full;
    int16_t *soft;
    unsigned crossings_full;
    unsigned crossings_soft;

    full = render_kick_sweep(127u, frames);
    soft = render_kick_sweep(20u, frames);
    if (full == 0 || soft == 0) {
        fail("out of memory");
        free(full);
        free(soft);
        return -1;
    }

    crossings_full = zero_crossings(full, 0u, window);
    crossings_soft = zero_crossings(soft, 0u, window);

    if (crossings_soft >= crossings_full) {
        fail("a soft hit does not sweep the pitch less far");
    } else {
        ok("a soft hit sweeps the pitch less far");
    }

    free(full);
    free(soft);
    return 0;
}

int command_velocity(void)
{
    if (command_velocity_level() != 0 || command_velocity_bend() != 0) {
        return 1;
    }
    return failures == 0 ? 0 : 1;
}

int command_mixer(void)
{
    h5_context ctx = h5_create_context(TEST_RATE);

    if (ctx == 0) {
        fail("could not create a context");
        return 1;
    }

    if (command_mixer_pan(ctx) != 0 ||
        command_mixer_volume(ctx) != 0 ||
        command_mixer_levels(ctx) != 0 ||
        command_mixer_saturation() != 0 ||
        command_mixer_render(ctx) != 0) {
        h5_destroy_context(ctx);
        return 1;
    }

    h5_destroy_context(ctx);
    return failures == 0 ? 0 : 1;
}
