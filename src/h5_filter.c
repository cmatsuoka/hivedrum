/*
 * h5_filter.c - second-order resonant filter core (SVF-style).
 *
 * One shared structure serves low-pass, band-pass and high-pass outputs:
 * each h5_svf_tune() call loads the numerator identity for one mode into
 * the same denominator. The filter is a direct-form I biquad with Q30
 * coefficients (a1 approaches -2 at low cutoff and high resonance, which
 * does not fit Q31), Q31 samples and 64-bit products.
 *
 * The static filters' coefficients are computed on parameter change only;
 * swept filters use the precomputed tables further down. The engine realizes
 * the same analog prototype as a TPT state-variable filter with
 * g = tan(pi*fc/fs); this direct-form biquad is the same bilinear transform
 * (the RBJ sin/cos form is algebraically identical for LP/HP/BP once the
 * cutoff is prewarped by tan(w/2)). Keeping the biquad form lets the
 * trigonometry come from the shared sine table through h5_sine_interp(),
 * never from libm. The numerator identities match LP b1 = 2*b0, b2 = b0;
 * HP b1 = -2*b0, b2 = b0; BP b1 = 0, b2 = -b0.
 *
 * The output shift rounds toward zero (magnitude truncation). Round-to-
 * nearest sustains low-amplitude zero-input limit cycles that grow as the
 * poles approach the unit circle (measured ~2^12 LSB at fc = 100 Hz,
 * Q = 2, 96 kHz); truncation toward zero decays to true silence.
 *
 * Numerical note: the sine table's peak is 32767, so the derived sine/cosine
 * carry a 30 ppm scale error versus the true unit circle. That is far below
 * tunable and is documented rather than compensated.
 */

#include "h5_internal.h"

void h5_svf_reset(struct h5_svf_s *f)
{
    f->x1 = 0;
    f->x2 = 0;
    f->y1 = 0;
    f->y2 = 0;
}

void h5_svf_tune(struct h5_svf_s *f, int mode, int32_t cutoff_q16,
    int32_t q_q16, uint32_t sample_rate)
{
    uint64_t cp, phase, max_cp;
    int64_t s, c, alpha, a0, pre, num;
    int32_t b0;

    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }

    if (q_q16 < H5_Q16_ONE / 2) {
        q_q16 = H5_Q16_ONE / 2;
    }
    if (q_q16 > 16 * H5_Q16_ONE) {
        q_q16 = 16 * H5_Q16_ONE;
    }

    /* Clamp the cutoff below Nyquist so the sine stays positive. Both
     * cutoffs are Q16 Hz: 0.49 * sample_rate in Q16. */
    cp = (cutoff_q16 < 0) ? 0u : (uint64_t)(uint32_t)cutoff_q16;
    max_cp = (uint64_t)sample_rate * 49u * 65536u / 100u;
    if (cp > max_cp) {
        cp = max_cp;
    }

    /* phase28 = cutoff / sample_rate * 2^28; cutoff is Q16. */
    phase = (cp << 12) / sample_rate;
    if (phase > 0x0FFFFFFEu) {
        phase = 0x0FFFFFFEu;
    }

    s = h5_sine_interp((uint32_t)phase);

    /* Derive cos(w) from the half-angle identity
     *     1 - cos(w) = 2 * sin^2(w/2).
     * Looking cos(w) up directly at the quarter turn lands on the sine
     * table's peak, where Q15 quantisation is coarser than 1-cos(w) at low
     * cutoffs. The half-angle sine is near zero there, so its interpolation
     * error is much smaller and the low-frequency poles stay accurate. */
    {
        int32_t sin_half = h5_sine_interp((uint32_t)(phase >> 1));
        int64_t one_minus_cos = 2 * h5_shr_round(
            (int64_t)sin_half * (int64_t)sin_half, 31u);

        c = ((int64_t)1 << 31) - one_minus_cos;
    }

    /* alpha = sin(w) / (2*Q), Q30. s is sin*2^31-scaled and Q is Q16, so
     * alpha_q30 = s * 2^14 / Q_q16. */
    alpha = ((int64_t)s << 14) / q_q16;
    a0 = ((int64_t)1 << 30) + alpha;

    f->a1 = (int32_t)(-(((int64_t)c * ((int64_t)1 << 30)) / a0));
    f->a2 = (int32_t)(((((int64_t)1 << 30) - alpha) << 30) / a0);

    switch (mode) {
    case H5_SVF_HP:
        pre = ((((int64_t)1 << 31) + c + 2) >> 2);
        num = (pre << 30) / a0;
        b0 = (int32_t)num;
        f->b0 = b0;
        f->mode = mode;
        break;
    case H5_SVF_BP:
        pre = alpha;
        num = (pre << 30) / a0;
        b0 = (int32_t)num;
        f->b0 = b0;
        f->mode = mode;
        break;
    default:
        pre = ((((int64_t)1 << 31) - c + 2) >> 2);
        num = (pre << 30) / a0;
        b0 = (int32_t)num;
        f->b0 = b0;
        f->mode = H5_SVF_LP;
        break;
    }
}

