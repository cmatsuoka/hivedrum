/*
 * h5_metallic.c - the metallic engines (Cymbal and Hi-hat).
 *
 * One engine type serves both voices; they differ in the output filter's
 * sweep depth and in the Hi-hat's open/closed trigger variants. The flow
 * is:
 *
 *   metal = sum_k (2*floor(2*phi_k) - 1)        10 square oscillators
 *   mix   = (1-NoiseTone)*metal + NoiseTone*u_hold
 *   env   = AR(Click, Decay)                    linear AR
 *   x     = mix * env * level                   level = the strike level
 *   pre   = x * (1 + (1-Drive)*level)           pre-saturation drive scales
 *                                               with (1-Drive)*gain
 *   y     = tanh(1.5 * pre)
 *   out   = SVF_LP(y; FilterFreq*(1 + FilterBend*env^3), Resonance)
 *           * 1.10
 *
 *   - Pitch scales the fixed bank [1205, 1960, 2461, 3216, 4871, 5626,
 *     6127, 6882, 8536, 9292] Hz; it is not a single-oscillator frequency.
 *   - The square oscillators are the sign bit of a 28-bit phase, so no
 *     table or interpolation is needed.
 *   - The Hi-hat is one voice with two trigger targets: the open instrument
 *     plays the same voice with a longer release (the release time is
 *     multiplied by H5_OPEN_RELEASE_MULT); the closed instrument plays it
 *     with the release as set. Retriggering the closed instrument while
 *     an open hat is sounding restarts the envelope, which chokes it.
 *   - The velocity scales the mix linearly and scales the pre-saturation
 *     drive by (1-Drive)*gain. It does not bend the pitch.
 *   - Bend is not mapped: the engine computes no pitch sweep, so the
 *     library marks H5_PARAM_BEND unsupported on both voices.
 *
 * Each engine is gated behind its own macro: LIBHIVE_ENGINE_HIHAT for the
 * Hi-hat, LIBHIVE_ENGINE_CYMBAL for the Cymbal. Both are defined by
 * default in the Makefile. When one is undefined, its voice and instruments
 * stay in the public API but render silence and accept no parameters.
 */

#include "h5_internal.h"

#if defined(LIBHIVE_ENGINE_HIHAT) || defined(LIBHIVE_ENGINE_CYMBAL)

/* Fixed output gain after the shaper: 1.10. */
#define H5_METALLIC_OUT_GAIN_Q16 (72090)

/* Per-oscillator square amplitude. Ten oscillators sum to at most
 * 10 * (1 << 27), comfortably inside int32_t. */
#define H5_METALLIC_SQ (1 << 27)

/* The sign bit of the 28-bit phase: the upper half of the period. */
#define H5_METALLIC_SIGN (1u << 27)

/* The bank frequencies, Hz. */
static const int32_t h5_metallic_bank[H5_METALLIC_BANK] = {
    1205, 1960, 2461, 3216, 4871, 5626, 6127, 6882, 8536, 9292
};

/* The strike level at rest. The trigger passes the real level. */
#define H5_METALLIC_LEVEL_FULL H5_Q16_ONE

/* The pre-saturation drive control. Fixed engine constant: it is not a
 * public parameter, so the library keeps it at the engine default, like the
 * drum voices. */
#define H5_METALLIC_DRIVE_Q16 (9830) /* 0.15 */

/* The Cymbal's filter-envelope depth; the Hi-hat keeps its default of 0. */
#define H5_CYMBAL_FILTER_BEND_Q16 (45875) /* 0.7 */

/* Open Hi-hat release multiplier. */
#define H5_OPEN_RELEASE_MULT 4

/* Set the envelope times; the open variant multiplies the release. */
static void h5_metallic_env_apply(struct h5_metallic_s *m,
    uint32_t sample_rate);

/* The bend of the output filter at full envelope, in Q16 of the base
 * cutoff: fc = FilterFreq * (1 + FilterBend * env^3). */
static int32_t h5_metallic_fc(const struct h5_metallic_s *m, int32_t env3)
{
    int32_t bend = h5_mul_q16(m->filter_bend_q16, env3 >> 14);

    return h5_sat_i32((int64_t)m->noise_pitch_q16
        + h5_mul_q16(m->noise_pitch_q16, bend));
}

/* Apply the parameters to one metallic engine. A parameter change goes
 * through here; the engine state is left alone so a sounding voice is not
 * cut. */
