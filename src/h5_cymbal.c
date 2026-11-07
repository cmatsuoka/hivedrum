/*
 * h5_cymbal.c - the Cymbal voice.
 *
 * The Cymbal is built from two parts:
 *
 *   - A short strike table, measured from the instrument, played through a
 *     variable-rate phase accumulator. The Tone Pitch sets the playback
 *     rate and Bend sweeps it over the wash envelope, so the strike takes
 *     the falling or rising character of the hardware.
 *   - A synthesized wash: the noise generator through a high-pass
 *     (Noise Body) and a swept low-pass (Noise and Sweep), plus a resonant
 *     band for the tonal part of the Noise/Tone balance. The wash has an
 *     exponential release, so its level falls by a nearly constant number
 *     of decibels per second through the tail.
 *
 * The Bell/Cymbal balance trades the sampled strike against the wash, and
 * Noise/Tone trades the noisy part of the wash against its tonal band.
 * The two parts share the strike level and leave through a fixed high-pass
 * and a soft clipper.
 *
 * The engine is gated behind LIBHIVE_ENGINE_CYMBAL. When it is undefined
 * the voice and instrument stay in the public API but render silence and
 * accept no parameters.
 */

#include "h5_internal.h"

#if defined(LIBHIVE_ENGINE_CYMBAL)

/* The strike table's own rate. */
#define H5_CYMBAL_TABLE_RATE 48000

/* The wash low-pass and the tonal band. */
#define H5_CYMBAL_LP_Q_Q16 H5_Q16_ONE
#define H5_CYMBAL_TONE_Q_Q16 (3 * H5_Q16_ONE)

/* The tonal band sits between these frequencies as Tone Pitch moves. */
#define H5_CYMBAL_TONE_LO_HZ 1800
#define H5_CYMBAL_TONE_SPAN_HZ 4200

/* The fixed output high-pass. */
#define H5_CYMBAL_OUT_HP_HZ 90
#define H5_CYMBAL_OUT_HP_Q (46341) /* 1/sqrt(2) */

/* The release law: the Decay control is the time to fall about 60 dB, so
 * one e-fold is Decay / ln(1000). */
#define H5_CYMBAL_TAU_SCALE_Q16 (9490) /* 1/6.9078 */

/* The wash envelope has a fast component that gives the first part of the
 * tail its extra drop, and a slow component that owns the long release. */
#define H5_CYMBAL_FAST_DIV 8
#define H5_CYMBAL_FAST_MIX_Q16 (19661) /* 0.30 */

/* The wash and tone gains. */
#define H5_CYMBAL_WASH_GAIN_Q16 (58982) /* 0.9 */
#define H5_CYMBAL_TONE_GAIN_Q16 (131072) /* 2.0 */

/* The envelope stops below this Q30 value (about -90 dB of the peak). */
#define H5_CYMBAL_ENV_STOP (1 << 10)

/* Q30 x Q30 -> Q30. */
static int32_t h5_cymbal_mul_q30(int32_t a, int32_t b)
{
    return h5_sat_i32(((int64_t)a * b) >> 30);
}

/* exp(-1/t) in Q30 for t >= 2 samples per e-fold, by the short series. */
static int32_t h5_cymbal_exp_q30(int32_t t)
{
    int64_t x, x2, x3, c;

    if (t < 2) {
        t = 2;
    }
    x = ((int64_t)1 << 30) / t;
    x2 = (x * x) >> 30;
    x3 = (x2 * x) >> 30;
    c = ((int64_t)1 << 30) - x + x2 / 2 - x3 / 6;
    if (c < 0) {
        c = 0;
    }
    if (c > ((int64_t)1 << 30)) {
        c = (int64_t)1 << 30;
    }
    return (int32_t)c;
}

/* The strike table sample at a Q16 position, Q31, zero past the end. */
static int32_t h5_cymbal_attack_sample(int32_t phase_q16)
{
    int32_t index = phase_q16 >> 16;
    int32_t frac = phase_q16 & 0xFFFF;
    int32_t a, b;

    if (index < 0 || index >= H5_CYMBAL_ATTACK_LEN - 1) {
        return 0;
    }
    a = (int32_t)h5_cymbal_attack[index] << 16;
    b = (int32_t)h5_cymbal_attack[index + 1] << 16;
    return a + h5_mul_q16(b - a, frac);
}

