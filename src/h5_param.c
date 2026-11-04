/*
 * h5_param.c - public parameter tables, validation and getters/setters.
 *
 * The h5_param_table is the single source of truth for which parameter is
 * valid for which voice, and for the default values and clamping ranges.
 * Values are stored as Q16.16 physical units: Hz, seconds, signed semitones,
 * or a 0..1 scalar.
 */

#include "h5_internal.h"

#define Q H5_Q16_ONE

/* Helper for integer physical values. */
#define I(v) ((int32_t)(v) * Q)

/* Helper for fractional physical values using round-half-up. */
#define F(num, den) ((int32_t)(((int64_t)(num) * Q + ((den) / 2u)) / (den)))

/* All parameters of a disabled engine: unsupported and unclamped. */
#define OFF { H5_PARAM_UNSUPPORTED, H5_PARAM_UNSUPPORTED, H5_PARAM_UNSUPPORTED }

const struct h5_param_info h5_param_table[H5_VOICE_COUNT][H5_PARAM_COUNT] = {
    /* Factory sound defaults: each voice loads its stock tuning, so the
     * instrument sounds right with no host interaction. */

    [H5_VOICE_KICK] = {
        [H5_PARAM_NOISE_PITCH] = { I(900), I(16000), I(9000) },
        [H5_PARAM_TONE_PITCH]  = { I(10),  I(440),   I(40) },
        [H5_PARAM_BEND]        = { I(-62), I(62),    I(40) },
        [H5_PARAM_DECAY]       = { F(1, 10), F(2, 1), F(3, 10) },
        [H5_PARAM_NOISE_TONE]  = { 0,      Q,        F(95, 100) },
        [H5_PARAM_CLICK_TONE]  = { 0,      Q,        F(1, 2) },
        [H5_PARAM_VOLUME]      = { 0,      Q,        Q / 2 },
        [H5_PARAM_PAN]         = { 0,      Q,        Q / 2 },
        [H5_PARAM_SENSITIVITY] = { 0,      Q,        Q },
    },
    [H5_VOICE_SNARE] = {
        [H5_PARAM_NOISE_PITCH] = { I(900), I(14000), 581042176 },
        [H5_PARAM_TONE_PITCH]  = { I(28),  I(880),   6278349 },
        [H5_PARAM_BEND]        = { I(-62), I(62),    1271398 },
        [H5_PARAM_DECAY]       = { F(2, 10), F(5, 1), 38011 },
        [H5_PARAM_NOISE_TONE]  = { 0,      Q,        18350 },
        [H5_PARAM_CLICK_TONE]  = { 0,      Q,        51773 },
        [H5_PARAM_VOLUME]      = { 0,      Q,        38666 },
        [H5_PARAM_PAN]         = { 0,      Q,        32768 },
        [H5_PARAM_SENSITIVITY] = { 0,      Q,        Q },
    },
    [H5_VOICE_TOM_1] = {
        [H5_PARAM_NOISE_PITCH] = { I(900), I(16000), I(8377) },
        [H5_PARAM_TONE_PITCH]  = { I(28),  I(880),   F(1853, 10) },
        [H5_PARAM_BEND]        = { I(-62), I(62),    I(24) },
        [H5_PARAM_DECAY]       = { F(2, 10), F(5, 1), F(71, 100) },
        [H5_PARAM_NOISE_TONE]  = { 0,      Q,        F(13, 20) },
        [H5_PARAM_CLICK_TONE]  = { 0,      Q,        F(37, 50) },
        [H5_PARAM_VOLUME]      = { 0,      Q,        Q / 2 },
        [H5_PARAM_PAN]         = { 0,      Q,        F(3, 5) },
        [H5_PARAM_SENSITIVITY] = { 0,      Q,        Q },
    },
    [H5_VOICE_TOM_2] = {
        [H5_PARAM_NOISE_PITCH] = { I(900), I(16000), I(8398) },
        [H5_PARAM_TONE_PITCH]  = { I(28),  I(880),   F(1144, 10) },
        [H5_PARAM_BEND]        = { I(-62), I(62),    I(24) },
        [H5_PARAM_DECAY]       = { F(2, 10), F(5, 1), F(73, 100) },
        [H5_PARAM_NOISE_TONE]  = { 0,      Q,        F(13, 20) },
        [H5_PARAM_CLICK_TONE]  = { 0,      Q,        F(18, 25) },
        [H5_PARAM_VOLUME]      = { 0,      Q,        Q / 2 },
        [H5_PARAM_PAN]         = { 0,      Q,        Q / 2 },
        [H5_PARAM_SENSITIVITY] = { 0,      Q,        Q },
    },
    [H5_VOICE_TOM_3] = {
        [H5_PARAM_NOISE_PITCH] = { I(900), I(16000), I(8419) },
        [H5_PARAM_TONE_PITCH]  = { I(28),  I(880),   F(706, 10) },
        [H5_PARAM_BEND]        = { I(-62), I(62),    I(24) },
        [H5_PARAM_DECAY]       = { F(2, 10), F(5, 1), F(3, 4) },
        [H5_PARAM_NOISE_TONE]  = { 0,      Q,        F(13, 20) },
        [H5_PARAM_CLICK_TONE]  = { 0,      Q,        F(7, 10) },
        [H5_PARAM_VOLUME]      = { 0,      Q,        Q / 2 },
        [H5_PARAM_PAN]         = { 0,      Q,        F(2, 5) },
        [H5_PARAM_SENSITIVITY] = { 0,      Q,        Q },
    },
    /* The metallic voices: TONE_PITCH scales the metallic bank (0..1)
     * rather than a single frequency, so its range is 0..1. NOISE_PITCH is
     * the output low-pass cutoff. Bend is not a control on either voice:
     * the engine has no pitch sweep. Defaults follow the shipped preset. */
#ifdef LIBHIVE_ENGINE_CYMBAL
    [H5_VOICE_CYMBAL] = {
        [H5_PARAM_NOISE_PITCH] = { I(900), I(16000), I(9547) },
        [H5_PARAM_TONE_PITCH]  = { 0,      Q,        F(7, 10) },
        [H5_PARAM_BEND]        = OFF,
        [H5_PARAM_DECAY]       = { F(1, 10), F(3, 1), I(3) },
        [H5_PARAM_NOISE_TONE]  = { 0,      Q,        F(69, 100) },
        [H5_PARAM_CLICK_TONE]  = { 0,      Q,        F(14, 100) },
        [H5_PARAM_VOLUME]      = { 0,      Q,        Q / 2 },
        [H5_PARAM_PAN]         = { 0,      Q,        Q / 2 },
        [H5_PARAM_SENSITIVITY] = { 0,      Q,        Q },
    },
#else
    [H5_VOICE_CYMBAL] = {
        [H5_PARAM_NOISE_PITCH] = OFF,
        [H5_PARAM_TONE_PITCH]  = OFF,
        [H5_PARAM_BEND]        = OFF,
        [H5_PARAM_DECAY]       = OFF,
        [H5_PARAM_NOISE_TONE]  = OFF,
        [H5_PARAM_CLICK_TONE]  = OFF,
        [H5_PARAM_VOLUME]      = OFF,
        [H5_PARAM_PAN]         = OFF,
        [H5_PARAM_SENSITIVITY] = OFF,
    },
#endif
#ifdef LIBHIVE_ENGINE_HIHAT
    [H5_VOICE_HIHAT] = {
        [H5_PARAM_NOISE_PITCH] = { I(900), I(16000), I(16000) },
        [H5_PARAM_TONE_PITCH]  = { 0,      Q,        Q },
        [H5_PARAM_BEND]        = OFF,
        [H5_PARAM_DECAY]       = { F(1, 10), F(3, 1), F(28, 100) },
        [H5_PARAM_NOISE_TONE]  = { 0,      Q,        F(11, 100) },
        [H5_PARAM_CLICK_TONE]  = { 0,      Q,        F(9, 100) },
        [H5_PARAM_VOLUME]      = { 0,      Q,        F(49, 100) },
        [H5_PARAM_PAN]         = { 0,      Q,        Q / 2 },
        [H5_PARAM_SENSITIVITY] = { 0,      Q,        Q },
    },
#else
    [H5_VOICE_HIHAT] = {
        [H5_PARAM_NOISE_PITCH] = OFF,
        [H5_PARAM_TONE_PITCH]  = OFF,
        [H5_PARAM_BEND]        = OFF,
        [H5_PARAM_DECAY]       = OFF,
        [H5_PARAM_NOISE_TONE]  = OFF,
        [H5_PARAM_CLICK_TONE]  = OFF,
        [H5_PARAM_VOLUME]      = OFF,
        [H5_PARAM_PAN]         = OFF,
        [H5_PARAM_SENSITIVITY] = OFF,
    },
#endif
};