static void h5_metallic_apply(void *engine,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    struct h5_metallic_s *m = (struct h5_metallic_s *)engine;
    int32_t pitch_scale, freq, fc_hi, i;
    int64_t acc;

    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }

    pitch_scale = params[H5_PARAM_TONE_PITCH];
    freq = params[H5_PARAM_NOISE_PITCH];

    m->noise_pitch_q16 = freq;
    m->noise_tone_q16 = params[H5_PARAM_NOISE_TONE];
    m->click_q16 = params[H5_PARAM_CLICK_TONE];
    m->decay_q16 = params[H5_PARAM_DECAY];

    /* Rebuild the bank increments at the new Pitch scale.
     * cycles/sample = f / sr; the 28-bit phase adds 2^28, and the bank
     * entries are plain Hz, so inc = bank * pitch_q16 * 2^12 / sr. */
    for (i = 0; i < H5_METALLIC_BANK; i++) {
        acc = (int64_t)h5_metallic_bank[i] * pitch_scale * 4096;
        m->inc[i] = (int32_t)(acc / (int64_t)sample_rate);
    }

    /* Envelope times with the current open state; a running envelope keeps
     * its position and picks up the new deltas. */
    h5_metallic_env_apply(m, sample_rate);

    /* Output filter: swept low-pass from the base cutoff to the cutoff at
     * a full envelope. The sweep table carries the coefficients and
     * h5_sweep_apply() writes them into the filter on every sample, so the
     * filter itself is not tuned here. */
    fc_hi = h5_sat_i32((int64_t)freq
        + h5_mul_q16(freq, m->filter_bend_q16));
    h5_sweep_build(&m->out_sweep, H5_SVF_LP, H5_Q16_ONE,
        freq, fc_hi, sample_rate);
}

static void h5_metallic_env_apply(struct h5_metallic_s *m,
    uint32_t sample_rate)
{
    int32_t release = m->decay_q16;

    if (m->has_open && m->open) {
        release = h5_sat_i32((int64_t)release * H5_OPEN_RELEASE_MULT);
    }
    h5_env_set_times(&m->env, m->click_q16, release, sample_rate);
}

void h5_metallic_setup(void *engine, h5_voice voice,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    struct h5_metallic_s *m = (struct h5_metallic_s *)engine;
    int i;

    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }

    /* Initial state. */
    h5_env_init(&m->env);
    h5_noise_init_voice(&m->noise, voice, sample_rate);
    h5_svf_reset(&m->out_filter);
    for (i = 0; i < H5_METALLIC_BANK; i++) {
        m->phase[i] = 0u;
        m->inc[i] = 0;
    }
    m->open = 0u;
    m->has_open = (voice == H5_VOICE_HIHAT) ? 1u : 0u;
    m->filter_bend_q16 = (voice == H5_VOICE_CYMBAL)
        ? H5_CYMBAL_FILTER_BEND_Q16 : 0;
    m->noise_pitch_q16 = 0;
    m->noise_tone_q16 = 0;
    m->click_q16 = 0;
    m->decay_q16 = 0;
    m->head.active = 0u;
    m->vel.level_q16 = H5_METALLIC_LEVEL_FULL;
    m->vel.depth_q16 = H5_Q16_ONE;
    m->vel.sweep_q16 = 0;
    m->vel.click_q16 = H5_Q16_ONE;

    h5_metallic_apply(engine, params, sample_rate);
}

void h5_metallic_configure(void *engine,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    h5_metallic_apply(engine, params, sample_rate);
}

void h5_metallic_trigger(void *engine, int32_t level_q16,
    uint32_t sample_rate, uint32_t variant)
{
    struct h5_metallic_s *m = (struct h5_metallic_s *)engine;
    int i;

    /* The strike level of this hit; the metallic voices take it as a
     * linear gain. */
    m->vel.level_q16 = level_q16;

    /* Open and closed releases differ on the Hi-hat; recompute the
     * envelope times for this hit. */
    m->open = (m->has_open && variant != 0u) ? 1u : 0u;
    h5_metallic_env_apply(m, sample_rate);

    for (i = 0; i < H5_METALLIC_BANK; i++) {
        m->phase[i] = 0u;
    }
    h5_env_trigger(&m->env);
    h5_svf_reset(&m->out_filter);
    m->head.active = 1u;
}

