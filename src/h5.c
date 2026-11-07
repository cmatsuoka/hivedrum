/*
 * h5.c - Libhive context lifecycle, trigger/mixer API and render entry point.
 *
 * This file provides the context lifecycle, the instrument-level wrappers and
 * the render entry point.
 */

#include "h5.h"
#include "h5_internal.h"

#include <stdlib.h>
#include <string.h>

/* Restore parameters and mixer values to their defaults. The sample rate is
 * left untouched; the caller is responsible for resetting any rate-dependent
 * runtime state. */
static void h5_init_defaults(struct h5_context_s *ctx)
{
    h5_voice v;
    h5_param p;

    for (v = 0; v < H5_VOICE_COUNT; v++) {
        for (p = 0; p < H5_PARAM_COUNT; p++) {
            ctx->voice_params[v][p] = h5_param_table[v][p].default_value;
        }
    }
    h5_fx_defaults(ctx);
    h5_mix_update_all(ctx);
}

h5_context h5_create_context(uint32_t sample_rate)
{
    struct h5_context_s *ctx =
        (struct h5_context_s *)calloc(1, sizeof(struct h5_context_s));

    if (ctx == 0) {
        return 0;
    }

    h5_sine_init();
    ctx->sample_rate = (sample_rate == 0u) ? H5_DEFAULT_SAMPLE_RATE
        : sample_rate;
    h5_init_defaults(ctx);
    h5_voice_setup_all(ctx);
    if (h5_fx_init(ctx) != 0) {
        free(ctx);
        return 0;
    }
    return (h5_context)ctx;
}

void h5_destroy_context(h5_context handle)
{
    struct h5_context_s *ctx = (struct h5_context_s *)handle;

    if (ctx == 0) {
        return;
    }
    h5_fx_free(ctx);
    free(ctx);
}

void h5_set_sample_rate(h5_context handle, uint32_t sample_rate)
{
    struct h5_context_s *ctx = (struct h5_context_s *)handle;

    if (ctx == 0) {
        return;
    }
    ctx->sample_rate = (sample_rate == 0u) ? H5_DEFAULT_SAMPLE_RATE
        : sample_rate;
    h5_voice_setup_all(ctx);
    h5_fx_clear(ctx);
    h5_fx_configure(ctx);
}

void h5_reset(h5_context handle)
{
    struct h5_context_s *ctx = (struct h5_context_s *)handle;

    if (ctx == 0) {
        return;
    }
    h5_init_defaults(ctx);
    h5_voice_setup_all(ctx);
    h5_fx_clear(ctx);
    h5_fx_configure(ctx);
}

void h5_process(h5_context handle, int16_t *stereo_out, size_t frames)
{
    struct h5_context_s *ctx = (struct h5_context_s *)handle;
    size_t i;

    if (stereo_out == 0) {
        return;
    }

    if (ctx == 0) {
        memset(stereo_out, 0, frames * 2u * sizeof(int16_t));
        return;
    }

    {
        h5_instrument v;

        for (v = 0; v < H5_INSTR_COUNT; v++) {
            ctx->voice_level[v] = 0;
        }
    }

    if (!h5_is_active(ctx)) {
        memset(stereo_out, 0, frames * 2u * sizeof(int16_t));
        return;
    }

    for (i = 0u; i < frames; i++) {
        int64_t left = 0;
        int64_t right = 0;
#if H5_FX_ENABLED
        int64_t fx_left = 0;
        int64_t fx_right = 0;
#endif
        h5_voice voice;

        /* Visit only the voices that are still sounding. Bits are cleared as
         * their engines finish, so a silent voice is skipped entirely and a
         * fully silent kit needs no per-frame mixing at all. */
        for (voice = 0; voice < H5_VOICE_COUNT; voice++) {
            uint32_t bit = (uint32_t)1u << (unsigned)voice;
            int32_t out;
            bool still_active;

            if ((ctx->active_mask & bit) == 0u) {
                continue;
            }
            out = h5_voice_process(ctx, voice, &still_active);
            if (out != 0) {
                int64_t level;
                int32_t volume;

                h5_mix_add(ctx, voice, out, &left, &right);

#if H5_FX_ENABLED
                /* The FX bus takes the voice output in the Q30 engine
                 * domain and uses the Q16 send gain directly. The mix bus
                 * doubles its contribution, so the bus does the same: both
                 * accumulators then live in the same Q31 domain, and the
                 * reverb's own gain staging starts from unity. */
                if ((ctx->fx_gain_left[voice] | ctx->fx_gain_right[voice])
                    != 0) {
                    fx_left += (int64_t)h5_mul_q16(out, ctx->fx_gain_left[voice]) * 2;
                    fx_right += (int64_t)h5_mul_q16(out, ctx->fx_gain_right[voice]) * 2;
                }
#endif

                volume = ctx->voice_params[voice][H5_PARAM_VOLUME];
                level = (int64_t)out * (int64_t)volume;
                if (level < 0) {
                    level = -level;
                }
                level = h5_shr_round(level, 30u);
                if (level > H5_Q16_ONE) {
                    level = H5_Q16_ONE;
                }
                if ((int32_t)level > ctx->voice_level[voice]) {
                    ctx->voice_level[voice] = (int32_t)level;
                }
                /* The two Hi-hat instruments share the voice, so they
                 * report the same level. Their indices differ by one. */
                if (voice == H5_VOICE_HIHAT
                    && (int32_t)level > ctx->voice_level[H5_INSTR_HIHAT_OPEN]) {
                    ctx->voice_level[H5_INSTR_HIHAT_OPEN] = (int32_t)level;
                }
            }
            if (!still_active) {
                ctx->active_mask &= ~bit;
            }
        }

#if H5_FX_ENABLED
        if (ctx->fx_any_send) {
            int32_t wet_l;
            int32_t wet_r;

            h5_fx_process(ctx, fx_left, fx_right, &wet_l, &wet_r);
            left += wet_l;
            right += wet_r;
        }
#endif

        stereo_out[2u * i] = h5_mix_to_q15(left);
        stereo_out[2u * i + 1u] = h5_mix_to_q15(right);

        /* The block is finished when no voice sounds and the FX bus has no
         * tail left. Without the bus there is no tail to wait for. */
        {
            bool finished = (ctx->active_mask == 0u);
#if H5_FX_ENABLED
            finished = finished && !(ctx->fx_any_send && h5_fx_busy(ctx));
#endif
            if (finished) {
                size_t remaining = frames - i - 1u;

                if (remaining > 0u) {
                    memset(&stereo_out[2u * (i + 1u)], 0,
                        remaining * 2u * sizeof(int16_t));
                }
                return;
            }
        }
    }
}

