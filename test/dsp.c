/*
 * dsp.c - fixed-point DSP layer checks for the Libhive test harness.
 *
 * Command --dsp verifies the shared primitives against libm: sine
 * interpolation, the per-voice noise generator, the linear AR envelope,
 * the semitone ratio table, the soft-clip shaper, the fixed-point
 * arithmetic helpers and the resonant filter core.
 *
 * Floating point is allowed here (this is the harness, not the library).
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h5.h"
#include "h5_internal.h"

#include "dsp.h"

#ifndef TEST_PI
#define TEST_PI 3.14159265358979323846
#endif

#define DSP_SR 44100u
#define DSP_SCALE (32767.0 * 65536.0)

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

static int iabsl(int32_t v)
{
    int64_t w = v;

    return (int)(w < 0 ? -w : w);
}

static int64_t iabs64(int64_t v)
{
    return v < 0 ? -v : v;
}

/* Test-only copies of helpers that the library no longer carries. */

static void test_noise_init(struct h5_noise_s *n, uint32_t seed,
    uint32_t sample_rate)
{
    uint32_t divider;

    if (seed == 0u) {
        seed = 1u;
    }
    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }
    divider = sample_rate / 48000u;
    if (divider == 0u) {
        divider = 1u;
    }
    n->x = seed;
    n->held = 0;
    n->divider = divider;
    n->phase = 0u;
}

static int32_t test_semitone_ratio(int semitones)
{
    if (semitones < -H5_SEMITONE_MAX) {
        semitones = -H5_SEMITONE_MAX;
    } else if (semitones > H5_SEMITONE_MAX) {
        semitones = H5_SEMITONE_MAX;
    }
    return h5_semitone[semitones + H5_SEMITONE_MAX];
}

static void test_svf_set(struct h5_svf_s *f, int mode, int32_t cutoff_q16,
    int32_t q_q16, uint32_t sample_rate)
{
    h5_svf_tune(f, mode, cutoff_q16, q_q16, sample_rate);
    h5_svf_reset(f);
}

static void check_fixed(void)
{
    int bad = 0;

    if (h5_sat_add_q31(INT32_MAX, 1) != INT32_MAX ||
        h5_sat_add_q31(INT32_MIN, -1) != INT32_MIN ||
        h5_sat_sub_q31(INT32_MIN, 1) != INT32_MIN ||
        h5_sat_add_q31(100, -200) != -100) {
        bad = 1;
    }
    if (h5_mul_q31(1 << 30, 1 << 30) != (1 << 29) ||
        h5_mul_q31((1 << 30), -(1 << 30)) != -(1 << 29) ||
        h5_mul_q31(3 << 29, 3 << 29) != 9 << 27 ||
        /* (1 - 2^-31)^2 rounds to 1 - 2^-30, one LSB below full scale. */
        h5_mul_q31(INT32_MAX, INT32_MAX) != INT32_MAX - 1) {
        bad = 1;
    }
    if (h5_mul_q30(12345, 1 << 30) != 12345 ||
        h5_mul_q30(1 << 30, 1 << 29) != (1 << 29)) {
        bad = 1;
    }
    if (h5_shr_round((int64_t)9 << 19, 20) != 5 ||
        h5_shr_round((int64_t)-9 * (1 << 19), 20) != -4 ||
        h5_shr_round((int64_t)5 << 20, 20) != 5) {
        bad = 1;
    }
    if (h5_sar32_round(5, 1) != 3 ||
        h5_sar32_round(-5, 1) != -2 ||
        h5_sar32_round(-1, 16) != 0 ||
        h5_sar32_round(65536, 16) != 1) {
        bad = 1;
    }
    if (h5_clip_i32_to_i16(40000) != 32767 ||
        h5_clip_i32_to_i16(-40000) != -32768 ||
        h5_q31_to_q15(65536) != 1 ||
        h5_q31_to_q15(98304) != 2 ||
        h5_q31_to_q15(INT32_MAX) != 32767 ||
        h5_q31_to_q15(INT32_MIN) != -32768) {
        bad = 1;
    }
    if (h5_cube_q30(1 << 30) != (1 << 30) ||
        h5_cube_q30(1 << 29) != (1 << 27) ||
        h5_cube_q30(-(1 << 29)) != -(1 << 27) ||
        h5_cube_q30(0) != 0) {
        bad = 1;
    }

    if (bad) {
        fail("the fixed-point helpers misbehave");
    } else {
        ok("the fixed-point helpers satisfy their contracts");
    }
}

