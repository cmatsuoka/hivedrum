/*
 * h5_metallic.c - the metallic engine (Hi-hat).
 *
 * The voice runs:
 *
 *   metal = sum_k A_k * tri_k                   10 triangle oscillators
 *   noise = lcg -> two-pole high-pass -> resonant low-pass
 *   env_m = AR(fixed attack, Decay release)      the metal envelope
 *   env_n = AR(fixed attack, Decay+Open release)  the noise envelope
 *   mix   = metal * BellCymbal^0.7 * env_m^5
 *           + noise * (1-BellCymbal)^0.7 * (1-NoiseTone)^0.7 * env_n^4
 *   out   = (1-Drive)*mix + Drive*shaper(2*pi*(1+Drive)*mix)
 *
 * The bank is ten triangle waves at [1205, 1960, 2461, 3216, 4871, 5626,
 * 6127, 6882, 8536, 9292] Hz, each with its own weight. A triangle carries
 * its harmonics with a 1/k^2 law, so the bank sounds like metal instead of
 * the harsh odd-harmonic buzz a square stack makes.
 *
 * The Bell/Cymbal balance is the metal/noise balance and the Noise/Tone
 * balance is the tone amount of the noise path. The noise is the same LCG
 * the drum voices use, with a two-pole high-pass and a resonant low-pass.
 * The Hi-hat keeps only the sibilant band above its corner.
 *
 * The attack of both envelopes is fixed in samples. The Decay control is
 * the release of the metal envelope; the release of the noise envelope is
 * the Decay plus the Open amount of the Hi-hat. The drive stage is a dry
 * path and a saturated path in parallel.
 *
 * The engine is gated behind LIBHIVE_ENGINE_HIHAT. When it is undefined
 * the voice and instruments stay in the public API but render silence and
 * accept no parameters.
 */

#include "h5_internal.h"
#if defined(LIBHIVE_ENGINE_HIHAT)

/* The bank frequencies, Hz. */
static const int32_t h5_metallic_bank[H5_METALLIC_BANK] = {
    1205, 1960, 2461, 3216, 4871, 5626, 6127, 6882, 8536, 9292
};

/* The bank amplitudes, Q16, normalised to the loudest one. */
static const int32_t h5_metallic_amp[H5_METALLIC_BANK] = {
    41898, 65536, 34131, 19797, 12337, 9847, 36460, 2824, 3297, 7020
};

/* The gain of the bank in the mix. */
#define H5_METALLIC_METAL_GAIN_Q16 (111411) /* 1.7 */

/* The weight of the noise path in the mix. */
#define H5_METALLIC_NOISE_WEIGHT_Q16 (51118) /* 0.78 */

/* The noise filter's flat resonance. */
#define H5_METALLIC_RESONANCE_Q16 H5_Q16_ONE

/* The limits of the noise filter's cutoff, Hz. */
#define H5_METALLIC_FC_MIN_HZ 10
#define H5_METALLIC_FC_MAX_HZ 14000

/* The high-pass coefficient is a Q30 one-pole weight. */
#define H5_METALLIC_HP_K_MAX ((int32_t)0x3FFFFFFF)

/* The drive of the saturation stage and the scale of its argument. */
#define H5_METALLIC_HIHAT_DRIVE_Q16 (8519) /* 0.13 */
#define H5_METALLIC_SHAPER_SCALE_Q16 (465000) /* 7.09 */

/* The envelope attack lengths, in samples. */
#define H5_METALLIC_HIHAT_METAL_ATTACK 48
#define H5_METALLIC_HIHAT_NOISE_ATTACK 240

/* The release laws of the Hi-hat's envelopes, in seconds:
 *   metal = 0.13*Decay + 0.05*strike + 0.1
 *   noise = Decay + Open + 0.10*strike */
#define H5_METALLIC_HIHAT_METAL_DECAY_Q16 (8529) /* 0.13 */
#define H5_METALLIC_HIHAT_METAL_GAIN_Q16 (3277) /* 0.05 */
#define H5_METALLIC_HIHAT_NOISE_GAIN_Q16 (6554) /* 0.10 */
#define H5_METALLIC_HIHAT_NOISE_OPEN_Q16 (49152) /* 0.75 */