h5_status h5_set_instrument_volume(h5_context handle, h5_instrument instrument,
    int32_t volume_q16)
{
    if ((unsigned)instrument >= (unsigned)H5_INSTR_COUNT) {
        return H5_ERR_INVALID_INSTRUMENT;
    }
    return h5_set_param(handle, h5_voice_from_instrument(instrument),
        H5_PARAM_VOLUME, volume_q16);
}

h5_status h5_set_instrument_pan(h5_context handle, h5_instrument instrument,
    int32_t pan_q16)
{
    if ((unsigned)instrument >= (unsigned)H5_INSTR_COUNT) {
        return H5_ERR_INVALID_INSTRUMENT;
    }
    return h5_set_param(handle, h5_voice_from_instrument(instrument),
        H5_PARAM_PAN, pan_q16);
}

h5_status h5_get_instrument_volume(const h5_context handle,
    h5_instrument instrument,
    int32_t *out)
{
    if ((unsigned)instrument >= (unsigned)H5_INSTR_COUNT) {
        return H5_ERR_INVALID_INSTRUMENT;
    }
    return h5_get_param(handle, h5_voice_from_instrument(instrument),
        H5_PARAM_VOLUME, out);
}

h5_status h5_get_instrument_pan(const h5_context handle,
    h5_instrument instrument,
    int32_t *out)
{
    if ((unsigned)instrument >= (unsigned)H5_INSTR_COUNT) {
        return H5_ERR_INVALID_INSTRUMENT;
    }
    return h5_get_param(handle, h5_voice_from_instrument(instrument),
        H5_PARAM_PAN, out);
}

h5_status h5_get_instrument_levels(const h5_context handle, int32_t *levels)
{
    const struct h5_context_s *ctx = (const struct h5_context_s *)handle;
    size_t i;

    if (ctx == 0 || levels == 0) {
        return H5_ERR_INVALID_CONTEXT;
    }

    for (i = 0u; i < (size_t)H5_INSTR_COUNT; i++) {
        levels[i] = ctx->voice_level[i];
    }
    return H5_OK;
}

h5_status h5_trigger(h5_context handle, h5_instrument instrument)
{
    struct h5_context_s *ctx = (struct h5_context_s *)handle;

    if (ctx == 0) {
        return H5_ERR_INVALID_CONTEXT;
    }
    if ((unsigned)instrument >= (unsigned)H5_INSTR_COUNT) {
        return H5_ERR_INVALID_INSTRUMENT;
    }

    h5_voice_trigger_instrument_velocity(ctx, instrument, 127u);
    return H5_OK;
}

h5_status h5_trigger_velocity(h5_context handle, h5_instrument instrument,
    uint8_t velocity)
{
    struct h5_context_s *ctx = (struct h5_context_s *)handle;

    if (ctx == 0) {
        return H5_ERR_INVALID_CONTEXT;
    }
    if ((unsigned)instrument >= (unsigned)H5_INSTR_COUNT) {
        return H5_ERR_INVALID_INSTRUMENT;
    }
    if (velocity > 127u) {
        return H5_ERR_RANGE;
    }

    h5_voice_trigger_instrument_velocity(ctx, instrument, velocity);
    return H5_OK;
}

uint32_t h5_version_number(void)
{
    return H5_VERSION_NUMBER;
}
