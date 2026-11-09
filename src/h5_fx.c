/*
 * h5_fx.c - shared FX bus: a stereo reverb and a stereo-linked noise gate.
 *
 * Voices feed the bus through their FX send. The reverb is a bank of damped
 * comb lines summed into four series allpass diffusers per channel. The gate
 * keys off the pre-reverb bus input and applies one shared gain to both
 * channels. The delay memory is a single block sized once for 48 kHz; active
 * line lengths follow the sample rate and the room size, so the bus never
 * allocates after context creation.
 *
 * The two effects are optional. HIVEDRUM_FX_REVERB compiles the reverb and
 * the delay memory it works on; HIVEDRUM_FX_NOISE_GATE compiles the gate
 * that shapes the reverb's return. The gate acts on the reverb's return, so
 * it needs the reverb and cannot be built without it. Without the reverb the
 * bus has no wet signal at all, so the send parameter and the FX parameters
 * report themselves unsupported and the bus renders silence.
 */

#include "h5_internal.h"

#include <stdlib.h>
#include <string.h>

#define H5_FX_INPUT_GAIN_Q31 ((int32_t)0x7FFFFFFF) /* 1.0 */
#define H5_FX_FB_Q31 ((int32_t)1804886016)         /* 0.84, round half up */
#define H5_FX_FULL ((int32_t)0x7FFFFFFF)           /* Q31 unity */
#define H5_FX_BUSY_LEVEL ((int32_t)(1 << 20))      /* ~ -66 dBFS */

#define Q H5_Q16_ONE

#define I(v) ((int32_t)(v) * Q)
#define F(num, den) ((int32_t)(((int64_t)(num) * Q + ((den) / 2u)) / (den)))

/* A parameter of an effect that is not compiled in is unsupported: its
 * minimum is the unsupported marker, so the setter and the getter report
 * H5_ERR_UNSUPPORTED and the value is not stored. */
#if H5_FX_REVERB
#define FX_REVERB_PARAM(min, max, def) { (min), (max), (def) }
#else
#define FX_REVERB_PARAM(min, max, def) \
    { H5_PARAM_UNSUPPORTED, H5_PARAM_UNSUPPORTED, 0 }
#endif

#if H5_FX_NOISE_GATE
#define FX_GATE_PARAM(min, max, def) { (min), (max), (def) }
#else
#define FX_GATE_PARAM(min, max, def) \
    { H5_PARAM_UNSUPPORTED, H5_PARAM_UNSUPPORTED, 0 }
#endif

static const struct h5_param_info h5_fx_param_table[H5_FX_PARAM_COUNT] = {
    [H5_FX_ROOM_SIZE] = FX_REVERB_PARAM(0, Q, F(1, 2)),
    [H5_FX_DAMPING] = FX_REVERB_PARAM(0, Q, F(35, 100)),
    [H5_FX_WIDTH] = FX_REVERB_PARAM(0, Q, F(8, 10)),
    [H5_FX_WET] = FX_REVERB_PARAM(0, Q, F(1, 2)),
    [H5_FX_GATE_THRESHOLD] = FX_GATE_PARAM(0, Q, F(3, 100)),
    [H5_FX_GATE_ATTACK] = FX_GATE_PARAM(0, I(5), F(5, 1000)),
    [H5_FX_GATE_HOLD] = FX_GATE_PARAM(0, I(5), F(30, 100)),
    [H5_FX_GATE_RELEASE] = FX_GATE_PARAM(0, I(5), F(6, 100)),
    [H5_FX_GATE_RANGE] = FX_GATE_PARAM(0, Q, Q),
};

#if H5_FX_REVERB

/* Comb and allpass tuning at 44.1 kHz, in samples. */
static const uint16_t h5_fx_comb_base[H5_FX_COMBS] = {
    1116u, 1188u, 1277u, 1356u, 1422u, 1491u, 1557u, 1617u
};

static const uint16_t h5_fx_allpass_base[H5_FX_ALLPASSES] = {
    556u, 441u, 341u, 225u
};

static uint32_t h5_fx_comb_alloc(uint32_t base)
{
    return (uint32_t)(((uint64_t)base * 2u * 48000u + 44100u - 1u) / 44100u);
}

static uint32_t h5_fx_allpass_alloc(uint32_t base)
{
    return (uint32_t)(((uint64_t)base * 48000u + 44100u - 1u) / 44100u);
}

