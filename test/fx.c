/*
 * fx.c - checks for the shared FX bus: reverb and noise gate.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h5.h"
#include "h5_internal.h"
#include "tests.h"

/* The FX bus is optional: the reverb behind HIVEDRUM_FX_REVERB and the
 * noise gate behind HIVEDRUM_FX_NOISE_GATE. The Makefile passes the same
 * definitions to the library and to the harness, so a trimmed build skips
 * the stages it cannot run instead of failing them. */
#if !H5_TEST_FX_REVERB

int command_fx(void)
{
    printf("skip: the FX bus is not compiled in\n");
    return 0;
}

#else

#define FX_RATE 44100u
#define FX_SECONDS 3u
#define FX_FRAMES (FX_RATE * FX_SECONDS)

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

static struct h5_context_s *fx_new(uint32_t rate)
{
    struct h5_context_s *ctx =
        (struct h5_context_s *)h5_create_context(rate);

    if (ctx == 0) {
        printf("FAIL: out of memory\n");
        exit(1);
    }
    return ctx;
}

static int32_t sample_abs(int16_t v)
{
    return (v < 0) ? -(int32_t)v : (int32_t)v;
}

#if H5_TEST_FX_GATE

static int32_t peak_abs(const int16_t *buffer, size_t begin, size_t end)
{
    int32_t peak = 0;
    size_t i;

    for (i = begin; i < end; i++) {
        int32_t l = sample_abs(buffer[2u * i]);
        int32_t r = sample_abs(buffer[2u * i + 1u]);

        if (l > peak) {
            peak = l;
        }
        if (r > peak) {
            peak = r;
        }
    }
    return peak;
}

#endif /* H5_TEST_FX_GATE */

static size_t last_above(const int16_t *buffer, size_t frames, int32_t level)
{
    size_t i = frames;

    while (i > 0u) {
        i--;
        if (sample_abs(buffer[2u * i]) > level
            || sample_abs(buffer[2u * i + 1u]) > level) {
            return i;
        }
    }
    return 0u;
}

static int16_t *fx_buffer(void)
{
    int16_t *buffer = (int16_t *)calloc((size_t)FX_FRAMES * 2u,
        sizeof(int16_t));

    if (buffer == 0) {
        printf("FAIL: out of memory\n");
        exit(1);
    }
    return buffer;
}

static void render_hits(struct h5_context_s *ctx, int16_t *buffer)
{
    h5_trigger((h5_context)ctx, H5_INSTR_KICK);
    h5_trigger((h5_context)ctx, H5_INSTR_SNARE);
    h5_process((h5_context)ctx, buffer, FX_FRAMES);
}

static int test_bit_identical(void)
{
    struct h5_context_s *ctx = fx_new(FX_RATE);
    int16_t *a = fx_buffer();
    int16_t *b = fx_buffer();
    int16_t *c = fx_buffer();
    int differ;
    size_t i;

    render_hits(ctx, a);

    h5_reset((h5_context)ctx);
    h5_set_instrument_fx_send((h5_context)ctx, H5_INSTR_KICK, H5_Q16_ONE / 2);
    h5_set_instrument_fx_send((h5_context)ctx, H5_INSTR_SNARE, H5_Q16_ONE / 2);
    render_hits(ctx, b);

    h5_reset((h5_context)ctx);
    render_hits(ctx, c);

    differ = 0;
    for (i = 0u; i < (size_t)FX_FRAMES * 2u; i++) {
        if (a[i] != b[i]) {
            differ = 1;
            break;
        }
    }
    if (!differ) {
        fail("FX send did not change the render");
    } else {
        ok("FX send changes the render");
    }

    if (memcmp(a, c, (size_t)FX_FRAMES * 2u * sizeof(int16_t)) != 0) {
        fail("FX bypass is not bit-identical to the dry render");
    } else {
        ok("FX bypass is bit-identical to the dry render");
    }

    free(a);
    free(b);
    free(c);
    h5_destroy_context((h5_context)ctx);
    return failures;
}