static void check_sine_interp(void)
{
    h5_context ctx = h5_create_context(DSP_SR);
    unsigned int i;
    int bad = 0;
    int worst = 0;

    /* Exact at integer table indices, including the wrap at 1023. A table
     * step is 2^18 of the 28-bit phase. */
    for (i = 0; i < H5_SINE_TABLE_SIZE; i++) {
        int32_t got = h5_sine_interp(i << 18);

        if (got != (int32_t)h5_sine_at(i) * 65536) {
            if (bad < 4) {
                printf("      i=%u: got %d\n", i, got);
            }
            bad++;
        }
    }
    if (h5_sine_interp(256u << 18) != 2147418112) {
        bad++;
    }
    if (bad != 0) {
        fail("sine interpolation is inexact at integer indices");
    } else {
        ok("sine interpolation is exact at all 1024 integer indices");
    }

    /* Near the true sine at scattered fractional phases. */
    bad = 0;
    worst = 0;
    for (i = 0; i < 2048u; i++) {
        uint32_t p = (i * 7919u) & 0x0FFFFFFFu;
        double exact = sin(2.0 * TEST_PI * (double)p / 268435456.0);
        int32_t expected = (int32_t)floor(exact * DSP_SCALE + 0.5);
        int32_t got = h5_sine_interp(p);
        int err = got - expected;

        if (err < 0) {
            err = -err;
        }
        if (err > worst) {
            worst = err;
        }
        if (err > 50000) {
            if (bad < 4) {
                printf("      p=%u: got %d, expected %d\n", p, got, expected);
            }
            bad++;
        }
    }
    if (bad != 0) {
        printf("FAIL: %d of 2048 sine phases deviate (worst %d LSB)\n",
            bad, worst);
        failures++;
    } else {
        printf("ok  : sine interpolation tracks sin() (worst %d LSB)\n",
            worst);
    }

    h5_destroy_context(ctx);
}