static int32_t h5_q16_to_q31_full(int32_t v)
{
    const int64_t t = (int64_t)v << 15;

    if (t > (int64_t)H5_FX_FULL) {
        return H5_FX_FULL;
    }
    if (t < 0) {
        return 0;
    }
    return (int32_t)t;
}

static int32_t h5_fx_mag(int32_t v)
{
    int64_t m = (v < 0) ? -(int64_t)v : (int64_t)v;

    return (m > INT32_MAX) ? INT32_MAX : (int32_t)m;
}

void h5_fx_defaults(struct h5_context_s *ctx)
{
    h5_fx_param p;

    for (p = 0; p < H5_FX_PARAM_COUNT; p++) {
        if (h5_fx_param_table[p].min != H5_PARAM_UNSUPPORTED) {
            ctx->fx_params[p] = h5_fx_param_table[p].default_value;
        }
    }
}

int h5_fx_init(struct h5_context_s *ctx)
{
    size_t total = 0u;
    int ch;
    int line;
    int16_t *cursor;

    /* The two channels have the same line lengths, so each line's size is
     * counted twice instead of walking the per-channel tables. */
    for (line = 0; line < H5_FX_COMBS; line++) {
        total += 2u * (size_t)h5_fx_comb_alloc(h5_fx_comb_base[line]);
    }
    for (line = 0; line < H5_FX_ALLPASSES; line++) {
        total += 2u * (size_t)h5_fx_allpass_alloc(h5_fx_allpass_base[line]);
    }

    ctx->fx.block = (int16_t *)calloc(total, sizeof(int16_t));
    if (ctx->fx.block == 0) {
        return -1;
    }
    ctx->fx.block_size = total * sizeof(int16_t);

    cursor = ctx->fx.block;
    for (ch = 0; ch < 2; ch++) {
        for (line = 0; line < H5_FX_COMBS; line++) {
            struct h5_comb_s *c = &ctx->fx.comb[ch][line];

            c->alloc = h5_fx_comb_alloc(h5_fx_comb_base[line]);
            c->pos = 0u;
            c->lp = 0;
            c->line = cursor;
            cursor += c->alloc;
        }
    }
    for (ch = 0; ch < 2; ch++) {
        for (line = 0; line < H5_FX_ALLPASSES; line++) {
            struct h5_allpass_s *a = &ctx->fx.allpass[ch][line];

            a->alloc = h5_fx_allpass_alloc(h5_fx_allpass_base[line]);
            a->pos = 0u;
            a->line = cursor;
            cursor += a->alloc;
        }
    }

    h5_fx_configure(ctx);
    return 0;
}

void h5_fx_free(struct h5_context_s *ctx)
{
    free(ctx->fx.block);
    ctx->fx.block = 0;
    ctx->fx.block_size = 0u;
}