int32_t h5_svf_run(struct h5_svf_s *f, int32_t x)
{
    int64_t ff, fb;
    int32_t y;

    /* Feed-forward, expanded on the stored identities: LP/HP have b2 = b0
     * and b1 = +/-2*b0, BP has b1 = 0 and b2 = -b0. Three 32x32 -> 64
     * products avoid a multiply with a wider-than-32-bit operand, which
     * some small cores have to emulate. The sum is exact. */
    switch (f->mode) {
    case H5_SVF_HP:
        ff = h5_mulwide(f->b0, x)
            - 2 * h5_mulwide(f->b0, f->x1)
            + h5_mulwide(f->b0, f->x2);
        break;
    case H5_SVF_BP:
        ff = h5_mulwide(f->b0, x) - h5_mulwide(f->b0, f->x2);
        break;
    default:
        ff = h5_mulwide(f->b0, x)
            + 2 * h5_mulwide(f->b0, f->x1)
            + h5_mulwide(f->b0, f->x2);
        break;
    }
    fb = h5_mulwide(f->a1, f->y1)
        + h5_mulwide(f->a2, f->y2);
    y = h5_trunc_shift30(ff - fb);

    f->x2 = f->x1;
    f->x1 = x;
    f->y2 = f->y1;
    f->y1 = y;
    return y;
}
/*
 * Swept-filter coefficient tables.
 *
 * The cutoff-following filters change coefficients every sample, but their
 * cutoff moves smoothly. Recomputing tan/sin and the normalising divisions
 * on every sample is therefore wasteful. Instead, the configure path fills a
 * small table over the filter's actual cutoff range, and the audio path
 * looks up and interpolates the coefficients.
 *
 * The table is RAM, built from the live parameters and sample rate when they
 * change; nothing is added to the static image beyond these functions.
 */
void h5_sweep_build(struct h5_sweep_s *sw, int mode, int32_t q_q16,
    int32_t lo_q16, int32_t hi_q16, uint32_t sample_rate)
{
    struct h5_svf_s tmp;
    int64_t span;
    int i;

    sw->lo_q16 = lo_q16;
    sw->mode = mode;

    if (hi_q16 <= lo_q16) {
        sw->enabled = 0;
        h5_svf_tune(&tmp, mode, lo_q16, q_q16, sample_rate);
        sw->point[0].b0 = tmp.b0;
        sw->point[0].a1 = tmp.a1;
        sw->point[0].a2 = tmp.a2;
        return;
    }

    sw->enabled = 1;
    span = (int64_t)hi_q16 - lo_q16;
    sw->step_q32 = (int32_t)(((int64_t)(H5_SWEEP_POINTS - 1) << 32) / span);

    for (i = 0; i < H5_SWEEP_POINTS; i++) {
        int32_t fc = lo_q16
            + (int32_t)((span * i) / (H5_SWEEP_POINTS - 1));

        h5_svf_tune(&tmp, mode, fc, q_q16, sample_rate);
        sw->point[i].b0 = tmp.b0;
        sw->point[i].a1 = tmp.a1;
        sw->point[i].a2 = tmp.a2;
    }
}

void h5_sweep_apply(const struct h5_sweep_s *sw, int32_t cutoff_q16,
    struct h5_svf_s *f)
{
    const struct h5_sweep_point *p0, *p1;
    uint32_t pos, idx, frac;

    if (!sw->enabled) {
        p0 = &sw->point[0];
        f->b0 = p0->b0;
        f->a1 = p0->a1;
        f->a2 = p0->a2;
        f->mode = sw->mode;
        return;
    }

    if (cutoff_q16 < sw->lo_q16) {
        cutoff_q16 = sw->lo_q16;
    }
    pos = (uint32_t)h5_shr_round(
        h5_mulwide(cutoff_q16 - sw->lo_q16, sw->step_q32), 16u);
    if (pos >= ((uint32_t)(H5_SWEEP_POINTS - 1) << 16)) {
        idx = H5_SWEEP_POINTS - 2u;
        frac = 0xFFFFu;
    } else {
        idx = pos >> 16;
        frac = pos & 0xFFFFu;
    }
    p0 = &sw->point[idx];
    p1 = &sw->point[idx + 1u];

    if (frac == 0u) {
        f->b0 = p0->b0;
        f->a1 = p0->a1;
        f->a2 = p0->a2;
        f->mode = sw->mode;
        return;
    }
    f->b0 = p0->b0 + (int32_t)h5_shr_round(
        h5_mulwide(p1->b0 - p0->b0, (int32_t)frac), 16u);
    f->a1 = p0->a1 + (int32_t)h5_shr_round(
        h5_mulwide(p1->a1 - p0->a1, (int32_t)frac), 16u);
    f->a2 = p0->a2 + (int32_t)h5_shr_round(
        h5_mulwide(p1->a2 - p0->a2, (int32_t)frac), 16u);
    f->mode = sw->mode;
}
