/*
 * h5_fixed.h - inline fixed-point helpers for the Hive-V library.
 *
 * All helpers are integer-only. Rounding is round-half-up: exact halves
 * round toward positive infinity, the same convention the table generators
 * and the sine table use. Shift amounts must stay inside the documented
 * bounds; the helpers trust their inputs like the rest of the audio path.
 */

#ifndef HIVE_V_H5_FIXED_H
#define HIVE_V_H5_FIXED_H

#include <stdint.h>

/* Saturate a 64-bit intermediate into the int32_t range. */
static inline int32_t h5_sat_i32(int64_t v)
{
    if (v > INT32_MAX) {
        return INT32_MAX;
    }
    if (v < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)v;
}

/* Saturating add/sub for Q31 samples. */
static inline int32_t h5_sat_add_q31(int32_t a, int32_t b)
{
    return h5_sat_i32((int64_t)a + (int64_t)b);
}

static inline int32_t h5_sat_sub_q31(int32_t a, int32_t b)
{
    return h5_sat_i32((int64_t)a - (int64_t)b);
}

/* The wide-product seam for two 32-bit operands (Q31 or Q30 samples and
 * coefficients). Taking int32_t arguments is what lets a 32-bit core with a
 * single 32x32 -> 64 multiply instruction (UMULL/SMULL, MULHI) emit that
 * instruction instead of widening to a __muldi3 call. Only the top 30-31
 * bits of the 64-bit product are ever consumed by the shift helpers below.
 * If target measurement shows the multiply is still slow, redefine this one
 * helper with the core's high-multiply instruction (inline asm or
 * intrinsic) and nothing else changes. */
static inline int64_t h5_mulwide(int32_t a, int32_t b)
{
    return (int64_t)a * (int64_t)b;
}

/* Q31 x Q31 -> Q31, rounded to the nearest LSB and saturated. */
static inline int32_t h5_mul_q31(int32_t a, int32_t b)
{
    int64_t p = h5_mulwide(a, b) + ((int64_t)1 << 30);

    return h5_sat_i32(p >> 31);
}

/* Q31 sample x Q30 coefficient -> Q31, rounded and saturated. */
static inline int32_t h5_mul_q30(int32_t x, int32_t c30)
{
    int64_t p = h5_mulwide(x, c30) + ((int64_t)1 << 29);

    return h5_sat_i32(p >> 30);
}

/* Arithmetic right shift of a 64-bit value with round-half-up. The shift
 * must be 0..62. */
static inline int64_t h5_shr_round(int64_t v, unsigned shift)
{
    if (shift == 0u) {
        return v;
    }
    return (v + ((int64_t)1 << (shift - 1u))) >> shift;
}

/* int32 variant of h5_shr_round. The result never overflows int32 because
 * the magnitude shrinks; the rounding add runs in int64. */
static inline int32_t h5_sar32_round(int32_t v, unsigned shift)
{
    if (shift == 0u) {
        return v;
    }
    return (int32_t)((v + ((int64_t)1 << (shift - 1u))) >> shift);
}

/* Q16.16 value x Q16.16 factor -> Q16.16, truncated toward zero. Unlike
 * h5_mul_q16() this does not round, so a factor of exactly 1.0 leaves the
 * value unchanged. The strike-level path uses it so that a hit at full
 * velocity is bit-identical to a hit without the velocity scaling. */
static inline int32_t h5_scale_q16(int32_t x, int32_t factor_q16)
{
    int64_t p = h5_mulwide(x, factor_q16);

    if (p >= 0) {
        return h5_sat_i32(p >> 16);
    }
    return h5_sat_i32(-((-p) >> 16));
}

/* Q31 sample x Q16.16 control -> Q31, rounded and saturated. */
static inline int32_t h5_mul_q16(int32_t x, int32_t c16)
{
    return h5_sat_i32(h5_shr_round(h5_mulwide(x, c16), 16u));
}

/* Magnitude-truncating shift for Q30-coefficient sums: (acc >> 30) rounded
 * toward zero. Unlike round-to-nearest, truncation toward zero sustains no
 * zero-input limit cycle in a stable biquad; the filter loop uses it so
 * impulse tails decay to true silence. The per-sample cost is one compare.
 * The result is saturated: a resonant filter can legitimately push the
 * pre-shift sum beyond one, and that must clip rather than wrap. */
static inline int32_t h5_trunc_shift30(int64_t acc)
{
    int64_t q = acc >> 30;

    if (acc < 0 && (acc & (((int64_t)1 << 30) - 1)) != 0) {
        q++;
    }
    return h5_sat_i32(q);
}

/* Saturate an int32 into the int16 range. */
static inline int16_t h5_clip_i32_to_i16(int32_t v)
{
    if (v > 32767) {
        return 32767;
    }
    if (v < -32768) {
        return -32768;
    }
    return (int16_t)v;
}

/* Q31 -> Q15, round-half-up, saturated. */
static inline int16_t h5_q31_to_q15(int32_t v)
{
    return h5_clip_i32_to_i16(h5_sar32_round(v, 16u));
}

/* Cube of a Q30 value, result Q30. Used for the cubed AR filter envelope. */
static inline int32_t h5_cube_q30(int32_t v)
{
    int64_t sq = h5_shr_round((int64_t)v * (int64_t)v, 30u);

    return (int32_t)h5_shr_round(sq * (int64_t)v, 30u);
}

#endif /* HIVE_V_H5_FIXED_H */