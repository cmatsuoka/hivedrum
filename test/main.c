/*
 * main.c - test harness for the Hive-V library.
 *
 * Commands:
 *   --init              report the context size and the default sample rate
 *   --sine              verify the shared sine table against
 *                       sin(2*pi*i/1024) at all 1024 indices
 *   --silence [path]    render silence, check it, and write a WAV file
 *   --api               verify the public API: context lifecycle, parameters,
 *                       mixer, triggers and render
 *   --kick              render a Kick hit and verify its controls
 *   --tom               render a Tom hit and verify its controls
 *   --snare             render a Snare hit and verify its controls
 *   --hihat             render Hi-hat hits and verify open/closed
 *   --cymbal            render a Cymbal hit and verify its controls
 *   --mixer             verify volume, pan and the stereo output stage
 *   --velocity          verify the velocity response and the sensitivity
 *                       control
 *   --fx                verify the shared reverb and noise gate
 *   --help              this text
 *
 * Exit status is 0 when every check passed, 1 otherwise.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h5.h"
#include "h5_internal.h"
#include "dsp.h"
#include "tests.h"
#include "wav_writer.h"

#ifndef TEST_PI
#define TEST_PI 3.14159265358979323846
#endif

#define TEST_SILENCE_SECONDS 0.25

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

static struct h5_context_s *context_new(uint32_t sample_rate)
{
    struct h5_context_s *ctx =
        (struct h5_context_s *)h5_create_context(sample_rate);

    if (ctx == 0) {
        printf("FAIL: out of memory\n");
        exit(1);
    }
    return ctx;
}

/* The metallic voices are optional engines: a voice whose engine is not
 * compiled in supports no parameter at all, so it is not checked here.
 * The Cymbal is behind LIBHIVE_ENGINE_CYMBAL and the Hi-hat behind
 * LIBHIVE_ENGINE_HIHAT. */
static int metallic_param_supported(h5_voice voice, h5_param param)
{
    int present = (voice == H5_VOICE_CYMBAL) ? H5_TEST_CYMBAL : H5_TEST_HIHAT;

    if (!present) {
        return 1;
    }
    return h5_param_supported(voice, param) ? 1 : 0;
}

static int command_init(void)
{
    struct h5_context_s *ctx = context_new(0);

    printf("context size        : %lu bytes\n",
        (unsigned long)sizeof(struct h5_context_s));
    printf("fx delay block      : %lu bytes\n",
        (unsigned long)ctx->fx.block_size);
    printf("default sample rate : %u Hz\n", (unsigned)H5_DEFAULT_SAMPLE_RATE);

    if (ctx->sample_rate != H5_DEFAULT_SAMPLE_RATE) {
        fail("h5_create_context(0) did not select the default sample rate");
    } else {
        ok("h5_create_context(0) selects the default sample rate");
    }

    h5_set_sample_rate((h5_context)ctx, 22050);
    if (ctx->sample_rate != 22050u) {
        fail("h5_set_sample_rate() did not take effect");
    } else {
        ok("h5_set_sample_rate() takes effect");
    }

    h5_set_sample_rate((h5_context)ctx, 0);
    if (ctx->sample_rate != H5_DEFAULT_SAMPLE_RATE) {
        fail("h5_set_sample_rate(0) did not restore the default rate");
    } else {
        ok("h5_set_sample_rate(0) restores the default rate");
    }

    h5_destroy_context((h5_context)ctx);

    return failures == 0 ? 0 : 1;
}