int32_t h5_metallic_process(void *engine)
{
    struct h5_metallic_s *m = (struct h5_metallic_s *)engine;
    int32_t env, env3, metal, mix, x, pre, y, out, cutoff;
    int i;

    if (!m->head.active) {
        return 0;
    }

    env = h5_env_next(&m->env);
    if (env <= 0 && m->env.state == H5_ENV_DONE) {
        m->head.active = 0u;
        return 0;
    }

    /* The bank: ten square oscillators, one sign bit each. */
    metal = 0;
    for (i = 0; i < H5_METALLIC_BANK; i++) {
        m->phase[i] = (m->phase[i] + (uint32_t)m->inc[i]) & 0x0FFFFFFFu;
        metal += (m->phase[i] & H5_METALLIC_SIGN) != 0u
            ? H5_METALLIC_SQ : -H5_METALLIC_SQ;
    }

    /* mix = (1-NoiseTone)*metal + NoiseTone*u_hold. */
    mix = h5_sat_add_q31(
        h5_mul_q16(metal, H5_Q16_ONE - m->noise_tone_q16),
        h5_mul_q16(h5_noise_next(&m->noise), m->noise_tone_q16));

    /* The voice mix multiplied by the strike level; the pre-saturation
     * drive scaled by (1-Drive)*level. */
    x = h5_mul_q30(mix, env);
    x = h5_scale_q16(x, m->vel.level_q16);
    pre = h5_scale_q16(x, H5_Q16_ONE
        + h5_scale_q16(H5_Q16_ONE - H5_METALLIC_DRIVE_Q16, m->vel.level_q16));

    y = h5_softclip_q31(pre);

    /* The fixed output gain, then the swept low-pass:
     * fc = FilterFreq * (1 + FilterBend * env^3). */
    out = h5_mul_q16(y, H5_METALLIC_OUT_GAIN_Q16);
    env3 = h5_cube_q30(env);
    cutoff = h5_metallic_fc(m, env3);
    h5_sweep_apply(&m->out_sweep, cutoff, &m->out_filter);
    return h5_svf_run(&m->out_filter, out);
}

#endif /* LIBHIVE_ENGINE_HIHAT || LIBHIVE_ENGINE_CYMBAL */

/* Engine operation sets. A disabled engine is replaced by a stub that
 * renders silence and never reports activity; the voice and its
 * instruments stay in the public API. */

#if defined(LIBHIVE_ENGINE_HIHAT)
const struct h5_engine_ops h5_hihat_ops = {
    h5_metallic_setup, h5_metallic_configure, h5_metallic_trigger,
    h5_metallic_process
};
#else
static void h5_hihat_stub_setup(void *engine, h5_voice voice,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    struct h5_engine_s *head = (struct h5_engine_s *)engine;

    (void)voice;
    (void)params;
    (void)sample_rate;
    head->active = 0u;
}

static void h5_hihat_stub_configure(void *engine,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    (void)engine;
    (void)params;
    (void)sample_rate;
}

static void h5_hihat_stub_trigger(void *engine, int32_t level_q16,
    uint32_t sample_rate, uint32_t variant)
{
    (void)engine;
    (void)level_q16;
    (void)sample_rate;
    (void)variant;
}

static int32_t h5_hihat_stub_process(void *engine)
{
    (void)engine;
    return 0;
}

const struct h5_engine_ops h5_hihat_ops = {
    h5_hihat_stub_setup, h5_hihat_stub_configure,
    h5_hihat_stub_trigger, h5_hihat_stub_process
};
#endif /* LIBHIVE_ENGINE_HIHAT */

#if defined(LIBHIVE_ENGINE_CYMBAL)
const struct h5_engine_ops h5_cymbal_ops = {
    h5_metallic_setup, h5_metallic_configure, h5_metallic_trigger,
    h5_metallic_process
};
#else
static void h5_cymbal_stub_setup(void *engine, h5_voice voice,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    struct h5_engine_s *head = (struct h5_engine_s *)engine;

    (void)voice;
    (void)params;
    (void)sample_rate;
    head->active = 0u;
}

static void h5_cymbal_stub_configure(void *engine,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    (void)engine;
    (void)params;
    (void)sample_rate;
}

static void h5_cymbal_stub_trigger(void *engine, int32_t level_q16,
    uint32_t sample_rate, uint32_t variant)
{
    (void)engine;
    (void)level_q16;
    (void)sample_rate;
    (void)variant;
}

static int32_t h5_cymbal_stub_process(void *engine)
{
    (void)engine;
    return 0;
}

const struct h5_engine_ops h5_cymbal_ops = {
    h5_cymbal_stub_setup, h5_cymbal_stub_configure,
    h5_cymbal_stub_trigger, h5_cymbal_stub_process
};
#endif /* LIBHIVE_ENGINE_CYMBAL */