void h5_fx_configure(struct h5_context_s *ctx)
{
    const int32_t room = ctx->fx_params[H5_FX_ROOM_SIZE];
    const uint32_t rate = ctx->sample_rate;
    int32_t scale_q16;
    int ch;
    int line;

    if (ctx->fx.block == 0) {
        return;
    }

    /* The room size scales the comb lines from 0.6x to 2.0x. The lengths
     * depend on the line index only, so they are held once per line rather
     * than once per line and channel. */
    scale_q16 = 39321 + (int32_t)(((int64_t)room * 91750) >> 16);
    for (line = 0; line < H5_FX_COMBS; line++) {
        int64_t len = (int64_t)h5_fx_comb_base[line] * scale_q16
            * (int64_t)rate;

        len /= (65536LL * 44100LL);
        if (len < 16) {
            len = 16;
        }
        if (len > (int64_t)ctx->fx.comb[0][line].alloc) {
            len = (int64_t)ctx->fx.comb[0][line].alloc;
        }
        ctx->fx.comb_len[line] = (uint32_t)len;
    }
    for (line = 0; line < H5_FX_ALLPASSES; line++) {
        int64_t len = (int64_t)h5_fx_allpass_base[line] * (int64_t)rate
            / 44100LL;

        if (len < 16) {
            len = 16;
        }
        if (len > (int64_t)ctx->fx.allpass[0][line].alloc) {
            len = (int64_t)ctx->fx.allpass[0][line].alloc;
        }
        ctx->fx.allpass_len[line] = (uint32_t)len;
        for (ch = 0; ch < 2; ch++) {
            if (ctx->fx.allpass[ch][line].pos >= ctx->fx.allpass_len[line]) {
                ctx->fx.allpass[ch][line].pos = 0u;
            }
        }
    }

    /* The damping coefficient is the one-pole's share of the new sample:
     * the larger it is, the closer the filter follows the comb's output
     * and the brighter the tail stays. The parameter therefore runs the
     * other way round: damping 0 is the bright tail, damping 1 the dark
     * one, as the header says. */
    ctx->fx.damp_q16 = 49152
        - (int32_t)(((int64_t)ctx->fx_params[H5_FX_DAMPING] * 39322) >> 16);

    ctx->fx.width_q31 = h5_q16_to_q31_full(ctx->fx_params[H5_FX_WIDTH]);
    ctx->fx.wet_q31 = h5_q16_to_q31_full(ctx->fx_params[H5_FX_WET]);

#if H5_FX_NOISE_GATE
    ctx->fx.thr_open_q31 = h5_q16_to_q31_full(
        ctx->fx_params[H5_FX_GATE_THRESHOLD]);
    ctx->fx.thr_close_q31 = ctx->fx.thr_open_q31 >> 1;

    {
        const int32_t range = ctx->fx_params[H5_FX_GATE_RANGE];
        int32_t floor_q31 = (int32_t)((int64_t)H5_FX_FULL
            - ((int64_t)range << 15));

        if (floor_q31 < 0) {
            floor_q31 = 0;
        }
        if (floor_q31 > H5_FX_FULL) {
            floor_q31 = H5_FX_FULL;
        }
        ctx->fx.floor_q31 = floor_q31;
    }

    ctx->fx.hold_samples = (uint32_t)(((int64_t)
        ctx->fx_params[H5_FX_GATE_HOLD] * (int64_t)rate) >> 16);

    {
        uint32_t attack_samples = (uint32_t)(((int64_t)
            ctx->fx_params[H5_FX_GATE_ATTACK] * (int64_t)rate) >> 16);
        uint32_t release_samples = (uint32_t)(((int64_t)
            ctx->fx_params[H5_FX_GATE_RELEASE] * (int64_t)rate) >> 16);

        if (attack_samples == 0u) {
            attack_samples = 1u;
        }
        if (release_samples == 0u) {
            release_samples = 1u;
        }
        ctx->fx.atk_delta = H5_FX_FULL / (int32_t)attack_samples;
        ctx->fx.rel_delta = H5_FX_FULL / (int32_t)release_samples;
    }
#endif
}

void h5_fx_clear(struct h5_context_s *ctx)
{
    int ch;
    int line;

    if (ctx->fx.block != 0) {
        memset(ctx->fx.block, 0, ctx->fx.block_size);
    }
    for (ch = 0; ch < 2; ch++) {
        for (line = 0; line < H5_FX_COMBS; line++) {
            ctx->fx.comb[ch][line].pos = 0u;
            ctx->fx.comb[ch][line].lp = 0;
        }
        for (line = 0; line < H5_FX_ALLPASSES; line++) {
            ctx->fx.allpass[ch][line].pos = 0u;
        }
    }
#if H5_FX_NOISE_GATE
    ctx->fx.key_env = 0;
    ctx->fx.gate_gain = 0;
    ctx->fx.hold_count = 0u;
    ctx->fx.state = H5_FX_GATE_CLOSED;
#endif
    ctx->fx.out_env = 0;
}

/* The reverb of one channel: the damped comb lines summed into the series
 * allpass diffusers. */
static int32_t h5_fx_reverb_channel(struct h5_context_s *ctx, int ch,
    int32_t in)
{
    struct h5_fx_s *fx = &ctx->fx;
    int32_t comb_in = h5_mul_q31(in, H5_FX_INPUT_GAIN_Q31);
    int64_t acc = 0;
    int32_t x;
    int line;

    for (line = 0; line < H5_FX_COMBS; line++) {
        struct h5_comb_s *c = &fx->comb[ch][line];
        uint32_t len = fx->comb_len[line];
        uint32_t wp = c->pos;
        uint32_t rp = (wp >= len) ? wp - len : wp + c->alloc - len;
        int32_t y = (int32_t)c->line[rp] * 65536;
        int32_t fb;
        int64_t w;

        c->lp += (int32_t)((((int64_t)y - c->lp) * fx->damp_q16) >> 16);
        fb = (int32_t)(((int64_t)c->lp * H5_FX_FB_Q31) >> 31);
        w = (int64_t)comb_in + (int64_t)fb;
        c->line[wp] = h5_clip_i32_to_i16(h5_sat_i32(w >> 16));

        wp++;
        if (wp >= c->alloc) {
            wp = 0u;
        }
        c->pos = wp;
        acc += y;
    }

    x = h5_sat_i32(acc >> 3);

    for (line = 0; line < H5_FX_ALLPASSES; line++) {
        struct h5_allpass_s *a = &fx->allpass[ch][line];
        uint32_t wp = a->pos;
        int32_t v_old = (int32_t)a->line[wp] * 65536;
        int32_t v = h5_sat_i32((int64_t)x + (v_old >> 1));
        int64_t next = (int64_t)v_old - (v >> 1);

        a->line[wp] = h5_clip_i32_to_i16(v >> 16);
        wp++;
        if (wp >= fx->allpass_len[line]) {
            wp = 0u;
        }
        a->pos = wp;
        x = h5_sat_i32(next);
    }

    return x;
}