/* The Hi-hat output gain, and the extra gain for an open hit. */
#define H5_METALLIC_HIHAT_OUTPUT_GAIN_Q16 (82510) /* 1.26, +2 dB */
#define H5_METALLIC_HIHAT_OPEN_GAIN_Q16 (82510) /* +2 dB more when open */

/* The Hi-hat softens its balances on a soft hit. */
#define H5_METALLIC_HIHAT_BELL_VEL_Q16 (6554)  /* 0.10 */
#define H5_METALLIC_HIHAT_NOISE_TONE_VEL_Q16 (32768) /* 0.50 */

/* The internal scale of the mix: Q28. */
#define H5_METALLIC_Q28_ONE (1 << 28)

/* The start phase of an oscillator of the bank. */
static uint32_t h5_metallic_phase(int index)
{
    return (uint32_t)(((uint64_t)index * 0x10000000ull) / H5_METALLIC_BANK);
}

/* The triangle of a phase accumulator. */
static int32_t h5_metallic_tri(uint32_t phase)
{
    uint32_t d = phase & 0x0FFFFFFFu;
    uint32_t u;

    u = (d > 0x08000000u) ? (0x10000000u - d) : d;

    return (int32_t)((int32_t)(1u << 27) - 2 * (int32_t)u);
}

/* The envelope times of the two envelopes. */
static void h5_metallic_env_apply(struct h5_metallic_s *m,
    uint32_t sample_rate)
{
    int32_t decay = m->decay_q16;
    int32_t level = m->vel.level_q16;
    int32_t attack_q16, release_metal, release_noise;

    attack_q16 = (int32_t)(((uint64_t)H5_METALLIC_HIHAT_METAL_ATTACK
        << 16) / sample_rate);
    release_metal = h5_mul_q16(decay, H5_METALLIC_HIHAT_METAL_DECAY_Q16)
        + h5_mul_q16(level, H5_METALLIC_HIHAT_METAL_GAIN_Q16)
        + (H5_Q16_ONE / 10);
    release_noise = decay + m->open_q16
        + h5_mul_q16(level, H5_METALLIC_HIHAT_NOISE_GAIN_Q16);

    h5_env_set_times(&m->env_metal, attack_q16, release_metal, sample_rate);
    attack_q16 = (int32_t)(((uint64_t)H5_METALLIC_HIHAT_NOISE_ATTACK
        << 16) / sample_rate);
    h5_env_set_times(&m->env_noise, attack_q16, release_noise, sample_rate);
}

/* The base cutoff of the noise filter, clamped. */
static int32_t h5_metallic_cutoff(const struct h5_metallic_s *m)
{
    int32_t fc_q16 = m->filter_freq_q16;

    if (fc_q16 < H5_METALLIC_FC_MIN_HZ * H5_Q16_ONE) {
        fc_q16 = H5_METALLIC_FC_MIN_HZ * H5_Q16_ONE;
    }
    if (fc_q16 > H5_METALLIC_FC_MAX_HZ * H5_Q16_ONE) {
        fc_q16 = H5_METALLIC_FC_MAX_HZ * H5_Q16_ONE;
    }
    return fc_q16;
}

/* Recompute the two balance gains from the voice's parameters. */
static void h5_metallic_balance_apply(struct h5_metallic_s *m)
{
    int32_t bell = m->bell_q16;
    int32_t noise_tone = m->noise_tone_q16;
    int32_t deficit = H5_Q16_ONE - m->vel.level_q16;
    int32_t bell_scale = H5_Q16_ONE - h5_mul_q16(deficit,
        H5_METALLIC_HIHAT_BELL_VEL_Q16);
    int32_t tone_scale = H5_Q16_ONE - h5_mul_q16(deficit,
        H5_METALLIC_HIHAT_NOISE_TONE_VEL_Q16);

    bell = h5_mul_q16(bell, bell_scale);
    noise_tone = h5_mul_q16(noise_tone, tone_scale);

    m->bell_hit_q16 = bell;
    m->noise_tone_hit_q16 = noise_tone;

    m->metal_gain_q16 = h5_pow07_q16(bell);
    m->noise_gain_q16 = h5_mul_q16(
        h5_mul_q16(H5_METALLIC_NOISE_WEIGHT_Q16,
            h5_pow07_q16(H5_Q16_ONE - bell)),
        h5_pow07_q16(H5_Q16_ONE - noise_tone));
}