h5_status h5_set_param(h5_context handle, h5_voice voice, h5_param param,
    int32_t value)
{
    struct h5_context_s *ctx = (struct h5_context_s *)handle;
    const struct h5_param_info *info;
    int32_t clamped;
    h5_status status;

    if (ctx == 0) {
        return H5_ERR_INVALID_CONTEXT;
    }
    if ((unsigned)voice >= (unsigned)H5_VOICE_COUNT) {
        return H5_ERR_INVALID_VOICE;
    }
    if ((unsigned)param >= (unsigned)H5_PARAM_COUNT) {
        return H5_ERR_INVALID_PARAM;
    }

    info = &h5_param_table[voice][param];
    if (info->min == H5_PARAM_UNSUPPORTED) {
        return H5_ERR_UNSUPPORTED;
    }

    clamped = value;
    status = H5_OK;
    if (clamped < info->min) {
        clamped = info->min;
        status = H5_ERR_RANGE;
    } else if (clamped > info->max) {
        clamped = info->max;
        status = H5_ERR_RANGE;
    }

    ctx->voice_params[voice][param] = clamped;
    if (param == H5_PARAM_VOLUME || param == H5_PARAM_PAN) {
        h5_mix_update(ctx, voice);
    } else {
        h5_voice_configure(ctx, voice);
    }
    return status;
}

h5_status h5_get_param(const h5_context handle, h5_voice voice, h5_param param,
    int32_t *out)
{
    const struct h5_context_s *ctx = (const struct h5_context_s *)handle;
    const struct h5_param_info *info;

    if (ctx == 0 || out == 0) {
        return H5_ERR_INVALID_CONTEXT;
    }
    if ((unsigned)voice >= (unsigned)H5_VOICE_COUNT) {
        return H5_ERR_INVALID_VOICE;
    }
    if ((unsigned)param >= (unsigned)H5_PARAM_COUNT) {
        return H5_ERR_INVALID_PARAM;
    }

    info = &h5_param_table[voice][param];
    if (info->min == H5_PARAM_UNSUPPORTED) {
        return H5_ERR_UNSUPPORTED;
    }

    *out = ctx->voice_params[voice][param];
    return H5_OK;
}

bool h5_param_supported(h5_voice voice, h5_param param)
{
    if ((unsigned)voice >= (unsigned)H5_VOICE_COUNT) {
        return false;
    }
    if ((unsigned)param >= (unsigned)H5_PARAM_COUNT) {
        return false;
    }
    return h5_param_table[voice][param].min != H5_PARAM_UNSUPPORTED;
}