void h5_fx_process(struct h5_context_s *ctx, int64_t in_l, int64_t in_r,
    int32_t *out_l, int32_t *out_r)
{
    struct h5_fx_s *fx = &ctx->fx;
    int32_t il = h5_sat_i32(in_l);
    int32_t ir = h5_sat_i32(in_r);
    int32_t al = h5_fx_reverb_channel(ctx, 0, il);
    int32_t ar = h5_fx_reverb_channel(ctx, 1, ir);
    int64_t mid = ((int64_t)al + (int64_t)ar) >> 1;
    int32_t diff = h5_sat_i32((int64_t)al - (int64_t)ar);
    int32_t side = h5_mul_q31(diff, fx->width_q31) >> 1;
    int32_t wl = h5_sat_i32(mid + (int64_t)side);
    int32_t wr = h5_sat_i32(mid - (int64_t)side);
    int32_t level;

#if H5_FX_NOISE_GATE
    {
        int32_t key = h5_fx_mag(il);
        int32_t key_r = h5_fx_mag(ir);

        if (key_r > key) {
            key = key_r;
        }

        /* Instant-attack, linear-release peak follower on the bus input. */
        if (key > fx->key_env) {
            fx->key_env = key;
        } else {
            int64_t env = (int64_t)fx->key_env - (int64_t)fx->rel_delta;

            fx->key_env = (env < 0) ? 0 : (int32_t)env;
        }

        if (fx->floor_q31 == H5_FX_FULL) {
            /* RANGE = 0: the gate is bypassed entirely. */
            fx->gate_gain = H5_FX_FULL;
            fx->state = H5_FX_GATE_CLOSED;
        } else {
            switch (fx->state) {
            case H5_FX_GATE_CLOSED:
                fx->gate_gain = fx->floor_q31;
                if (fx->key_env >= fx->thr_open_q31) {
                    fx->state = H5_FX_GATE_OPENING;
                }
                break;
            case H5_FX_GATE_OPENING: {
                int64_t gain = (int64_t)fx->gate_gain + fx->atk_delta;

                if (gain >= H5_FX_FULL) {
                    gain = H5_FX_FULL;
                    fx->state = H5_FX_GATE_OPEN;
                }
                fx->gate_gain = (int32_t)gain;
                break;
            }
            case H5_FX_GATE_OPEN:
                if (fx->key_env < fx->thr_close_q31) {
                    fx->hold_count = fx->hold_samples;
                    fx->state = H5_FX_GATE_HOLDING;
                }
                break;
            case H5_FX_GATE_HOLDING:
                if (fx->hold_count > 0u) {
                    fx->hold_count--;
                }
                if (fx->key_env >= fx->thr_open_q31) {
                    fx->state = H5_FX_GATE_OPEN;
                } else if (fx->hold_count == 0u) {
                    fx->state = H5_FX_GATE_RELEASING;
                }
                break;
            case H5_FX_GATE_RELEASING: {
                int64_t gain = (int64_t)fx->gate_gain - fx->rel_delta;

                if (gain <= fx->floor_q31) {
                    gain = fx->floor_q31;
                    fx->state = H5_FX_GATE_CLOSED;
                }
                fx->gate_gain = (int32_t)gain;
                break;
            }
            default:
                fx->state = H5_FX_GATE_CLOSED;
                break;
            }
        }
    }

    wl = h5_mul_q31(wl, fx->gate_gain);
    wr = h5_mul_q31(wr, fx->gate_gain);
#else
    /* Without the gate the wet return passes through unchanged. */
    fx->gate_gain = H5_FX_FULL;
#endif

    wl = h5_mul_q31(wl, fx->wet_q31);
    wr = h5_mul_q31(wr, fx->wet_q31);

    level = h5_fx_mag(wl);
    {
        int32_t level_r = h5_fx_mag(wr);

        if (level_r > level) {
            level = level_r;
        }
    }
    /* The bus is busy while its tail is still audible. The level is a
     * peak follower: it jumps to the newest peak at once and falls away
     * slowly, so the sparse echoes of a decaying tail keep it above the
     * threshold instead of dropping between them. */
    if (level > fx->out_env) {
        fx->out_env = level;
    } else {
        fx->out_env = (int32_t)((int64_t)fx->out_env
            - (((int64_t)fx->out_env - level) >> 12));
    }

    *out_l = wl;
    *out_r = wr;
}