static void check_noise(void)
{
    struct h5_noise_s a, b;
    const int n = 200000;
    double sum = 0.0, sum12 = 0.0, sum2 = 0.0, sum22 = 0.0;
    double minv = 1.0, maxv = -1.0, mean;
    int32_t prev = 0;
    int i;
    int bad = 0;

    test_noise_init(&a, 1u, DSP_SR);
    if (a.divider != 1u) {
        fail("the noise divider at 44100 is not 1");
        bad = 1;
    }
    test_noise_init(&b, 2u, 96000);
    if (b.divider != 2u) {
        fail("the noise divider at 96000 is not 2");
        bad = 1;
    }
    if (bad) {
        return;
    }

    /* Statistics for a 48 kHz stream (divider 1). */
    for (i = 0; i < n; i++) {
        double u = h5_noise_next(&a) / 2147483648.0;

        sum += u;
        if (u < minv) {
            minv = u;
        }
        if (u > maxv) {
            maxv = u;
        }
    }
    mean = sum / (double)n;
    if (minv > -0.8 || maxv < 0.8) {
        fail("the noise stream does not cover its range");
    } else if (mean > 0.014 || mean < -0.014) {
        printf("FAIL: noise mean is %.4f over %d samples\n", mean, n);
        failures++;
    } else {
        printf("ok  : noise covers [-1, 1) (mean %.4f)\n", mean);
    }

    /* Consecutive values differ at divider 1. */
    test_noise_init(&a, 1u, DSP_SR);
    bad = 0;
    for (i = 0; i < 1000; i++) {
        int32_t cur = h5_noise_next(&a);

        if (i > 0 && cur == prev) {
            bad++;
        }
        prev = cur;
    }
    if (bad != 0) {
        fail("the noise stream repeats values at divider 1");
    } else {
        ok("the noise stream changes value every sample at divider 1");
    }

    /* Hold pairs at divider 2: pairs (0,1), (2,3), ... are equal. The LCG
     * runs at the full rate and the value is sampled every second sample, so
     * the holds are X1, X1, X3, X3, ... */
    test_noise_init(&a, 1u, 96000);
    bad = 0;
    for (i = 0; i < 10000; i++) {
        int32_t cur = h5_noise_next(&a);

        if (i >= 1 && (i & 1) == 1 && cur != prev) {
            if (bad < 4) {
                printf("      i=%d: pair differs\n", i);
            }
            bad++;
        }
        prev = cur;
    }
    if (bad != 0) {
        fail("the noise stream does not hold pairs at divider 2");
    } else {
        ok("the noise stream holds each 48 kHz value for 2 samples at 96 kHz");
    }

    /* The LCG runs at the full sample rate and the output is decimated by
     * the divider. Check the exact sequence: seed 1, sr 96000, so the held
     * values are the LCG outputs at full-rate indices 0, 2, 4, ... */
    test_noise_init(&a, 1u, 96000);
    {
        uint32_t x = 1u;
        int32_t held = 0;

        bad = 0;
        for (i = 0; i < 1000; i++) {
            x = x * 1103515245u + 12345u;
            if ((i & 1) == 0) {
                held = (int32_t)x;
            }
            if (h5_noise_next(&a) != held) {
                bad++;
            }
        }
        if (bad != 0) {
            fail("the noise stream is not the decimated full-rate LCG");
        } else {
            ok("the noise stream matches the decimated full-rate LCG");
        }
    }

    /* Two seeds decorrelate. */
    test_noise_init(&a, 1u, DSP_SR);
    test_noise_init(&b, 0xF00DFFEEu, DSP_SR);
    for (i = 0; i < 100000; i++) {
        double u = h5_noise_next(&a) / 2147483648.0;
        double v = h5_noise_next(&b) / 2147483648.0;

        sum12 += u * v;
        sum2 += u * u;
        sum22 += v * v;
    }
    {
        double r = sum12 / sqrt(sum2 * sum22);

        if (r > 0.02 || r < -0.02) {
            printf("FAIL: distinct noise seeds correlate (r = %.4f)\n", r);
            failures++;
        } else {
            printf("ok  : distinct noise seeds decorrelate (r = %.4f)\n", r);
        }
    }

    /* The per-voice default seeds decorrelate too. */
    h5_noise_init_voice(&a, H5_VOICE_KICK, DSP_SR);
    h5_noise_init_voice(&b, H5_VOICE_SNARE, DSP_SR);
    sum12 = sum2 = sum22 = 0.0;
    for (i = 0; i < 100000; i++) {
        double u = h5_noise_next(&a) / 2147483648.0;
        double v = h5_noise_next(&b) / 2147483648.0;

        sum12 += u * v;
        sum2 += u * u;
        sum22 += v * v;
    }
    {
        double r = sum12 / sqrt(sum2 * sum22);

        if (r > 0.02 || r < -0.02) {
            printf("FAIL: default voice seeds correlate (r = %.4f)\n", r);
            failures++;
        } else {
            printf("ok  : default voice seeds decorrelate\n");
        }
    }
}

