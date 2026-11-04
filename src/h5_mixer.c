/*
 * h5_mixer.c - per-voice volume/pan and stereo output conversion.
 *
 * Each voice produces a mono Q30 sample (1.0 = 1 << 30; the engines keep one
 * headroom bit for the soft-clip shaper's overshoot, so hot values may
 * exceed 1.0). The per-voice stereo gains (volume folded with pan) are
 * computed when the controls change, so the mixer only scales each voice once
 * per channel, converts to the Q31 accumulator domain by doubling, adds the
 * contribution to the two 64-bit channel accumulators, and converts the
 * completed mix to Q15 once per frame.
 */

#include "h5_internal.h"

void h5_mix_update(struct h5_context_s *ctx, h5_voice voice)
{
    const int32_t volume = ctx->voice_params[voice][H5_PARAM_VOLUME];
    const int32_t pan = ctx->voice_params[voice][H5_PARAM_PAN];

    ctx->mix_left[voice] = h5_mul_q16(volume, H5_Q16_ONE - pan);
    ctx->mix_right[voice] = h5_mul_q16(volume, pan);
}

void h5_mix_update_all(struct h5_context_s *ctx)
{
    h5_voice voice;

    for (voice = 0; voice < H5_VOICE_COUNT; voice++) {
        h5_mix_update(ctx, voice);
    }
}

void h5_mix_add(const struct h5_context_s *ctx, h5_voice voice,
    int32_t voice_out, int64_t *left, int64_t *right)
{
    *left += (int64_t)h5_mul_q16(voice_out, ctx->mix_left[voice]) * 2;
    *right += (int64_t)h5_mul_q16(voice_out, ctx->mix_right[voice]) * 2;
}

int16_t h5_mix_to_q15(int64_t sum)
{
    int64_t rounded = h5_shr_round(sum, 16u);

    if (rounded > 32767) {
        return 32767;
    }
    if (rounded < -32768) {
        return -32768;
    }
    return (int16_t)rounded;
}