bool h5_fx_busy(const struct h5_context_s *ctx)
{
    return ctx->fx.out_env > H5_FX_BUSY_LEVEL;
}

#else /* !H5_FX_REVERB */

/* Without the reverb the bus has no wet signal: the send parameter and the
 * FX parameters are unsupported, so the bus never runs and holds no memory.
 * These stubs keep the rest of the library free of conditionals. */

void h5_fx_defaults(struct h5_context_s *ctx)
{
    (void)ctx;
}

int h5_fx_init(struct h5_context_s *ctx)
{
    (void)ctx;

    return 0;
}

void h5_fx_free(struct h5_context_s *ctx)
{
    (void)ctx;
}

void h5_fx_configure(struct h5_context_s *ctx)
{
    (void)ctx;
}

void h5_fx_clear(struct h5_context_s *ctx)
{
    (void)ctx;
}

void h5_fx_process(struct h5_context_s *ctx, int64_t in_l, int64_t in_r,
    int32_t *out_l, int32_t *out_r)
{
    (void)ctx;
    (void)in_l;
    (void)in_r;
    *out_l = 0;
    *out_r = 0;
}

bool h5_fx_busy(const struct h5_context_s *ctx)
{
    (void)ctx;

    return false;
}

#endif /* H5_FX_REVERB */

h5_status h5_set_fx_param(h5_context handle, h5_fx_param param, int32_t value)
{
    struct h5_context_s *ctx = (struct h5_context_s *)handle;
    const struct h5_param_info *info;
    int32_t clamped;
    h5_status status;

    if (ctx == 0) {
        return H5_ERR_INVALID_CONTEXT;
    }
    if ((unsigned)param >= (unsigned)H5_FX_PARAM_COUNT) {
        return H5_ERR_INVALID_PARAM;
    }

    info = &h5_fx_param_table[param];
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

    ctx->fx_params[param] = clamped;
    h5_fx_configure(ctx);
    return status;
}

h5_status h5_get_fx_param(const h5_context handle, h5_fx_param param,
    int32_t *out)
{
    const struct h5_context_s *ctx = (const struct h5_context_s *)handle;

    if (ctx == 0) {
        return H5_ERR_INVALID_CONTEXT;
    }
    if (out == 0) {
        return H5_ERR_INVALID_PARAM;
    }
    if ((unsigned)param >= (unsigned)H5_FX_PARAM_COUNT) {
        return H5_ERR_INVALID_PARAM;
    }
    if (h5_fx_param_table[param].min == H5_PARAM_UNSUPPORTED) {
        return H5_ERR_UNSUPPORTED;
    }

    *out = ctx->fx_params[param];
    return H5_OK;
}

h5_status h5_set_instrument_fx_send(h5_context handle,
    h5_instrument instrument, int32_t send_q16)
{
    if ((unsigned)instrument >= (unsigned)H5_INSTR_COUNT) {
        return H5_ERR_INVALID_INSTRUMENT;
    }
    return h5_set_param(handle, h5_voice_from_instrument(instrument),
        H5_PARAM_FX_SEND, send_q16);
}

h5_status h5_get_instrument_fx_send(const h5_context handle,
    h5_instrument instrument, int32_t *out)
{
    if ((unsigned)instrument >= (unsigned)H5_INSTR_COUNT) {
        return H5_ERR_INVALID_INSTRUMENT;
    }
    return h5_get_param(handle, h5_voice_from_instrument(instrument),
        H5_PARAM_FX_SEND, out);
}