static void check_envelope(void)
{
    struct h5_env_s e;
    const int an_target = 22050;
    const int rn_target = 44100;
    int i;
    int bad;

    /* Times: truncation to samples. */
    h5_env_init(&e);
    h5_env_set_times(&e, 32768, 65536, DSP_SR);
    if (e.attack_len != an_target || e.release_len != rn_target) {
        printf("FAIL: envelope lengths %d/%d, expected %d/%d\n",
            e.attack_len, e.release_len, an_target, rn_target);
        failures++;
    } else {
        ok("envelope set_times truncates seconds to samples");
    }
    if (e.delta_a <= 0 || e.delta_r <= 0) {
        fail("envelope deltas are not positive");
    }

    /* Attack: linear to exactly 1.0. */
    bad = 0;
    h5_env_trigger(&e);
    for (i = 0; i < an_target; i++) {
        int32_t got = h5_env_next(&e);
        int64_t expected = ((int64_t)(i + 1) << 30) / an_target;

        if (iabs64(got - expected) > an_target / 2 + 4) {
            if (bad < 4) {
                printf("      k=%d: got %d, expected %d\n", i, got,
                    (int)expected);
            }
            bad++;
        }
    }
    if (bad != 0) {
        fail("the envelope attack is not linear");
    } else {
        ok("the envelope attack is linear to the sample");
    }
    if (e.value != (1 << 30)) {
        fail("the envelope peak is not exactly 1.0");
    } else {
        ok("the envelope peak is exactly 1.0 (Q30)");
    }

    /* Release: linear down to zero at an + rn samples. */
    bad = 0;
    for (i = 0; i < rn_target; i++) {
        int32_t got = h5_env_next(&e);
        int64_t expected = ((int64_t)(rn_target - 1 - i) << 30) / rn_target;

        if (i == rn_target - 1) {
            if (got != 0) {
                bad++;
            }
        } else if (iabs64(got - expected) > rn_target / 2 + 4) {
            if (bad < 4) {
                printf("      j=%d: got %d, expected %d\n", i, got,
                    (int)expected);
            }
            bad++;
        }
    }
    if (bad != 0) {
        fail("the envelope release is not linear");
    } else {
        ok("the envelope release is linear to zero");
    }
    if (e.state != H5_ENV_DONE) {
        fail("the envelope does not reach DONE after attack+release");
    } else {
        ok("the envelope reaches DONE after attack+release");
    }
    h5_env_next(&e);
    h5_env_next(&e);
    if (h5_env_next(&e) != 0) {
        fail("the envelope restarts output after DONE");
    } else {
        ok("a finished envelope stays silent");
    }

    /* Zero attack peaks on the first sample. */
    h5_env_set_times(&e, 0, 65536, DSP_SR);
    h5_env_trigger(&e);
    if (h5_env_next(&e) != (1 << 30)) {
        fail("a zero-attack envelope does not peak immediately");
    } else {
        ok("a zero-attack envelope peaks on the first sample");
    }

    /* Retrigger restarts. */
    h5_env_set_times(&e, 32768, 65536, DSP_SR);
    h5_env_trigger(&e);
    for (i = 0; i < 500; i++) {
        h5_env_next(&e);
    }
    h5_env_trigger(&e);
    if (h5_env_next(&e) != e.delta_a) {
        fail("a retriggered envelope does not restart at the attack delta");
    } else {
        ok("a retriggered envelope restarts from the beginning");
    }

    /* Changing the times mid-flight applies without resetting the phase. */
    h5_env_trigger(&e);
    for (i = 0; i < 100; i++) {
        h5_env_next(&e);
    }
    h5_env_set_times(&e, 0, 65536, DSP_SR);
    if (h5_env_next(&e) != (1 << 30)) {
        fail("mid-flight set_times did not clamp the attack to its end");
    } else {
        ok("mid-flight set_times applies without resetting the phase");
    }
}

static void check_semitone(void)
{
    int n;
    int bad = 0;

    if (test_semitone_ratio(0) != 65536) {
        bad = 1;
    }
    for (n = -H5_SEMITONE_MAX; n <= H5_SEMITONE_MAX; n++) {
        double exact = pow(2.0, n / 12.0) * 65536.0;
        int32_t expected = (int32_t)floor(exact + 0.5);

        if (test_semitone_ratio(n) != expected) {
            if (bad < 4) {
                printf("      n=%d: got %d, expected %d\n", n,
                    (int)test_semitone_ratio(n), (int)expected);
            }
            bad++;
        }
    }
    if (bad != 0) {
        fail("the semitone table disagrees with 2^(n/12)");
    } else {
        ok("the semitone table matches 2^(n/12) at every entry");
    }
    if (test_semitone_ratio(H5_SEMITONE_MAX + 100) !=
            test_semitone_ratio(H5_SEMITONE_MAX) ||
        test_semitone_ratio(-H5_SEMITONE_MAX - 100) !=
            test_semitone_ratio(-H5_SEMITONE_MAX)) {
        fail("the semitone ratio helper does not clamp out-of-range input");
    } else {
        ok("the semitone ratio helper clamps out-of-range input");
    }
}