static int test_tail(void)
{
    struct h5_context_s *ctx = fx_new(FX_RATE);
    int16_t *wet = fx_buffer();
    int16_t *dry = fx_buffer();
    size_t wet_last;
    size_t dry_last;

    h5_set_fx_param((h5_context)ctx, H5_FX_ROOM_SIZE, H5_Q16_ONE);
    h5_set_fx_param((h5_context)ctx, H5_FX_DAMPING, 0);
    h5_set_fx_param((h5_context)ctx, H5_FX_WET, H5_Q16_ONE);
    h5_set_fx_param((h5_context)ctx, H5_FX_GATE_RANGE, 0);
    h5_set_instrument_fx_send((h5_context)ctx, H5_INSTR_SNARE, H5_Q16_ONE);
    h5_trigger((h5_context)ctx, H5_INSTR_SNARE);
    h5_process((h5_context)ctx, wet, FX_FRAMES);
    wet_last = last_above(wet, FX_FRAMES, 64);

    h5_reset((h5_context)ctx);
    h5_set_fx_param((h5_context)ctx, H5_FX_GATE_RANGE, 0);
    h5_trigger((h5_context)ctx, H5_INSTR_SNARE);
    h5_process((h5_context)ctx, dry, FX_FRAMES);
    dry_last = last_above(dry, FX_FRAMES, 64);

    if (wet_last <= dry_last + FX_RATE / 10u) {
        fail("the reverb send does not extend the tail");
    } else {
        ok("the reverb send adds a tail after the dry voice ends");
    }

    free(wet);
    free(dry);
    h5_destroy_context((h5_context)ctx);
    return failures;
}

/* The wet return of the bus, with the gate bypassed, against the dry
 * render of the same hit. The reverb is a send effect: its return has to
 * be loud enough to hear next to the voice that feeds it, and it has to
 * stay below it, or the bus would drown the kit. */
static int test_wet_level(void)
{
    struct h5_context_s *ctx = fx_new(FX_RATE);
    int16_t *wet = fx_buffer();
    int16_t *dry = fx_buffer();
    int32_t wet_peak;
    int32_t dry_peak;

    h5_set_fx_param((h5_context)ctx, H5_FX_ROOM_SIZE, H5_Q16_ONE);
    h5_set_fx_param((h5_context)ctx, H5_FX_WET, H5_Q16_ONE);
    h5_set_fx_param((h5_context)ctx, H5_FX_GATE_RANGE, 0);
    h5_set_instrument_fx_send((h5_context)ctx, H5_INSTR_SNARE, H5_Q16_ONE);
    h5_trigger((h5_context)ctx, H5_INSTR_SNARE);
    h5_process((h5_context)ctx, wet, FX_FRAMES);

    h5_reset((h5_context)ctx);
    h5_set_fx_param((h5_context)ctx, H5_FX_GATE_RANGE, 0);
    h5_trigger((h5_context)ctx, H5_INSTR_SNARE);
    h5_process((h5_context)ctx, dry, FX_FRAMES);

    /* The wet is the difference between the two renders: the bus adds its
     * return to the dry signal. */
    wet_peak = 0;
    dry_peak = 0;
    {
        size_t i;

        for (i = 0u; i < (size_t)FX_FRAMES * 2u; i++) {
            int32_t d = (int32_t)wet[i] - (int32_t)dry[i];
            int32_t v = sample_abs(d);

            if (v > wet_peak) {
                wet_peak = v;
            }
            v = sample_abs(dry[i]);
            if (v > dry_peak) {
                dry_peak = v;
            }
        }
    }

    /* At least a tenth of the dry level: the return has to be heard. */
    if (wet_peak < dry_peak / 10) {
        fail("the wet return is too quiet to hear");
    } else {
        ok("the wet return is loud enough to hear");
    }

    /* Below the dry level: the bus must not drown the kit. */
    if (wet_peak > dry_peak) {
        fail("the wet return is louder than the dry signal");
    } else {
        ok("the wet return stays below the dry signal");
    }

    free(wet);
    free(dry);
    h5_destroy_context((h5_context)ctx);
    return failures;
}

/* -40 dBFS in Q15 LSB. */
#define FX_MINUS_40_DBFS 328

/* Q31 unity gain. */
#define FX_GATE_UNITY ((int32_t)0x7FFFFFFF)

#if H5_TEST_FX_GATE

static void setup_gate(struct h5_context_s *ctx, int32_t range)
{
    h5_set_fx_param((h5_context)ctx, H5_FX_ROOM_SIZE, H5_Q16_ONE);
    h5_set_fx_param((h5_context)ctx, H5_FX_DAMPING, 0);
    h5_set_fx_param((h5_context)ctx, H5_FX_WET, H5_Q16_ONE);
    h5_set_fx_param((h5_context)ctx, H5_FX_GATE_HOLD,
        (H5_Q16_ONE * 2) / 10);
    h5_set_fx_param((h5_context)ctx, H5_FX_GATE_RELEASE,
        (H5_Q16_ONE * 2) / 100);
    h5_set_fx_param((h5_context)ctx, H5_FX_GATE_RANGE, range);
}

/* Drive the bus directly with a one-second low-frequency burst so the tail
 * is long enough to observe the gate on its own. */