static int command_sine(void)
{
    struct h5_context_s *ctx = context_new(H5_DEFAULT_SAMPLE_RATE);
    unsigned int i;
    int mismatches = 0;
    int worst = 0;
    int fallback_mismatches = 0;

    if (h5_sine == 0) {
        fail("h5_create_context() did not rebuild the sine table");
        h5_destroy_context((h5_context)ctx);
        return 1;
    }

    for (i = 0; i < H5_SINE_TABLE_SIZE; i++) {
        double exact = 32767.0 * sin(2.0 * TEST_PI * (double)i / (double)H5_SINE_TABLE_SIZE);
        int expected = (int)floor(exact + 0.5);
        int got = (int)h5_sine[i];
        int error = got - expected;

        if (error < 0) {
            error = -error;
        }
        if (error > worst) {
            worst = error;
        }
        if (error != 0) {
            if (mismatches < 8) {
                printf("      i=%u: got %d, expected %d\n", i, got, expected);
            }
            mismatches++;
        }

        if ((int)h5_sine_at(i) != got) {
            fallback_mismatches++;
        }
    }

    if (mismatches != 0) {
        printf("FAIL: %d of %u rebuilt entries differ (worst %d LSB)\n",
            mismatches, (unsigned)H5_SINE_TABLE_SIZE, worst);
        failures++;
    } else {
        ok("the rebuilt table matches sin(2*pi*i/1024) at all 1024 indices");
    }

    if (fallback_mismatches != 0) {
        fail("the quadrant fallback disagrees with the rebuilt table");
    } else {
        ok("the quadrant lookup agrees with the rebuilt table");
    }

    /* Only the first quadrant lives in the image; the full period is built
     * once, on the first h5_create_context(), and shared by every context. */
    if ((H5_SINE_QUAD_SIZE * sizeof(int16_t)) != 514u) {
        fail("the sine quadrant is not 514 bytes");
    } else {
        ok("the sine quadrant is 514 bytes (257 x int16)");
    }

    {
        const char *table = (const char *)h5_sine;
        const char *ctx_start = (const char *)ctx;
        const char *ctx_end = ctx_start + sizeof(struct h5_context_s);

        if ((table >= ctx_start) && (table < ctx_end)) {
            fail("the sine table lives inside the context");
        } else {
            ok("the sine table is shared, not stored in the context");
        }
    }

    h5_destroy_context((h5_context)ctx);
    return failures == 0 ? 0 : 1;
}

static int command_silence(const char *path)
{
    struct h5_context_s *ctx = context_new(H5_DEFAULT_SAMPLE_RATE);
    const uint32_t sample_rate = H5_DEFAULT_SAMPLE_RATE;
    size_t frames = (size_t)((TEST_SILENCE_SECONDS * (double)sample_rate) + 0.5);
    int16_t *buffer = (int16_t *)malloc(frames * 2u * sizeof(int16_t));
    size_t i;
    int nonzero = 0;

    if (buffer == 0) {
        printf("FAIL: out of memory\n");
        h5_destroy_context((h5_context)ctx);
        return 1;
    }

    memset(buffer, 0xA5, frames * 2u * sizeof(int16_t));

    h5_process((h5_context)ctx, buffer, frames);

    for (i = 0; i < frames * 2u; i++) {
        if (buffer[i] != 0) {
            nonzero++;
        }
    }

    if (nonzero != 0) {
        printf("FAIL: %d samples are not silent\n", nonzero);
        failures++;
    } else {
        ok("the renderer writes silence when no voice is triggered");
    }

    if (wav_write_stereo16(path, buffer, frames, sample_rate) != 0) {
        printf("FAIL: could not write %s\n", path);
        failures++;
    } else {
        printf("ok  : wrote %s (%lu frames)\n", path, (unsigned long)frames);
    }

    free(buffer);
    h5_destroy_context((h5_context)ctx);
    return failures == 0 ? 0 : 1;
}

