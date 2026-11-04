/*
 * h5_metallic.c - the metallic engines (Cymbal and Hi-hat).
 *
 * One engine type serves both voices; they differ in their envelope times,
 * in their noise filter and in the Hi-hat's open/closed trigger variants.
 * The flow is:
 *
 *   metal = sum_k A_k * tri_k                   10 triangle oscillators
 *   noise = lcg -> 25 Hz high-pass -> resonant low-pass
 *   env_m = AR(fixed attack, Decay release)      the metal envelope
 *   env_n = AR(fixed attack, Decay+Open release)  the noise envelope
 *   mix   = metal * BellCymbal^0.7 * env_m^5
 *           + noise * (1-BellCymbal)^0.7 * (1-NoiseTone)^0.7 * env_n^4
 *   out   = (1-Drive)*mix + Drive*shaper(2*pi*(1+Drive)*mix)
 *
 *   - The bank is ten triangle waves at [1205, 1960, 2461, 3216, 4871,
 *     5626, 6127, 6882, 8536, 9292] Hz. A triangle carries its harmonics
 *     with a 1/k^2 law, so the bank sounds like metal instead of the harsh
 *     odd-harmonic buzz a square stack makes. Each oscillator's amplitude
 *     follows the engine's own weight for it, so the bank has the same
 *     balance between the partials.
 *   - The Tone Pitch scales the bank (0..1). The Bend sweeps the whole bank
 *     down or up over the envelope, in semitones, with the same depth law
 *     the drum voices use.
 *   - The Bell/Cymbal balance is the metal/noise balance: it is the engine's
 *     Click control, and it works with the steep x^0.7 law. The Noise/Tone
 *     balance is the tone amount of the noise path: 1 leaves no noise at
 *     all, 0 leaves all of it.
 *   - The noise is the same LCG the drum voices use, with a 25 Hz high-pass
 *     and a resonant low-pass whose cutoff follows the envelope. The Sweep
 *     is signed: a positive sweep closes the filter as the sound dies, a
 *     negative one opens it, so the tail is either dull or bright.
 *   - The attack of both envelopes is fixed in samples: 1 ms for the metal
 *     and 5 ms for the noise of the Hi-hat, 20 ms for both of the Cymbal.
 *     The Decay control is the release of the metal envelope; the release of
 *     the noise envelope is the Decay plus the Open amount of the Hi-hat.
 *   - The drive stage is a dry path and a saturated path in parallel, the
 *     way the engine does it: the dry path keeps the mix linear and the
 *     saturated path adds the soft-clipped harmonics.
 *
 * Each engine is gated behind its own macro: LIBHIVE_ENGINE_HIHAT for the
 * Hi-hat, LIBHIVE_ENGINE_CYMBAL for the Cymbal. Both are defined by
 * default in the Makefile. When one is undefined, its voice and instruments
 * stay in the public API but render silence and accept no parameters.
 */

#include "h5_internal.h"
#if defined(LIBHIVE_ENGINE_HIHAT) || defined(LIBHIVE_ENGINE_CYMBAL)

/* The bank frequencies, Hz. */
static const int32_t h5_metallic_bank[H5_METALLIC_BANK] = {
    1205, 1960, 2461, 3216, 4871, 5626, 6127, 6882, 8536, 9292
};

/* The bank amplitudes, Q16. Each oscillator's weight in the engine's mix
 * divided by its frequency, normalised to the loudest one. The division by
 * the frequency is the engine's own normalisation: the oscillators are
 * integrated squares there, so their amplitude follows the length of their
 * half period. */
static const int32_t h5_metallic_amp[H5_METALLIC_BANK] = {
    14844, 37709, 24628, 18684, 17642, 16273, 65536, 5708, 8251, 19117
};

/* The gain of the bank in the mix. It puts the engine's metal level on the
 * same scale as its noise level. */
#define H5_METALLIC_METAL_GAIN_Q16 (111411) /* 1.7 */

/* The weight of the noise path in the mix. */
#define H5_METALLIC_NOISE_WEIGHT_Q16 (51118) /* 0.78 */