/* Apply the parameters. Called on setup and whenever a parameter changes. */
static void h5_cymbal_apply(void *engine,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    struct h5_cymbal_s *c = (struct h5_cymbal_s *)engine;
    int32_t pitch, bell, noise_tone, decay;
    int32_t fc_attack, fc_tail, tone_freq, tau_q16, tau_samples;
    int64_t step;

    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }

    pitch = params[H5_PARAM_TONE_PITCH];
    bell = params[H5_PARAM_BELL_CYMBAL];
    noise_tone = params[H5_PARAM_NOISE_TONE];
    decay = params[H5_PARAM_DECAY];

    c->pitch_q16 = pitch;
    c->bend_q16 = params[H5_PARAM_BEND];
    c->decay_q16 = decay;
    c->sweep_q16 = params[H5_PARAM_SWEEP];
    c->filter_freq_q16 = params[H5_PARAM_NOISE_PITCH];
    c->hp_freq_q16 = params[H5_PARAM_NOISE_BODY];

    /* The Bell/Cymbal balance trades the strike against the wash, and
     * Noise/Tone trades the noisy wash against the tonal band. Both use
     * the soft x^0.7 law. */
    c->attack_gain_q16 = h5_pow07_q16(bell);
    c->tail_gain_q16 = h5_pow07_q16(H5_Q16_ONE - bell);
    c->noise_gain_q16 = h5_pow07_q16(H5_Q16_ONE - noise_tone);
    c->tone_gain_q16 = h5_pow07_q16(noise_tone);

    /* The strike table step: one table sample per output sample at 48 kHz
     * and Tone Pitch 1.0. */
    step = (int64_t)H5_CYMBAL_TABLE_RATE * pitch / (int64_t)sample_rate;
    if (step < 1) {
        step = 1;
    }
    c->attack_step_q16 = (int32_t)step;

    /* The wash high-pass and the fixed output high-pass. */
    h5_svf_tune(&c->noise_hp, H5_SVF_HP, c->hp_freq_q16, H5_Q16_ONE,
        sample_rate);
    h5_svf_tune(&c->out_hp, H5_SVF_HP,
        H5_CYMBAL_OUT_HP_HZ * H5_Q16_ONE, H5_CYMBAL_OUT_HP_Q, sample_rate);

    /* The tonal band. */
    tone_freq = (H5_CYMBAL_TONE_LO_HZ * H5_Q16_ONE)
        + h5_mul_q16(H5_CYMBAL_TONE_SPAN_HZ * H5_Q16_ONE, pitch);
    h5_svf_tune(&c->tone_bp, H5_SVF_BP, tone_freq, H5_CYMBAL_TONE_Q_Q16,
        sample_rate);

    /* The wash low-pass contour: at the strike it sits at the Noise
     * setting, and it moves toward (1 - Sweep) times that value as the
     * envelope falls. A positive Sweep closes the filter, a negative one
     * opens it. */
    fc_attack = c->filter_freq_q16;
    fc_tail = h5_mul_q16(c->filter_freq_q16,
        H5_Q16_ONE - c->sweep_q16);
    if (fc_tail < 20 * H5_Q16_ONE) {
        fc_tail = 20 * H5_Q16_ONE;
    }
    if (fc_tail > 18000 * H5_Q16_ONE) {
        fc_tail = 18000 * H5_Q16_ONE;
    }
    if (c->sweep_q16 == 0) {
        h5_svf_tune(&c->noise_lp, H5_SVF_LP, fc_attack,
            H5_CYMBAL_LP_Q_Q16, sample_rate);
    } else {
        int32_t lo = (fc_attack < fc_tail) ? fc_attack : fc_tail;
        int32_t hi = (fc_attack < fc_tail) ? fc_tail : fc_attack;

        h5_sweep_build(&c->lp_sweep, H5_SVF_LP, H5_CYMBAL_LP_Q_Q16,
            lo, hi, sample_rate);
    }

    /* The exponential wash release. The fast component is a short drop
     * under the slow one. */
    tau_q16 = h5_mul_q16(decay, H5_CYMBAL_TAU_SCALE_Q16);
    tau_samples = (int32_t)(((int64_t)tau_q16 * sample_rate) >> 16);
    if (tau_samples < 4) {
        tau_samples = 4;
    }
    c->env_slow_coeff = h5_cymbal_exp_q30(tau_samples);
    c->env_fast_coeff = h5_cymbal_exp_q30(tau_samples / H5_CYMBAL_FAST_DIV);
    c->env_fast_mix_q16 = H5_CYMBAL_FAST_MIX_Q16;
}

void h5_cymbal_setup(void *engine, h5_voice voice,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    struct h5_cymbal_s *c = (struct h5_cymbal_s *)engine;

    (void)voice;

    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }

    h5_noise_init_voice(&c->noise, H5_VOICE_CYMBAL, sample_rate);
    h5_svf_reset(&c->noise_hp);
    h5_svf_reset(&c->noise_lp);
    h5_svf_reset(&c->tone_bp);
    h5_svf_reset(&c->out_hp);
    c->attack_phase_q16 = 0;
    c->attack_step_q16 = 0;
    c->attack_gain_q16 = 0;
    c->tail_gain_q16 = 0;
    c->noise_gain_q16 = 0;
    c->tone_gain_q16 = 0;
    c->env_slow = 0;
    c->env_fast = 0;
    c->env_slow_coeff = 0;
    c->env_fast_coeff = 0;
    c->env_fast_mix_q16 = 0;
    c->level_q16 = H5_Q16_ONE;
    c->filter_freq_q16 = 0;
    c->hp_freq_q16 = 0;
    c->sweep_q16 = 0;
    c->bend_q16 = 0;
    c->pitch_q16 = H5_Q16_ONE;
    c->decay_q16 = 0;
    c->attack_active = 0u;
    c->head.active = 0u;

    h5_cymbal_apply(engine, params, sample_rate);
}