/* Apply the parameters to the engine. */
static void h5_metallic_apply(void *engine,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    struct h5_metallic_s *m = (struct h5_metallic_s *)engine;
    int32_t pitch_scale, fc, hp_q16, hp_k, i;
    int64_t acc;

    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }

    pitch_scale = params[H5_PARAM_TONE_PITCH];

    m->decay_q16 = params[H5_PARAM_DECAY];
    m->bell_q16 = params[H5_PARAM_BELL_CYMBAL];
    m->noise_tone_q16 = params[H5_PARAM_NOISE_TONE];

    m->filter_freq_q16 = params[H5_PARAM_NOISE_PITCH];
    hp_q16 = params[H5_PARAM_NOISE_BODY];

    h5_metallic_balance_apply(m);

    fc = h5_metallic_cutoff(m);
    m->fc_const_q16 = fc;

    hp_k = (int32_t)(((uint64_t)hp_q16 * 6747485820ull)
        / ((uint64_t)sample_rate << 16));
    if (hp_k > H5_METALLIC_HP_K_MAX) {
        hp_k = H5_METALLIC_HP_K_MAX;
    }
    m->hp_k_q30 = hp_k;

    /* Rebuild the bank increments at the new Tone Pitch. */
    for (i = 0; i < H5_METALLIC_BANK; i++) {
        acc = (int64_t)h5_metallic_bank[i] * pitch_scale * 4096;
        m->base_inc[i] = (int32_t)(acc / (int64_t)sample_rate);
        m->inc[i] = m->base_inc[i];
    }

    h5_sweep_build(&m->noise_sweep, H5_SVF_LP,
        H5_METALLIC_RESONANCE_Q16, fc, fc, sample_rate);

    h5_metallic_env_apply(m, sample_rate);
}

void h5_metallic_setup(void *engine, h5_voice voice,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    struct h5_metallic_s *m = (struct h5_metallic_s *)engine;
    int i;

    (void)voice;

    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }

    h5_env_init(&m->env_metal);
    h5_env_init(&m->env_noise);
    h5_noise_init_voice(&m->noise, H5_VOICE_HIHAT, sample_rate);
    h5_svf_reset(&m->noise_filter);
    h5_svf_reset(&m->noise_filter2);
    for (i = 0; i < H5_METALLIC_BANK; i++) {
        m->phase[i] = h5_metallic_phase(i);
        m->inc[i] = 0;
        m->base_inc[i] = 0;
    }
    m->open = 0u;
    m->open_q16 = 0;
    m->hp_state = 0;
    m->hp_state2 = 0;
    m->wet_hp_state = 0;
    m->wet_hp_state2 = 0;
    m->head.active = 0u;
    m->vel.level_q16 = H5_Q16_ONE;
    m->vel.depth_q16 = H5_Q16_ONE;
    m->vel.sweep_q16 = 0;
    m->vel.click_q16 = H5_Q16_ONE;
    m->drive_q16 = H5_METALLIC_HIHAT_DRIVE_Q16;

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

    m->vel.level_q16 = level_q16;
    m->open = (variant != 0u) ? 1u : 0u;
    m->open_q16 = m->open ? H5_METALLIC_HIHAT_NOISE_OPEN_Q16 : 0;
    h5_metallic_balance_apply(m);
    h5_metallic_env_apply(m, sample_rate);

    for (i = 0; i < H5_METALLIC_BANK; i++) {
        m->phase[i] = h5_metallic_phase(i);
        m->inc[i] = m->base_inc[i];
    }
    h5_env_trigger(&m->env_metal);
    h5_env_trigger(&m->env_noise);
    h5_svf_reset(&m->noise_filter);
    h5_svf_reset(&m->noise_filter2);
    m->hp_state = 0;
    m->hp_state2 = 0;
    m->wet_hp_state = 0;
    m->wet_hp_state2 = 0;
    m->head.active = 1u;
}