/* The cutoff of the noise filter's base, Hz. It is an engine constant: the
 * faceplate of the metallic modules has no filter cutoff of its own. */
#define H5_METALLIC_HIHAT_FILTER_HZ 16000
#define H5_METALLIC_CYMBAL_FILTER_HZ 9547

/* The corner of the noise high-pass, Hz. */
#define H5_METALLIC_HP_HZ 25

/* The resonance of the noise filter. It is an engine constant too. */
#define H5_METALLIC_RESONANCE_Q16 H5_Q16_ONE

/* The limits of the noise filter's cutoff, Hz. */
#define H5_METALLIC_FC_MIN_HZ 10
#define H5_METALLIC_FC_MAX_HZ 14000

/* The drive of the saturation stage, per voice. It is an engine constant:
 * the faceplate has no drive of its own. */
#define H5_METALLIC_HIHAT_DRIVE_Q16 (8519) /* 0.13 */
#define H5_METALLIC_CYMBAL_DRIVE_Q16 (9830) /* 0.15 */

/* The scale of the saturation stage's argument: 2*pi*(1+Drive). The drive
 * is fixed per voice, so the scale is a constant too. */
#define H5_METALLIC_SHAPER_SCALE_Q16 (465000) /* 7.09 */

/* The envelope attack lengths, in samples. The engine's attacks are fixed
 * counts, not times. */
#define H5_METALLIC_HIHAT_METAL_ATTACK 48
#define H5_METALLIC_HIHAT_NOISE_ATTACK 240
#define H5_METALLIC_CYMBAL_ATTACK 960

/* The release laws of the Hi-hat's envelopes, in seconds:
 *   metal = 0.13*Decay + 0.05*strike + 0.1
 *   noise = Decay + Open + 0.10*strike
 * and of the Cymbal's single envelope: 0.3 + 0.25*Decay. */
#define H5_METALLIC_HIHAT_METAL_DECAY_Q16 (8529) /* 0.13 */
#define H5_METALLIC_HIHAT_METAL_GAIN_Q16 (3277) /* 0.05 */
#define H5_METALLIC_HIHAT_NOISE_GAIN_Q16 (6554) /* 0.10 */
#define H5_METALLIC_HIHAT_NOISE_OPEN_Q16 H5_Q16_ONE
#define H5_METALLIC_CYMBAL_DECAY_Q16 (19661) /* 0.30 */
#define H5_METALLIC_CYMBAL_DECAY_SCALE_Q16 (16384) /* 0.25 */

/* The internal scale of the mix: Q28, so the bank's sum and the noise both
 * fit in an int32_t with the engine's own headroom. */
#define H5_METALLIC_Q28_ONE (1 << 28)

/* The start phase of an oscillator of the bank. The oscillators are spread
 * over the period, so their sum does not sit on a common edge and the bank
 * carries no offset of its own. */
static uint32_t h5_metallic_phase(int index)
{
    return (uint32_t)(((uint64_t)index * 0x10000000ull) / H5_METALLIC_BANK);
}

/* The triangle of a phase accumulator: the phase is 28 bits, the triangle
 * runs from -0.5 to +0.5 with its peak at the half phase. */
static int32_t h5_metallic_tri(uint32_t phase)
{
    uint32_t d = phase & 0x0FFFFFFFu;
    uint32_t u;

    u = (d > 0x08000000u) ? (0x10000000u - d) : d;

    return (int32_t)((int32_t)(1u << 27) - 2 * (int32_t)u);
}

/* The envelope times of the two envelopes. The strike level and the Open
 * amount are part of the release, so the times are set at every trigger. */