static void render_fx_burst(struct h5_context_s *ctx, int16_t *out,
    size_t frames, size_t burst)
{
    size_t i;

    for (i = 0u; i < frames; i++) {
        int64_t in = 0;
        int32_t l;
        int32_t r;

        if (i < burst) {
            in = ((i / 220u) % 2u) ? 0x20000000 : -0x20000000;
        }
        h5_fx_process(ctx, in, in, &l, &r);
        out[2u * i] = h5_mix_to_q15(l);
        out[2u * i + 1u] = h5_mix_to_q15(r);
    }
}

static int test_gate(void)
{
    struct h5_context_s *ctx = fx_new(FX_RATE);
    int16_t *gated = fx_buffer();
    int16_t *open = fx_buffer();
    int32_t gated_peak;
    int32_t open_peak;
    size_t begin = (size_t)FX_RATE * 3u / 2u;
    size_t burst = FX_RATE;
    size_t i;
    int32_t max_step = 0;

    setup_gate(ctx, H5_Q16_ONE);
    render_fx_burst(ctx, gated, FX_FRAMES, burst);
    gated_peak = peak_abs(gated, begin, FX_FRAMES);

    if (gated_peak > FX_MINUS_40_DBFS) {
        fail("the gate does not close the tail below -40 dBFS");
    } else {
        ok("the gate closes the tail below -40 dBFS");
    }

    for (i = burst + 1u; i < (size_t)FX_FRAMES; i++) {
        int32_t dl = (int32_t)gated[2u * i]
            - (int32_t)gated[2u * (i - 1u)];
        int32_t dr = (int32_t)gated[2u * i + 1u]
            - (int32_t)gated[2u * (i - 1u) + 1u];

        if (dl < 0) {
            dl = -dl;
        }
        if (dr < 0) {
            dr = -dr;
        }

        if (dl > max_step) {
            max_step = dl;
        }
        if (dr > max_step) {
            max_step = dr;
        }
    }
    if (max_step >= 8192) {
        fail("the gated render has a click-sized step");
    } else {
        ok("the gated render has no click-sized steps");
    }

    h5_reset((h5_context)ctx);
    setup_gate(ctx, 0);
    render_fx_burst(ctx, open, FX_FRAMES, burst);
    open_peak = peak_abs(open, begin, FX_FRAMES);

    if (open_peak <= FX_MINUS_40_DBFS) {
        fail("the bypassed gate does not leave a louder tail");
    } else {
        ok("the bypassed gate leaves a louder tail");
    }

    free(gated);
    free(open);
    h5_destroy_context((h5_context)ctx);
    return failures;
}

/* A hit that lands while the gate is releasing must re-arm it. Otherwise the
 * release ramp keeps pulling the gain down and the new hit is attenuated for
 * the rest of the ramp. */
static int test_gate_retrigger(void)
{
    struct h5_context_s *ctx = fx_new(FX_RATE);
    size_t burst = (size_t)FX_RATE / 4u;
    size_t i;
    int seen_release = 0;

    setup_gate(ctx, H5_Q16_ONE);

    for (i = 0u; i < FX_FRAMES; i++) {
        int64_t in = 0;
        int32_t l;
        int32_t r;

        if (i < burst) {
            in = ((i / 220u) % 2u) ? 0x20000000 : -0x20000000;
        }
        h5_fx_process(ctx, in, in, &l, &r);

        if (ctx->fx.state == H5_FX_GATE_RELEASING) {
            seen_release = 1;
            break;
        }
    }

    if (!seen_release) {
        fail("the gate never entered its release ramp");
    } else {
        size_t window = (size_t)FX_RATE / 500u;
        int reopened = 0;

        for (i = 0u; i < window; i++) {
            int64_t in = ((i / 220u) % 2u) ? 0x20000000 : -0x20000000;
            int32_t l;
            int32_t r;

            h5_fx_process(ctx, in, in, &l, &r);
            if (ctx->fx.gate_gain == FX_GATE_UNITY) {
                reopened = 1;
                break;
            }
        }

        if (!reopened) {
            fail("a hit during release does not reopen the gate");
        } else {
            ok("a hit during release reopens the gate");
        }
    }

    h5_destroy_context((h5_context)ctx);
    return failures;
}

#endif /* H5_TEST_FX_GATE */

static int test_levels(void)
{
    struct h5_context_s *ctx = fx_new(FX_RATE);
    int16_t buffer[2048];
    int32_t levels[H5_INSTR_COUNT];

    h5_set_instrument_fx_send((h5_context)ctx, H5_INSTR_SNARE, H5_Q16_ONE);
    h5_trigger((h5_context)ctx, H5_INSTR_SNARE);
    h5_process((h5_context)ctx, buffer, 1024u);

    if (h5_get_instrument_levels((h5_context)ctx, levels) != H5_OK) {
        fail("h5_get_instrument_levels() failed with the FX bus on");
    } else if (levels[H5_INSTR_SNARE] <= 0) {
        fail("the snare level is not reported with the FX bus on");
    } else if (levels[H5_INSTR_HIHAT_OPEN]
        != levels[H5_INSTR_HIHAT_CLOSED]) {
        fail("the shared Hi-hat levels diverged");
    } else {
        ok("per-instrument levels still work with the FX bus on");
    }

    h5_destroy_context((h5_context)ctx);
    return failures;
}