/* The saturation stage: a dry path and a saturated path in parallel. */
static int32_t h5_metallic_shaper(struct h5_metallic_s *m, int32_t x)
{
    int64_t arg;
    int32_t wet, dry, lp;

    arg = h5_shr_round(h5_mulwide(x, H5_METALLIC_SHAPER_SCALE_Q16), 16u);
    if (arg > H5_METALLIC_Q28_ONE) {
        arg = H5_METALLIC_Q28_ONE;
    }
    if (arg < -H5_METALLIC_Q28_ONE) {
        arg = -H5_METALLIC_Q28_ONE;
    }

    wet = h5_softclip_q31(h5_sat_i32((int64_t)arg << 3)) >> 2;

    lp = m->wet_hp_state + h5_mul_q30(h5_sat_sub_q31(wet, m->wet_hp_state),
        m->hp_k_q30);
    m->wet_hp_state = lp;
    wet = h5_sat_sub_q31(wet, lp);
    lp = m->wet_hp_state2
        + h5_mul_q30(h5_sat_sub_q31(wet, m->wet_hp_state2), m->hp_k_q30);
    m->wet_hp_state2 = lp;
    wet = h5_sat_sub_q31(wet, lp);

    dry = h5_mul_q16(x, H5_Q16_ONE - m->drive_q16);

    return h5_sat_add_q31(dry, h5_mul_q16(wet, m->drive_q16));
}

int32_t h5_metallic_process(void *engine)
{
    struct h5_metallic_s *m = (struct h5_metallic_s *)engine;
    int32_t env_m, env_n, env_m2, env_m4, env_m5, metal, noise, mix, x, u, lp;
    int32_t env_n2, env_n4, cutoff;
    int i;

    if (!m->head.active) {
        return 0;
    }

    env_m = h5_env_next(&m->env_metal);
    env_n = h5_env_next(&m->env_noise);
    if (env_m <= 0 && env_n <= 0
        && m->env_metal.state == H5_ENV_DONE
        && m->env_noise.state == H5_ENV_DONE) {
        m->head.active = 0u;
        return 0;
    }

    metal = 0;
    for (i = 0; i < H5_METALLIC_BANK; i++) {
        m->phase[i] = (m->phase[i] + (uint32_t)m->inc[i]) & 0x0FFFFFFFu;
        metal += (int32_t)h5_shr_round(
            h5_mulwide(h5_metallic_amp[i], h5_metallic_tri(m->phase[i])),
            16u);
    }
    metal = h5_mul_q16(metal, H5_METALLIC_METAL_GAIN_Q16);

    env_m2 = h5_mul_q30(env_m, env_m);
    env_m4 = h5_mul_q30(env_m2, env_m2);
    env_m5 = h5_mul_q30(env_m4, env_m);
    metal = h5_mul_q16(metal, m->metal_gain_q16);
    metal = h5_mul_q30(metal, env_m5);

    u = h5_noise_next(&m->noise) >> 1;
    lp = m->hp_state + h5_mul_q30(h5_sat_sub_q31(u, m->hp_state),
        m->hp_k_q30);
    m->hp_state = lp;
    u = h5_sat_sub_q31(u, lp);
    lp = m->hp_state2 + h5_mul_q30(h5_sat_sub_q31(u, m->hp_state2),
        m->hp_k_q30);
    m->hp_state2 = lp;
    u = h5_sat_i32((int64_t)h5_sat_sub_q31(u, lp) << 1);

    env_n2 = h5_mul_q30(env_n, env_n);
    env_n4 = h5_mul_q30(env_n2, env_n2);

    cutoff = m->fc_const_q16;
    h5_sweep_apply(&m->noise_sweep, cutoff, &m->noise_filter);
    m->noise_filter2.b0 = m->noise_filter.b0;
    m->noise_filter2.a1 = m->noise_filter.a1;
    m->noise_filter2.a2 = m->noise_filter.a2;
    m->noise_filter2.mode = m->noise_filter.mode;
    noise = h5_svf_run(&m->noise_filter, u);
    noise = h5_svf_run(&m->noise_filter2, noise);
    noise = h5_mul_q16(noise >> 3, m->noise_gain_q16);
    noise = h5_mul_q30(noise, env_n4);

    mix = h5_sat_add_q31(metal, noise);

    x = h5_mul_q16(mix, m->vel.level_q16);
    x = h5_sat_i32((int64_t)h5_metallic_shaper(m, x) << 2);
    x = h5_mul_q16(x, H5_METALLIC_HIHAT_OUTPUT_GAIN_Q16);
    if (m->open != 0u) {
        x = h5_mul_q16(x, H5_METALLIC_HIHAT_OPEN_GAIN_Q16);
    }
    return x;
}

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