static void h5_metallic_env_apply(struct h5_metallic_s *m,
    uint32_t sample_rate)
{
    int32_t decay = m->decay_q16;
    int32_t level = m->vel.level_q16;
    int32_t attack_q16, release_metal, release_noise;

    if (m->has_open) {
        /* The Hi-hat: the metal and the noise have their own envelopes. */
        attack_q16 = (int32_t)(((uint64_t)H5_METALLIC_HIHAT_METAL_ATTACK
            << 16) / sample_rate);
        release_metal = h5_mul_q16(decay,
            H5_METALLIC_HIHAT_METAL_DECAY_Q16)
            + h5_mul_q16(level, H5_METALLIC_HIHAT_METAL_GAIN_Q16)
            + (H5_Q16_ONE / 10);
        release_noise = decay + m->open_q16
            + h5_mul_q16(level, H5_METALLIC_HIHAT_NOISE_GAIN_Q16);

        h5_env_set_times(&m->env_metal, attack_q16, release_metal,
            sample_rate);
        attack_q16 = (int32_t)(((uint64_t)H5_METALLIC_HIHAT_NOISE_ATTACK
            << 16) / sample_rate);
        h5_env_set_times(&m->env_noise, attack_q16, release_noise,
            sample_rate);
    } else {
        /* The Cymbal: the metal and the noise have their own envelopes.
         * The metal release is 0.3 + 0.25*Decay, the noise release is the
         * Decay itself. */
        attack_q16 = (int32_t)(((uint64_t)H5_METALLIC_CYMBAL_ATTACK << 16)
            / sample_rate);
        release_metal = H5_METALLIC_CYMBAL_DECAY_Q16
            + h5_mul_q16(decay, H5_METALLIC_CYMBAL_DECAY_SCALE_Q16);
        h5_env_set_times(&m->env_metal, attack_q16, release_metal,
            sample_rate);
        h5_env_set_times(&m->env_noise, attack_q16, decay, sample_rate);
    }
}

/* The cutoff of the noise filter for the current envelope. The sweep is
 * signed: a positive sweep closes the filter as the sound dies. */
static int32_t h5_metallic_cutoff(const struct h5_metallic_s *m,
    int32_t env_p)
{
    int32_t factor_q16, fc_q16;

    factor_q16 = H5_Q16_ONE - h5_mul_q16(m->sweep_q16,
        H5_Q16_ONE - (env_p >> 14));
    fc_q16 = h5_mul_q16(m->filter_freq_q16, factor_q16);

    if (fc_q16 < H5_METALLIC_FC_MIN_HZ * H5_Q16_ONE) {
        fc_q16 = H5_METALLIC_FC_MIN_HZ * H5_Q16_ONE;
    }
    if (fc_q16 > H5_METALLIC_FC_MAX_HZ * H5_Q16_ONE) {
        fc_q16 = H5_METALLIC_FC_MAX_HZ * H5_Q16_ONE;
    }
    return fc_q16;
}

/* Apply the parameters to one metallic engine. A parameter change goes
 * through here; the engine state is left alone so a sounding voice is not
 * cut. */