static void check_pow07(void)
{
    int k;
    int worst = 0;
    int bad = 0;

    if (h5_pow07_q16(0) != 0 || h5_pow07_q16(H5_Q16_ONE) != H5_Q16_ONE) {
        fail("the x^0.7 mapping misses its endpoints");
    } else {
        ok("the x^0.7 mapping hits its endpoints");
    }

    for (k = 0; k <= 4096; k++) {
        int32_t x = (int32_t)(((int64_t)k * H5_Q16_ONE) / 4096);
        double exact = pow((double)x / (double)H5_Q16_ONE, 0.7)
            * (double)H5_Q16_ONE;
        int32_t expected = (int32_t)floor(exact + 0.5);
        int32_t got = h5_pow07_q16(x);
        int err = got - expected;

        if (err < 0) {
            err = -err;
        }
        if (err > worst) {
            worst = err;
        }
    }
    if (worst > 600) {
        printf("FAIL: x^0.7 mapping deviates (worst %d Q16 LSB)\n", worst);
        failures++;
    } else {
        printf("ok  : x^0.7 mapping tracks pow() (worst %d Q16 LSB)\n", worst);
    }
    (void)bad;
}

static void check_softclip(void)
{
    int k;
    int bad = 0;
    int worst = 0;
    int32_t prev = 0;

    if (h5_softclip_q31(0) != 0) {
        fail("the shaper does not pass zero");
    }

    /* Odd symmetry, ignoring the unrepresentable +full scale twin. */
    for (k = 1; k < 4096; k++) {
        int32_t m = (int32_t)((int64_t)k * 1048576 - 2147483648LL);
        int32_t neg, pos;

        neg = h5_softclip_q31(m);
        pos = h5_softclip_q31((int32_t)(-(int64_t)m));
        if (neg != -pos) {
            if (bad < 4) {
                printf("      m=%d: asym %d vs %d\n", m, neg, pos);
            }
            bad++;
        }
    }
    if (bad != 0) {
        fail("the shaper is not odd-symmetric");
    } else {
        ok("the shaper is odd-symmetric");
    }

    /* Monotone and close to tanh(1.5 * m) over the full int32 range, in
     * Q30 (1.0 = 1 << 30; the result peaks at tanh(1.5) ~= 0.905 and the
     * curve is held at that value as |mix| approaches 1.0). The table steps
     * the argument by 1/16, so the linear interpolation between the entries
     * leaves a worst-case error of a few 1/10000 of full scale. */
    bad = 0;
    worst = 0;
    for (k = 0; k < 4096; k++) {
        int32_t m = (int32_t)((int64_t)k * 1048576 - 2147483648LL);
        double exact = tanh(1.5 * (double)m / 2147483648.0);
        int32_t expected = (int32_t)(exact * 1073741824.0);
        int32_t got = h5_softclip_q31(m);
        int err = got - expected;

        if (k > 0 && got < prev) {
            if (bad < 4) {
                printf("      m=%d: not monotone (%d after %d)\n", m, got,
                    prev);
            }
            bad++;
        }
        prev = got;
        if (err < 0) {
            err = -err;
        }
        if (err > worst) {
            worst = err;
        }
    }
    if (worst <= 600000 && bad == 0) {
        printf("ok  : shaper tracks tanh(1.5*x) in Q30 (worst %d LSB)\n",
            worst);
    } else {
        printf("FAIL: shaper deviates from tanh(1.5*x) (worst %d LSB)\n",
            worst);
        failures++;
    }
}