static int command_api(void)
{
    struct h5_context_s *ctx;
    int16_t block[64];
    int32_t value;
    h5_status status;

    /* Version reporting. */
    if (h5_version_number() != H5_VERSION_NUMBER) {
        fail("h5_version_number() does not match H5_VERSION_NUMBER");
    } else {
        ok("h5_version_number() returns the header version");
    }

    h5_destroy_context(0);
    ok("h5_destroy_context(NULL) is ignored");

    ctx = context_new(0);
    if (ctx->sample_rate != H5_DEFAULT_SAMPLE_RATE) {
        fail("h5_create_context(0) selected the wrong rate");
    } else {
        ok("h5_create_context(0) selects the default rate");
    }

    h5_set_sample_rate((h5_context)ctx, 22050);
    if (ctx->sample_rate != 22050u) {
        fail("h5_set_sample_rate() did not take effect");
    } else {
        ok("h5_set_sample_rate() takes effect");
    }

    h5_reset((h5_context)ctx);
    if (ctx->sample_rate != 22050u) {
        fail("h5_reset() changed the sample rate");
    } else {
        ok("h5_reset() keeps the sample rate");
    }

    memset(block, 0x5A, sizeof(block));
    h5_process((h5_context)ctx, block, 0);
    ok("h5_process() accepts zero frames");

    memset(block, 0x5A, sizeof(block));
    h5_process((h5_context)ctx, block, sizeof(block) / (2u * sizeof(int16_t)));
    if (block[0] != 0 || block[63] != 0) {
        fail("h5_process() did not clear the whole block");
    } else {
        ok("h5_process() clears the whole block");
    }

    memset(block, 0x5A, sizeof(block));
    h5_process(0, block, sizeof(block) / (2u * sizeof(int16_t)));
    if (block[0] != 0 || block[63] != 0) {
        fail("h5_process(NULL, ...) did not clear the whole block");
    } else {
        ok("h5_process(NULL, ...) clears the whole block");
    }

    h5_destroy_context((h5_context)ctx);

    ctx = context_new(48000);
    if (ctx->sample_rate != 48000u) {
        fail("h5_create_context(48000) selected the wrong rate");
    } else {
        ok("h5_create_context() honors an explicit rate");
    }

    /* Parameter set/get round trip. */
    status = h5_set_param((h5_context)ctx, H5_VOICE_KICK, H5_PARAM_TONE_PITCH,
        50 * H5_Q16_ONE);
    if (status != H5_OK) {
        fail("h5_set_param(Kick, TonePitch, 50 Hz) did not return OK");
    } else {
        ok("h5_set_param() accepts a supported parameter");
    }

    status = h5_get_param((h5_context)ctx, H5_VOICE_KICK, H5_PARAM_TONE_PITCH,
        &value);
    if (status != H5_OK || value != 50 * H5_Q16_ONE) {
        fail("h5_get_param() did not return the stored Kick TonePitch");
    } else {
        ok("h5_get_param() round-trips a stored parameter");
    }

    /* Parameter clamping. */
    status = h5_set_param((h5_context)ctx, H5_VOICE_KICK, H5_PARAM_TONE_PITCH,
        1 * H5_Q16_ONE);
    if (status != H5_ERR_RANGE ||
        ctx->voice_params[H5_VOICE_KICK][H5_PARAM_TONE_PITCH] != 10 * H5_Q16_ONE) {
        fail("h5_set_param() did not clamp a value below the minimum");
    } else {
        ok("h5_set_param() clamps values below the minimum");
    }

    status = h5_set_param((h5_context)ctx, H5_VOICE_KICK, H5_PARAM_TONE_PITCH,
        1000 * H5_Q16_ONE);
    if (status != H5_ERR_RANGE ||
        ctx->voice_params[H5_VOICE_KICK][H5_PARAM_TONE_PITCH] != 440 * H5_Q16_ONE) {
        fail("h5_set_param() did not clamp a value above the maximum");
    } else {
        ok("h5_set_param() clamps values above the maximum");
    }

    /* An out-of-range parameter is rejected. */
    status = h5_set_param((h5_context)ctx, H5_VOICE_KICK, H5_PARAM_COUNT, 0);
    if (status != H5_ERR_INVALID_PARAM) {
        fail("h5_set_param() did not reject an out-of-range parameter");
    } else {
        ok("h5_set_param() rejects an out-of-range parameter");
    }

    status = h5_get_param((h5_context)ctx, H5_VOICE_KICK, H5_PARAM_COUNT,
        &value);
    if (status != H5_ERR_INVALID_PARAM) {
        fail("h5_get_param() did not reject an out-of-range parameter");
    } else {
        ok("h5_get_param() rejects an out-of-range parameter");
    }

    /* h5_param_supported(). */
    if (!h5_param_supported(H5_VOICE_KICK, H5_PARAM_NOISE_PITCH)) {
        fail("h5_param_supported() wrongly reports Kick NoisePitch unsupported");
    } else {
        ok("h5_param_supported() accepts a valid pair");
    }
    if (!h5_param_supported(H5_VOICE_KICK, H5_PARAM_SENSITIVITY) ||
        !h5_param_supported(H5_VOICE_SNARE, H5_PARAM_SENSITIVITY) ||
        !h5_param_supported(H5_VOICE_TOM_1, H5_PARAM_SENSITIVITY) ||
        !h5_param_supported(H5_VOICE_TOM_2, H5_PARAM_SENSITIVITY) ||
        !h5_param_supported(H5_VOICE_TOM_3, H5_PARAM_SENSITIVITY) ||
        !metallic_param_supported(H5_VOICE_CYMBAL, H5_PARAM_SENSITIVITY) ||
        !metallic_param_supported(H5_VOICE_HIHAT, H5_PARAM_SENSITIVITY)) {
        fail("h5_param_supported() does not report Sensitivity on every voice");
    } else if (h5_get_param((h5_context)ctx, H5_VOICE_SNARE,
            H5_PARAM_SENSITIVITY, &value) != H5_OK ||
        value != H5_Q16_ONE) {
        fail("Sensitivity does not default to 1");
    } else {
        ok("Sensitivity is supported on every voice with the default 1");
    }
    if (h5_param_supported(H5_VOICE_COUNT, H5_PARAM_DECAY) ||
        h5_param_supported(H5_VOICE_KICK, H5_PARAM_COUNT)) {
        fail("h5_param_supported() did not reject out-of-range enums");
    } else {
        ok("h5_param_supported() rejects out-of-range enums");
    }

    /* Every standard drum voice shares the same six faceplate controls. */
    {
        static const h5_voice drum_voices[] = {
            H5_VOICE_KICK, H5_VOICE_SNARE,
            H5_VOICE_TOM_1, H5_VOICE_TOM_2, H5_VOICE_TOM_3
        };
        static const h5_param faceplate[] = {
            H5_PARAM_NOISE_PITCH, H5_PARAM_TONE_PITCH, H5_PARAM_BEND,
            H5_PARAM_DECAY, H5_PARAM_NOISE_TONE, H5_PARAM_CLICK_TONE
        };
        size_t vi;
        size_t pi;
        int missing = 0;

        for (vi = 0u; vi < sizeof(drum_voices) / sizeof(drum_voices[0]); vi++) {
            for (pi = 0u; pi < sizeof(faceplate) / sizeof(faceplate[0]); pi++) {
                if (!h5_param_supported(drum_voices[vi], faceplate[pi])) {
                    printf("      voice %d is missing param %d\n",
                        (int)drum_voices[vi], (int)faceplate[pi]);
                    missing++;
                }
            }
        }
        if (missing != 0) {
            fail("a standard drum voice is missing a common faceplate control");
        } else {
            ok("all standard drum voices expose the six faceplate controls");
        }
    }

    /* The metallic voices have their own faceplate: Sweep, Tone Pitch,
     * Bend, Decay, Noise/Tone and Bell/Cymbal. The drum controls Noise
     * and Click/Drum are not part of it. */
    {
        static const h5_voice metallic_voices[] = {
            H5_VOICE_CYMBAL, H5_VOICE_HIHAT
        };
        static const h5_param metallic_faceplate[] = {
            H5_PARAM_SWEEP, H5_PARAM_TONE_PITCH, H5_PARAM_BEND,
            H5_PARAM_DECAY, H5_PARAM_NOISE_TONE, H5_PARAM_BELL_CYMBAL
        };
        size_t vi;
        size_t pi;
        int missing = 0;

        for (vi = 0u; vi < sizeof(metallic_voices) / sizeof(metallic_voices[0]);
            vi++) {
            for (pi = 0u;
                pi < sizeof(metallic_faceplate) / sizeof(metallic_faceplate[0]);
                pi++) {
                if (!metallic_param_supported(metallic_voices[vi],
                        metallic_faceplate[pi])) {
                    missing++;
                }
            }
            if (metallic_param_supported(metallic_voices[vi],
                    H5_PARAM_NOISE_PITCH) ||
                metallic_param_supported(metallic_voices[vi],
                    H5_PARAM_CLICK_TONE)) {
                missing++;
            }
        }
        if (missing != 0) {
            fail("a metallic voice does not expose its own faceplate");
        } else {
            ok("the metallic voices expose their own six faceplate controls");
        }
    }

    /* Mixer set/get round trip and defaults. */
    status = h5_set_instrument_volume((h5_context)ctx, H5_INSTR_KICK,
        H5_Q16_ONE);
    if (status != H5_OK) {
        fail("h5_set_instrument_volume() did not return OK");
    }
    status = h5_get_instrument_volume((h5_context)ctx, H5_INSTR_KICK, &value);
    if (status != H5_OK || value != H5_Q16_ONE) {
        fail("h5_get_instrument_volume() did not return the stored value");
    } else {
        ok("h5_set_instrument_volume() / h5_get_instrument_volume() round trip");
    }

    status = h5_set_instrument_pan((h5_context)ctx, H5_INSTR_SNARE, 0);
    if (status != H5_OK) {
        fail("h5_set_instrument_pan() did not return OK");
    }
    status = h5_get_instrument_pan((h5_context)ctx, H5_INSTR_SNARE, &value);
    if (status != H5_OK || value != 0) {
        fail("h5_get_instrument_pan() did not return the stored value");
    } else {
        ok("h5_set_instrument_pan() / h5_get_instrument_pan() round trip");
    }

    /* The mixer controls are ordinary parameters: h5_set_param() writes
     * them and the instrument getters read them back. */
    status = h5_set_param((h5_context)ctx, H5_VOICE_SNARE, H5_PARAM_VOLUME,
        H5_Q16_ONE / 4);
    if (status != H5_OK) {
        fail("h5_set_param() did not accept the mixer Volume parameter");
    }
    status = h5_get_instrument_volume((h5_context)ctx, H5_INSTR_SNARE, &value);
    if (status != H5_OK || value != H5_Q16_ONE / 4) {
        fail("a volume written by h5_set_param() is not visible through"
            " h5_get_instrument_volume()");
    } else if (ctx->mix_left[H5_VOICE_SNARE] != H5_Q16_ONE / 4 ||
        ctx->mix_right[H5_VOICE_SNARE] != 0) {
        fail("h5_set_param() did not refresh the cached mixer gains");
    } else {
        ok("the mixer controls are settable via h5_set_param()");
    }

    /* Mixer clamping. */
    status = h5_set_instrument_volume((h5_context)ctx, H5_INSTR_TOM_1,
        -H5_Q16_ONE);
    if (status != H5_ERR_RANGE ||
        ctx->voice_params[H5_VOICE_TOM_1][H5_PARAM_VOLUME] != 0) {
        fail("h5_set_instrument_volume() did not clamp a negative value");
    } else {
        ok("h5_set_instrument_volume() clamps negative values to zero");
    }

    status = h5_set_instrument_pan((h5_context)ctx, H5_INSTR_TOM_3,
        2 * H5_Q16_ONE);
    if (status != H5_ERR_RANGE ||
        ctx->voice_params[H5_VOICE_TOM_3][H5_PARAM_PAN] != H5_Q16_ONE) {
        fail("h5_set_instrument_pan() did not clamp a value above one");
    } else {
        ok("h5_set_instrument_pan() clamps values above one");
    }

    /* h5_reset() restores mixer defaults. */
    h5_reset((h5_context)ctx);
    if (ctx->voice_params[H5_VOICE_KICK][H5_PARAM_VOLUME]
            != H5_Q16_ONE / 2 ||
        ctx->voice_params[H5_VOICE_SNARE][H5_PARAM_PAN] != H5_Q16_ONE / 2) {
        fail("h5_reset() did not restore mixer defaults");
    } else {
        ok("h5_reset() restores mixer defaults");
    }

    /* h5_reset() also refreshes the cached stereo gains derived from them. */
    {
        h5_voice v;
        int stale = 0;

        for (v = 0; v < H5_VOICE_COUNT; v++) {
            int32_t volume = ctx->voice_params[v][H5_PARAM_VOLUME];
            int32_t pan = ctx->voice_params[v][H5_PARAM_PAN];

            if (ctx->mix_left[v] != h5_mul_q16(volume, H5_Q16_ONE - pan) ||
                ctx->mix_right[v] != h5_mul_q16(volume, pan)) {
                stale++;
            }
        }
        if (stale != 0) {
            fail("h5_reset() left stale mixer gains");
        } else {
            ok("h5_reset() refreshes the mixer gains");
        }
    }

    /* h5_reset() restores parameter defaults. */
    if (ctx->voice_params[H5_VOICE_KICK][H5_PARAM_TONE_PITCH] !=
        40 * H5_Q16_ONE) {
        fail("h5_reset() did not restore Kick TonePitch default");
    } else {
        ok("h5_reset() restores parameter defaults");
    }

    /* The three Tom voices have independent parameter storage and distinct
     * default tunings. */
    if (ctx->voice_params[H5_VOICE_TOM_1][H5_PARAM_TONE_PITCH] <=
            ctx->voice_params[H5_VOICE_TOM_2][H5_PARAM_TONE_PITCH] ||
        ctx->voice_params[H5_VOICE_TOM_2][H5_PARAM_TONE_PITCH] <=
            ctx->voice_params[H5_VOICE_TOM_3][H5_PARAM_TONE_PITCH]) {
        fail("the three Tom voices do not have descending default tunings");
    } else {
        ok("the three Tom voices have distinct default tunings");
    }

    /* The three Tom voices have independent parameter storage: changing
     * Tom 1 must leave Tom 2's stored value untouched. */
    status = h5_get_param((h5_context)ctx, H5_VOICE_TOM_2,
        H5_PARAM_TONE_PITCH, &value);
    if (status != H5_OK) {
        fail("h5_get_param(Tom2) failed before the independence check");
    }

    status = h5_set_param((h5_context)ctx, H5_VOICE_TOM_1,
        H5_PARAM_TONE_PITCH, 300 * H5_Q16_ONE);
    if (status != H5_OK ||
        ctx->voice_params[H5_VOICE_TOM_1][H5_PARAM_TONE_PITCH] !=
            300 * H5_Q16_ONE ||
        ctx->voice_params[H5_VOICE_TOM_2][H5_PARAM_TONE_PITCH] != value) {
        fail("the Tom voices do not keep independent parameter storage");
    } else {
        ok("the Tom voices keep independent parameter storage");
    }

    /* Trigger validation. */
    if (h5_is_active((h5_context)ctx)) {
        fail("h5_is_active() reports activity before any trigger");
    } else {
        ok("h5_is_active() is false before any trigger");
    }

    status = h5_trigger((h5_context)ctx, H5_INSTR_KICK);
    if (status != H5_OK) {
        fail("h5_trigger(Kick) did not return OK");
    } else {
        ok("h5_trigger() accepts a valid instrument");
    }

    if (!h5_is_active((h5_context)ctx)) {
        fail("h5_is_active() does not report a triggered voice");
    } else {
        ok("h5_is_active() reports a triggered voice");
    }

    {
        size_t elapsed = 0u;
        const size_t limit = 3u * 48000u;

        while (h5_is_active((h5_context)ctx) && elapsed < limit) {
            h5_process((h5_context)ctx, block, 32u);
            elapsed += 32u;
        }
        if (h5_is_active((h5_context)ctx)) {
            fail("h5_is_active() still reports activity after the release");
        } else {
            ok("h5_is_active() returns to idle after the release");
        }
    }

    status = h5_trigger((h5_context)ctx, H5_INSTR_COUNT);
    if (status != H5_ERR_INVALID_INSTRUMENT) {
        fail("h5_trigger() did not reject an invalid instrument");
    } else {
        ok("h5_trigger() rejects an invalid instrument");
    }

    /* Triggering with a velocity. */
    status = h5_trigger_velocity((h5_context)ctx, H5_INSTR_KICK, 100u);
    if (status != H5_OK) {
        fail("h5_trigger_velocity() did not return OK");
    } else {
        ok("h5_trigger_velocity() accepts a valid velocity");
    }

    status = h5_trigger_velocity((h5_context)ctx, H5_INSTR_COUNT, 100u);
    if (status != H5_ERR_INVALID_INSTRUMENT) {
        fail("h5_trigger_velocity() did not reject an invalid instrument");
    } else {
        ok("h5_trigger_velocity() rejects an invalid instrument");
    }

    status = h5_trigger_velocity((h5_context)ctx, H5_INSTR_KICK, 200u);
    if (status != H5_ERR_RANGE) {
        fail("h5_trigger_velocity() did not reject a velocity above 127");
    } else {
        ok("h5_trigger_velocity() rejects a velocity above 127");
    }

    if (h5_trigger_velocity(0, H5_INSTR_KICK, 100u) != H5_ERR_INVALID_CONTEXT) {
        fail("h5_trigger_velocity(NULL, ...) did not return INVALID_CONTEXT");
    } else {
        ok("h5_trigger_velocity() rejects a NULL context");
    }

    /* The strike level of a hit follows the velocity and the voice's
     * sensitivity: at sensitivity 0 the velocity is ignored. */
    {
        int32_t full = h5_voice_strike_level(ctx, H5_VOICE_KICK, 127u);
        int32_t soft = h5_voice_strike_level(ctx, H5_VOICE_KICK, 32u);

        if (full != H5_Q16_ONE) {
            fail("the strike level of a full-velocity hit is not 1");
        } else if (soft <= 0 || soft >= full) {
            fail("a soft hit does not get a lower strike level");
        } else {
            ok("the strike level follows the velocity");
        }

        h5_set_param((h5_context)ctx, H5_VOICE_KICK, H5_PARAM_SENSITIVITY, 0);
        if (h5_voice_strike_level(ctx, H5_VOICE_KICK, 32u) != full) {
            fail("sensitivity 0 does not ignore the velocity");
        } else {
            ok("sensitivity 0 ignores the velocity");
        }

        h5_set_param((h5_context)ctx, H5_VOICE_KICK, H5_PARAM_SENSITIVITY,
            H5_Q16_ONE);
    }

    /* Null-context handling. */
    if (h5_set_param(0, H5_VOICE_KICK, H5_PARAM_DECAY, H5_Q16_ONE) !=
        H5_ERR_INVALID_CONTEXT) {
        fail("h5_set_param(NULL, ...) did not return INVALID_CONTEXT");
    } else {
        ok("h5_set_param() rejects a NULL context");
    }

    if (h5_get_instrument_volume(0, H5_INSTR_KICK, &value) !=
        H5_ERR_INVALID_CONTEXT) {
        fail("h5_get_instrument_volume(NULL, ...) did not return INVALID_CONTEXT");
    } else {
        ok("h5_get_instrument_volume() rejects a NULL context");
    }

    if (h5_trigger(0, H5_INSTR_KICK) != H5_ERR_INVALID_CONTEXT) {
        fail("h5_trigger(NULL, ...) did not return INVALID_CONTEXT");
    } else {
        ok("h5_trigger() rejects a NULL context");
    }

    if (h5_is_active(0)) {
        fail("h5_is_active(NULL) reports activity");
    } else {
        ok("h5_is_active(NULL) is false");
    }

    h5_destroy_context((h5_context)ctx);

    return failures == 0 ? 0 : 1;
}

