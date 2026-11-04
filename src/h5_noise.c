/*
 * h5_noise.c - 32-bit LCG noise with a 48 kHz-target sample-and-hold.
 *
 * The noise primitive is:
 *
 *     X[n+1] = (1103515245 * X[n] + 12345) mod 2^32
 *     u[n]   = X[n] / 2^31                          signed, [-1, 1)
 *     D      = max(1, sample_rate / 48000)          integer division
 *
 * The LCG advances once per output sample; the value is sampled only when
 * the hold phase is zero (a sample-and-hold decimator). D is
 * the integer division sample_rate / 48000, so the held rate is
 * sample_rate / D: exactly 48 kHz only when the render rate is an integer
 * multiple of 48 kHz (48 kHz, 96 kHz, ...). At 44.1 kHz D is 1 and the held
 * rate is 44.1 kHz; at 88.2 kHz D is 1 as well (the quotient truncates),
 * not 48 kHz. Noise time is voice-local: it advances only while the voice is
 * processed.
 *
 * Every voice instance owns its own LCG state so the five noise streams stay
 * uncorrelated; each default seed is distinct and non-zero.
 */

#include "h5_internal.h"

#define H5_NOISE_MUL 1103515245u
#define H5_NOISE_INC 12345u
#define H5_NOISE_RATE 48000u

/* Arbitrary distinct non-zero seeds, one per voice instance. */
static const uint32_t h5_noise_seeds[H5_VOICE_COUNT] = {
    0x31F38A59u, 0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u,
    0x510E527Fu
};

static void h5_noise_setup(struct h5_noise_s *n, uint32_t seed,
    uint32_t sample_rate)
{
    uint32_t divider;

    if (seed == 0u) {
        seed = 1u;
    }
    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }

    divider = sample_rate / H5_NOISE_RATE;
    if (divider == 0u) {
        divider = 1u;
    }

    n->x = seed;
    n->held = 0;
    n->divider = divider;
    n->phase = 0u;
}

void h5_noise_init_voice(struct h5_noise_s *n, h5_voice voice,
    uint32_t sample_rate)
{
    if ((unsigned)voice >= (unsigned)H5_VOICE_COUNT) {
        voice = H5_VOICE_KICK;
    }
    h5_noise_setup(n, h5_noise_seeds[voice], sample_rate);
}

int32_t h5_noise_next(struct h5_noise_s *n)
{
    n->x = n->x * H5_NOISE_MUL + H5_NOISE_INC;
    /* At the rates where the hold divides exactly (44.1 kHz and up, the
     * divisor is 1), every sample is held; skip the hold-phase bookkeeping
     * entirely. */
    if (n->divider == 1u) {
        n->held = (int32_t)n->x;
        return n->held;
    }
    if (n->phase == 0u) {
        n->held = (int32_t)n->x;
    }
    n->phase = (n->phase + 1u == n->divider) ? 0u : n->phase + 1u;
    return n->held;
}