static void check_filter(void)
{
    struct h5_svf_s f;
    const int32_t one_q16 = H5_Q16_ONE;
    const int32_t fc_q16 = 1000 * H5_Q16_ONE;
    const int32_t dc = 1 << 30;
    int i;
    int bad;
    double phase, step;

    /* LP DC gain converges to unity. The bound accounts for Q30
     * coefficient quantization: denominator 1 + a1 + a2 suffers the same
     * cancellation the exact filter cancels to zero, so the quantized DC
     * gain sits a few ten-thousandths of an LSB of Q16 off (measured 59). */
    test_svf_set(&f, H5_SVF_LP, fc_q16, one_q16, DSP_SR);
    for (i = 0; i < 4000; i++) {
        f.y1 = h5_svf_run(&f, dc);
    }
    if (iabs64(f.y1 - dc) > 256) {
        printf("FAIL: LP DC gain is off by %d LSB\n",
            (int)iabs64(f.y1 - dc));
        failures++;
    } else {
        ok("the LP DC gain is unity");
    }

    /* HP passes Nyquist at unity. */
    test_svf_set(&f, H5_SVF_HP, fc_q16, one_q16, DSP_SR);
    bad = 0;
    for (i = 0; i < 4000; i++) {
        int32_t y = h5_svf_run(&f, (i & 1) ? -(1 << 30) : (1 << 30));

        if (i >= 3984 && iabsl(y) < (1 << 30) - 8) {
            bad++;
        }
    }
    if (bad != 0) {
        fail("the HP Nyquist gain is not unity");
    } else {
        ok("the HP Nyquist gain is unity");
    }

    /* BP rejects DC and has ~unity gain at its center frequency. */
    test_svf_set(&f, H5_SVF_BP, fc_q16, one_q16, DSP_SR);
    for (i = 0; i < 4000; i++) {
        f.y1 = h5_svf_run(&f, dc);
    }
    if (iabsl(f.y1) > 8) {
        fail("the BP does not reject DC");
    } else {
        ok("the BP rejects DC");
    }
    phase = 0.0;
    step = 2.0 * TEST_PI * 1000.0 / (double)DSP_SR;
    for (i = 0; i < 20000; i++) {
        double s = sin(phase) * 134217728.0;

        f.y1 = h5_svf_run(&f, (int32_t)floor(s + 0.5));
        phase += step;
        if (phase >= 2.0 * TEST_PI) {
            phase -= 2.0 * TEST_PI;
        }
    }
    {
        int32_t peak = 0;

        for (i = 0; i < 2048; i++) {
            double s = sin(phase) * 134217728.0;
            int32_t y = h5_svf_run(&f, (int32_t)floor(s + 0.5));
            int32_t a = iabsl(y);

            phase += step;
            if (phase >= 2.0 * TEST_PI) {
                phase -= 2.0 * TEST_PI;
            }
            if (a > peak) {
                peak = a;
            }
        }
        if (peak < 107374182 || peak > 161061273) {
            printf("FAIL: BP center gain peaks at %d (expected ~134217728)\n",
                peak);
            failures++;
        } else {
            ok("the BP gain at its center frequency is unity");
        }
    }

    /* Stability and decay: impulse, then silence, over a wide grid. */
    bad = 0;
    {
        static const uint32_t rates[3] = { 44100u, 48000u, 96000u };
        static const int32_t cuts[7] = { 100, 900, 1600, 4000, 8000,
            12000, 16000 };
        static const int32_t qs[4] = { 1, 2, 4, 8 };
        const int32_t imp = 1 << 30;
        int rate_i, cut_i, q_i, mode;

        for (rate_i = 0; rate_i < 3; rate_i++) {
            for (cut_i = 0; cut_i < 7; cut_i++) {
                for (q_i = 0; q_i < 4; q_i++) {
                    for (mode = H5_SVF_LP; mode <= H5_SVF_HP; mode++) {
                        int32_t tail_peak = 0;

                        test_svf_set(&f, mode, cuts[cut_i] * H5_Q16_ONE,
                            qs[q_i] * H5_Q16_ONE, rates[rate_i]);
                        h5_svf_run(&f, imp);
                        for (i = 0; i < 50000; i++) {
                            int32_t y = h5_svf_run(&f, 0);

                            if (i >= 49500) {
                                int32_t a = iabsl(y);

                                if (a > tail_peak) {
                                    tail_peak = a;
                                }
                            }
                        }
                        if (tail_peak > 4096) {
                            if (bad < 4) {
                                printf("      fc=%d q=%d mode=%d sr=%u: "
                                    "tail peak %d\n",
                                    cuts[cut_i], qs[q_i], mode,
                                    rates[rate_i], tail_peak);
                            }
                            bad++;
                        }
                    }
                }
            }
        }
    }
    if (bad != 0) {
        fail("the filter core fails to decay over the sweep grid");
    } else {
        ok("the filter core decays to silence across the sweep grid");
    }
}

int command_dsp(void)
{
    check_fixed();
    check_sine_interp();
    check_noise();
    check_envelope();
    check_semitone();
    check_pow07();
    check_softclip();
    check_filter();

    return failures == 0 ? 0 : 1;
}