static int test_ram(void)
{
    struct h5_context_s *ctx = fx_new(FX_RATE);
    unsigned long bytes = (unsigned long)ctx->fx.block_size;

    printf("fx delay block     : %lu bytes\n", bytes);
    if (bytes == 0ul || bytes > 110000ul) {
        fail("the FX delay block is outside the size budget");
    } else {
        ok("the FX delay block fits the size budget");
    }

    h5_destroy_context((h5_context)ctx);
    return failures;
}

static int test_invalid(void)
{
    struct h5_context_s *ctx = fx_new(FX_RATE);
#if H5_TEST_FX_GATE
    int32_t value = 0;
#endif

    if (h5_set_fx_param((h5_context)ctx, H5_FX_PARAM_COUNT, 0)
        != H5_ERR_INVALID_PARAM) {
        fail("h5_set_fx_param() did not reject an invalid parameter");
    } else {
        ok("h5_set_fx_param() rejects an invalid parameter");
    }

    if (h5_set_fx_param(0, H5_FX_WET, 0) != H5_ERR_INVALID_CONTEXT) {
        fail("h5_set_fx_param(NULL, ...) did not return INVALID_CONTEXT");
    } else {
        ok("h5_set_fx_param() rejects a NULL context");
    }

#if H5_TEST_FX_GATE
    if (h5_set_fx_param((h5_context)ctx, H5_FX_GATE_THRESHOLD,
        H5_Q16_ONE * 2) != H5_ERR_RANGE) {
        fail("h5_set_fx_param() did not clamp an out-of-range threshold");
    } else if (h5_get_fx_param((h5_context)ctx, H5_FX_GATE_THRESHOLD,
        &value) != H5_OK || value != H5_Q16_ONE) {
        fail("the clamped threshold was not stored");
    } else {
        ok("h5_set_fx_param() clamps and stores the threshold");
    }

    if (h5_set_fx_param((h5_context)ctx, H5_FX_GATE_HOLD,
        H5_Q16_ONE * 10) != H5_ERR_RANGE) {
        fail("h5_set_fx_param() did not clamp an out-of-range hold time");
    } else if (h5_get_fx_param((h5_context)ctx, H5_FX_GATE_HOLD,
        &value) != H5_OK || value != H5_Q16_ONE * 5) {
        fail("the clamped hold time was not stored");
    } else {
        ok("h5_set_fx_param() clamps and stores the hold time");
    }
#endif

    if (h5_get_fx_param((h5_context)ctx, H5_FX_WET, 0)
        != H5_ERR_INVALID_PARAM) {
        fail("h5_get_fx_param() did not reject a NULL out pointer");
    } else {
        ok("h5_get_fx_param() rejects a NULL out pointer");
    }

    h5_destroy_context((h5_context)ctx);
    return failures;
}

static int test_rate_change(void)
{
    struct h5_context_s *ctx = fx_new(44100u);
    int16_t buffer[4096];
    size_t i;

    h5_set_fx_param((h5_context)ctx, H5_FX_ROOM_SIZE, H5_Q16_ONE);
    h5_set_instrument_fx_send((h5_context)ctx, H5_INSTR_SNARE, H5_Q16_ONE);
    h5_set_sample_rate((h5_context)ctx, 48000u);
    h5_trigger((h5_context)ctx, H5_INSTR_SNARE);
    for (i = 0u; i < 8u; i++) {
        h5_process((h5_context)ctx, buffer, 2048u);
    }
    h5_set_sample_rate((h5_context)ctx, 44100u);
    h5_trigger((h5_context)ctx, H5_INSTR_SNARE);
    h5_process((h5_context)ctx, buffer, 2048u);

    ok("the FX bus survives a sample-rate change");
    h5_destroy_context((h5_context)ctx);
    return failures;
}

int command_fx(void)
{
    test_bit_identical();
    test_tail();
    test_wet_level();
#if H5_TEST_FX_GATE
    test_gate();
    test_gate_retrigger();
#else
    printf("skip: the noise gate is not compiled in\n");
#endif
    test_levels();
    test_ram();
    test_invalid();
    test_rate_change();

    return failures == 0 ? 0 : 1;
}

#endif /* !H5_TEST_FX_REVERB */