static void usage(const char *program)
{
    printf("usage: %s <command>\n\n", program);
    printf("  --init             report the context size and default sample rate\n"
        "  --sine             verify the shared sine table, index by index\n"
        "  --dsp              verify the fixed-point DSP primitives\n"
        "  --silence [path]   render silence, check it and write a WAV file\n"
        "                     (default path: build/silence.wav)\n"
        "  --api              verify the context lifecycle, parameters,\n"
        "                     mixer, triggers and render entry\n"
        "  --kick             render a Kick hit and verify its controls\n"
        "  --tom              render a Tom hit and verify its controls\n"
        "  --snare            render a Snare hit and verify its controls\n"
        "  --hihat            render Hi-hat hits and verify open/closed\n"
        "  --cymbal           render a Cymbal hit and verify its controls\n"
        "  --mixer            verify volume, pan and the stereo output stage\n"
        "  --velocity         verify the velocity response and sensitivity\n"
        "  --fx               verify the reverb and noise gate\n"
        "  --help             this text\n");
}

int main(int argc, char **argv)
{
    const char *command;

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    command = argv[1];

    if (strcmp(command, "--init") == 0) {
        return command_init();
    }
    if (strcmp(command, "--sine") == 0) {
        return command_sine();
    }
    if (strcmp(command, "--dsp") == 0) {
        return command_dsp();
    }
    if (strcmp(command, "--silence") == 0) {
        const char *path = (argc > 2) ? argv[2] : "build/silence.wav";

        return command_silence(path);
    }
    if (strcmp(command, "--api") == 0) {
        return command_api();
    }
    if (strcmp(command, "--kick") == 0) {
        return command_kick();
    }
    if (strcmp(command, "--tom") == 0) {
        return command_tom();
    }
    if (strcmp(command, "--snare") == 0) {
        return command_snare();
    }
    if (strcmp(command, "--hihat") == 0) {
        return command_hihat();
    }
    if (strcmp(command, "--cymbal") == 0) {
        return command_cymbal();
    }
    if (strcmp(command, "--mixer") == 0) {
        return command_mixer();
    }

    if (strcmp(command, "--velocity") == 0) {
        return command_velocity();
    }
    if (strcmp(command, "--fx") == 0) {
        return command_fx();
    }
    if (strcmp(command, "--help") == 0 || strcmp(command, "-h") == 0) {
        usage(argv[0]);
        return 0;
    }

    printf("unknown command: %s\n\n", command);
    usage(argv[0]);
    return 1;
}