static void h5_metallic_apply(void *engine,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    struct h5_metallic_s *m = (struct h5_metallic_s *)engine;
    int32_t pitch_scale, fc_lo, fc_hi, i;
    int64_t acc;

    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }

    pitch_scale = params[H5_PARAM_TONE_PITCH];

    m->decay_q16 = params[H5_PARAM_DECAY];
    m->bend_q16 = params[H5_PARAM_BEND];
    m->sweep_q16 = params[H5_PARAM_SWEEP];

    /* The Bell/Cymbal balance is the metal/noise balance, and the Noise/Tone
     * balance is the tone amount of the noise path. Both follow the steep
     * x^0.7 law of the engine. */
    m->metal_gain_q16 = h5_pow07_q16(params[H5_PARAM_BELL_CYMBAL]);
    m->noise_gain_q16 = h5_mul_q16(
        h5_mul_q16(H5_METALLIC_NOISE_WEIGHT_Q16,
            h5_pow07_q16(H5_Q16_ONE - params[H5_PARAM_BELL_CYMBAL])),
        h5_pow07_q16(H5_Q16_ONE - params[H5_PARAM_NOISE_TONE]));

    m->filter_freq_q16 = m->has_open
        ? H5_METALLIC_HIHAT_FILTER_HZ * H5_Q16_ONE
        : H5_METALLIC_CYMBAL_FILTER_HZ * H5_Q16_ONE;
    /* The cutoff of a zero sweep does not depend on the envelope, so it is
     * computed once here instead of on every sample. With a zero sweep the
     * contour factor is exactly 1, so this is the clamped base cutoff. */
    m->fc_const_q16 = h5_metallic_cutoff(m, 0);

    /* The high-pass on the noise: one pole at 25 Hz. Its coefficient is
     * 1 - exp(-2*pi*fc/sr), in Q30. */
    m->hp_k_q30 = (int32_t)(((uint64_t)H5_METALLIC_HP_HZ * 6747485820ull)
        / (uint64_t)sample_rate);

    /* Rebuild the bank increments at the new Tone Pitch.
     * cycles/sample = f / sr; the 28-bit phase adds 2^28, and the bank
     * entries are plain Hz, so inc = bank * pitch_q16 * 2^12 / sr. */
    for (i = 0; i < H5_METALLIC_BANK; i++) {
        acc = (int64_t)h5_metallic_bank[i] * pitch_scale * 4096;
        m->base_inc[i] = (int32_t)(acc / (int64_t)sample_rate);
        m->inc[i] = m->base_inc[i];
    }

    /* The noise filter sweeps with the envelope between the two ends of the
     * sweep range. */
    fc_lo = h5_metallic_cutoff(m, 0);
    fc_hi = h5_metallic_cutoff(m, H5_Q16_ONE << 14);
    if (fc_lo > fc_hi) {
        int32_t swap = fc_lo;

        fc_lo = fc_hi;
        fc_hi = swap;
    }
    h5_sweep_build(&m->noise_sweep, H5_SVF_LP, H5_METALLIC_RESONANCE_Q16,
        fc_lo, fc_hi, sample_rate);

    h5_metallic_env_apply(m, sample_rate);
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
    h5_env_init(&m->env_metal);
    h5_env_init(&m->env_noise);
    h5_noise_init_voice(&m->noise, voice, sample_rate);
    h5_svf_reset(&m->noise_filter);
    h5_svf_reset(&m->noise_filter2);
    for (i = 0; i < H5_METALLIC_BANK; i++) {
        m->phase[i] = h5_metallic_phase(i);
        m->inc[i] = 0;
        m->base_inc[i] = 0;
    }
    m->open = 0u;
    m->open_q16 = 0;
    m->has_open = (voice == H5_VOICE_HIHAT) ? 1u : 0u;
    m->hp_state = 0;
    m->hp_state2 = 0;
    m->head.active = 0u;
    m->vel.level_q16 = H5_Q16_ONE;
    m->vel.depth_q16 = H5_Q16_ONE;
    m->vel.sweep_q16 = 0;
    m->vel.click_q16 = H5_Q16_ONE;
    m->drive_q16 = m->has_open
        ? H5_METALLIC_HIHAT_DRIVE_Q16
        : H5_METALLIC_CYMBAL_DRIVE_Q16;

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
    m->open_q16 = m->open ? H5_METALLIC_HIHAT_NOISE_OPEN_Q16 : 0;
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
    m->head.active = 1u;
}

/* The saturation stage: a dry path and a saturated path in parallel. */
static int32_t h5_metallic_shaper(const struct h5_metallic_s *m, int32_t x)
{
    int64_t arg;
    int32_t wet, dry;

    /* arg = 2*pi*(1+Drive)*mix, clamped to the shaper's input range. The
     * scale fits an int32_t, so the product stays on the wide-multiply
     * seam. */
    arg = h5_shr_round(h5_mulwide(x, H5_METALLIC_SHAPER_SCALE_Q16), 16u);
    if (arg > H5_METALLIC_Q28_ONE) {
        arg = H5_METALLIC_Q28_ONE;
    }
    if (arg < -H5_METALLIC_Q28_ONE) {
        arg = -H5_METALLIC_Q28_ONE;
    }

    /* The soft clipper reads a Q31 argument of at most 1. */
    wet = h5_softclip_q31(h5_sat_i32((int64_t)arg << 3)) >> 2;
    dry = h5_mul_q16(x, H5_Q16_ONE - m->drive_q16);

    return h5_sat_add_q31(dry, h5_mul_q16(wet, m->drive_q16));
}