void h5_cymbal_configure(void *engine,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    h5_cymbal_apply(engine, params, sample_rate);
}

void h5_cymbal_trigger(void *engine, int32_t level_q16,
    uint32_t sample_rate, uint32_t variant)
{
    struct h5_cymbal_s *c = (struct h5_cymbal_s *)engine;

    (void)sample_rate;
    (void)variant; /* the Cymbal has no open character */

    c->level_q16 = level_q16;
    c->attack_phase_q16 = 0;
    c->attack_active = (c->attack_gain_q16 != 0) ? 1u : 0u;
    c->env_slow = (c->tail_gain_q16 != 0) ? ((int32_t)1 << 30) : 0;
    c->env_fast = c->env_slow;

    h5_svf_reset(&c->noise_hp);
    h5_svf_reset(&c->noise_lp);
    h5_svf_reset(&c->tone_bp);
    h5_svf_reset(&c->out_hp);
    c->head.active = 1u;
}

int32_t h5_cymbal_process(void *engine)
{
    struct h5_cymbal_s *c = (struct h5_cymbal_s *)engine;
    int32_t env, env16, attack, u, hp, lp, bp, wash, mix;
    int32_t step;

    if (!c->head.active) {
        return 0;
    }

    c->env_slow = h5_cymbal_mul_q30(c->env_slow, c->env_slow_coeff);
    c->env_fast = h5_cymbal_mul_q30(c->env_fast, c->env_fast_coeff);
    env = h5_sat_add_q31(
        h5_mul_q16(c->env_slow, H5_Q16_ONE - c->env_fast_mix_q16),
        h5_mul_q16(c->env_fast, c->env_fast_mix_q16));

    if (c->attack_active == 0u && c->env_slow <= H5_CYMBAL_ENV_STOP
        && c->env_fast <= H5_CYMBAL_ENV_STOP) {
        c->head.active = 0u;
        return 0;
    }

    /* The sampled strike. Bend scales the table step with the wash
     * envelope, the same way the drum voices scale their pitch envelope. */
    attack = 0;
    if (c->attack_active != 0u) {
        attack = h5_cymbal_attack_sample(c->attack_phase_q16);
        attack = h5_mul_q16(attack, c->attack_gain_q16);
        attack = h5_mul_q16(attack, c->level_q16);

        step = c->attack_step_q16;
        if (c->bend_q16 != 0) {
            int32_t semitones = h5_mul_q16(c->bend_q16,
                (int32_t)h5_shr_round((int64_t)c->env_slow, 14u));
            int32_t ratio = h5_semitone_ratio_interp(semitones);

            step = h5_scale_q16(step, ratio);
        }
        if (step < 1) {
            step = 1;
        }
        c->attack_phase_q16 += step;
        if (c->attack_phase_q16
            >= (H5_CYMBAL_ATTACK_LEN << 16)) {
            c->attack_active = 0u;
        }
    }

    /* The wash. The filter cutoff follows the envelope when Sweep moves
     * it; otherwise the coefficients are already in place. */
    if (c->sweep_q16 != 0) {
        int32_t fc_attack = c->filter_freq_q16;
        int32_t fc_tail = h5_mul_q16(c->filter_freq_q16,
            H5_Q16_ONE - c->sweep_q16);
        int32_t cutoff;

        if (fc_tail < 20 * H5_Q16_ONE) {
            fc_tail = 20 * H5_Q16_ONE;
        }
        if (fc_tail > 18000 * H5_Q16_ONE) {
            fc_tail = 18000 * H5_Q16_ONE;
        }
        env16 = env >> 14;
        cutoff = fc_tail + h5_mul_q16(fc_attack - fc_tail, env16);
        h5_sweep_apply(&c->lp_sweep, cutoff, &c->noise_lp);
    }

    /* The high-pass runs at half scale: the innovation of a fast high-pass
     * exceeds one on a full-scale signal, and the half-scale loop keeps it
     * inside the Q31 range. The scale is restored after the filter chain. */
    u = h5_noise_next(&c->noise) >> 1;
    hp = h5_svf_run(&c->noise_hp, u);
    lp = h5_svf_run(&c->noise_lp, hp);
    bp = h5_svf_run(&c->tone_bp, hp);

    wash = h5_sat_add_q31(
        h5_mul_q16(h5_mul_q16(lp, c->noise_gain_q16),
            H5_CYMBAL_WASH_GAIN_Q16),
        h5_mul_q16(h5_mul_q16(bp, c->tone_gain_q16),
            H5_CYMBAL_TONE_GAIN_Q16));
    wash = h5_sat_i32((int64_t)wash << 1);
    wash = h5_mul_q30(wash, env);
    wash = h5_mul_q16(wash, c->tail_gain_q16);
    wash = h5_mul_q16(wash, c->level_q16);

    mix = h5_sat_add_q31(attack, wash);
    mix = h5_svf_run(&c->out_hp, mix);

    return h5_softclip_q31(mix);
}

const struct h5_engine_ops h5_cymbal_ops = {
    h5_cymbal_setup, h5_cymbal_configure, h5_cymbal_trigger,
    h5_cymbal_process
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