int32_t h5_metallic_process(void *engine)
{
    struct h5_metallic_s *m = (struct h5_metallic_s *)engine;
    int32_t env_m, env_n, env_m3, env_f, metal, noise, mix, x, u, lp;
    int32_t env_m2, env_m4, env_m5, env_n2, env_n4, env_n3;
    int32_t ratio, cutoff;
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

    /* The bank: ten triangle oscillators. The Bend sweeps the whole bank
     * with the metal envelope, so the increments follow the envelope. */
    env_m3 = (m->bend_q16 != 0 || !m->has_open) ? h5_cube_q30(env_m) : 0;
    if (m->bend_q16 != 0) {
        ratio = h5_semitone_ratio_interp(
            (int32_t)h5_shr_round((int64_t)m->bend_q16 * env_m3, 30u));
        for (i = 0; i < H5_METALLIC_BANK; i++) {
            m->inc[i] = h5_scale_q16(m->base_inc[i], ratio);
        }
    }

    metal = 0;
    for (i = 0; i < H5_METALLIC_BANK; i++) {
        m->phase[i] = (m->phase[i] + (uint32_t)m->inc[i]) & 0x0FFFFFFFu;
        metal += (int32_t)h5_shr_round(
            h5_mulwide(h5_metallic_amp[i], h5_metallic_tri(m->phase[i])),
            16u);
    }
    metal = h5_mul_q16(metal, H5_METALLIC_METAL_GAIN_Q16);

    /* The metal envelope is raised to the fifth power. */
    env_m2 = h5_mul_q30(env_m, env_m);
    env_m4 = h5_mul_q30(env_m2, env_m2);
    env_m5 = h5_mul_q30(env_m4, env_m);
    metal = h5_mul_q16(metal, m->metal_gain_q16);
    metal = h5_mul_q30(metal, env_m5);

    /* The noise: the LCG, a high-pass at 25 Hz and the resonant low-pass
     * whose cutoff follows the envelope. */
    u = h5_noise_next(&m->noise);
    lp = m->hp_state + h5_mul_q30(h5_sat_sub_q31(u, m->hp_state),
        m->hp_k_q30);
    m->hp_state = lp;
    u = h5_sat_sub_q31(u, lp);
    lp = m->hp_state2 + h5_mul_q30(h5_sat_sub_q31(u, m->hp_state2),
        m->hp_k_q30);
    m->hp_state2 = lp;
    u = h5_sat_sub_q31(u, lp);

    if (m->has_open) {
        /* The Hi-hat: the noise envelope is raised to the fourth power,
         * and it is that envelope that drives the filter. */
        env_n2 = h5_mul_q30(env_n, env_n);
        env_n4 = h5_mul_q30(env_n2, env_n2);
        env_f = env_n4;
    } else {
        /* The Cymbal: the noise envelope is raised to the third power, and
         * the filter follows the metal envelope, which falls away faster. */
        env_n3 = h5_cube_q30(env_n);
        env_n4 = env_n3;
        env_f = env_m3;
    }

    cutoff = (m->sweep_q16 == 0)
        ? m->fc_const_q16 : h5_metallic_cutoff(m, env_f);
    h5_sweep_apply(&m->noise_sweep, cutoff, &m->noise_filter);
    /* The two sections share the cutoff, so the coefficients interpolated
     * for the first section are the ones the second needs too. */
    m->noise_filter2.b0 = m->noise_filter.b0;
    m->noise_filter2.a1 = m->noise_filter.a1;
    m->noise_filter2.a2 = m->noise_filter.a2;
    m->noise_filter2.mode = m->noise_filter.mode;
    noise = h5_svf_run(&m->noise_filter, u);
    noise = h5_svf_run(&m->noise_filter2, noise);
    noise = h5_mul_q16(noise >> 3, m->noise_gain_q16);
    noise = h5_mul_q30(noise, env_n4);

    mix = h5_sat_add_q31(metal, noise);

    /* The strike level, then the saturation stage. */
    x = h5_mul_q16(mix, m->vel.level_q16);
    return h5_sat_i32((int64_t)h5_metallic_shaper(m, x) << 2);
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
