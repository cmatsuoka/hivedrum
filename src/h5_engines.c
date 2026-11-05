/*
 * h5_engines.c - per-voice DSP engines.
 *
 * Every tone generator follows one architecture: a tonal branch (a sine
 * oscillator with a drive blend into a triangle branch), a sustained
 * noise branch (filtered noise under its own AR envelope) and a short
 * transient burst on the filtered noise. The Kick and Tom sum into the
 * soft-clip shaper and leave through a fixed output filter. The Snare
 * routes its swept low-pass through the rattle path only, so the shell
 * tone keeps its pitch and harmonics while Filter and Sweep colour the
 * noise.
 *
 *   - Tone path: the tone envelope is a linear AR whose value is cubed;
 *     the cube drives both the tone amplitude and the pitch exponent.
 *     The tone branch pairs a sine oscillator with the triangle of the
 *     same phase through the Drive blend: the triangle carries the odd
 *     harmonics that give the tone its body, the way the original
 *     device's triangle core does. The Kick tone attack is 0.005*Decay and
 *     the Drive blends the sine with the triangle branch, whose weight
 *     per engine differs; the triangle branch follows the body envelope
 *     so it decays with the sound.
 *   - Sustained noise: filtered noise * AR(noise_attack, Decay release) *
 *     noiseWeight.
 *   - Transient burst: AR^10 over the same filtered noise with a 1 ms
 *     attack, weighted Click^0.25.
 *   - Pitch envelope: f = Pitch * 1.5^(Bend*env^3/12) applies to the
 *     tonal branch of each voice.
 *   - Noise cutoff sweep: (NoisePitch + 2000*env_n^3)
 *     * (1 - FilterBend*(1 - env_n^3)); FilterBend 0.5 on the Kick and
 *     0.55/0.57/0.56 per Tom instance. The noise passes a fixed 50 Hz
 *     high-pass / 20 kHz low-pass band limit first.
 *   - Output: tanh(1.5 * mix) * 1.10. The saturation sits inside the
 *     curve, so a mix below full scale is compressed smoothly and the
 *     shaper never clips; the engine emits Q30 (1.0 = 1 << 30) with one
 *     headroom bit, and the mixer applies the user Volume exactly once.
 *     Kick and Tom add their fixed output filter after this stage. The
 *     Snare routes the swept low-pass through the rattle path only and
 *     leaves the mix through a fixed high-pass, so the shell tone and the
 *     click burst keep their full bandwidth while the wire rattle is shaped.
 *
 * Drive is the public blend into the triangle branch. Resonance and Gain
 * are engine constants (1.0, 1.0), not exposed parameters. Each trigger
 * restarts the oscillator phase, the envelopes and the filters; the noise
 * generator free-runs.
 *
 *   - Strike level: every hit carries a strike level, taken from the MIDI
 *     velocity. It scales the level of the whole mix, the depth of the
 *     pitch envelope (0.05 + 0.95*g of the Bend setting), the noise
 *     cutoff sweep (2000*g^4 Hz) and the weight of the transient
 *     (0.3 + 0.7*g^4 on the Kick, 0.4 + 0.6*g^4 on the Snare and the
 *     Tom), with g the strike level. The pitch curve and the swept-filter
 *     coefficients depend on it, so they are rebuilt at every trigger.
 *
 * The three engines share the parts of the per-sample chain that are
 * identical: the oscillator, the triangle, the transient burst, the body
 * mix and the output stage live in the shared helpers below. What stays
 * engine-specific is the branch structure each voice actually uses.
 *
 * One stage is a refined version that only LIBHIVE_XTRA_PRECISION keeps:
 * the Snare's second drive harmonic (about 40 dB below the fundamental),
 * which sits far below masking in the mixed output. The default build
 * omits it to keep the per-sample cost down.
 */

#include "h5_internal.h"

/* log2(1.5) in Q16.16. Scaling the bend depth by this converts the engine's
 * 1.5^(n/12) curve into 2^(n*log2(1.5)/12), which the semitone table covers. */
#define H5_BEND_LOG2_3_2_Q16 (38336)

/* Fixed output gain after the shaper: 1.10. */
#define H5_OUT_GAIN_Q16 (72090)

/* One-pole fixed cutoffs, Hz in Q16: the noise band limit (50 Hz high-pass
 * and 20 kHz low-pass). */
#define H5_PF_LO_HZ (50 * H5_Q16_ONE)
#define H5_PF_HI_HZ (20000 * H5_Q16_ONE)

/* Cutoff sweep contribution at env = 1, Hz in Q16. The engine sweeps the
 * noise cutoff by 2000*gain^4 Hz, so the amount scales with the fourth
 * power of the strike level. */
#define H5_NOISE_SWEEP_Q16 (2000 * H5_Q16_ONE)

/* The velocity laws. The pitch envelope sweeps 0.05 + 0.95*g of the Bend
 * setting, with g the strike level (0..1), so the bend tracks the strike
 * across the whole range and falls further on soft strikes. The strike level
 * is raised to the fourth power for the noise cutoff sweep (2000*g^4 Hz) and
 * the transient weight, which is floor + (1-floor)*g^4 with a floor of 0.3
 * for the Kick and 0.4 for the Snare and the Tom. */
#define H5_VEL_DEPTH_BASE_Q16 (5 * H5_Q16_ONE / 100)  /* 0.05 */
#define H5_VEL_DEPTH_GAIN_Q16 (95 * H5_Q16_ONE / 100) /* 0.95 */
#define H5_KICK_CLICK_FLOOR_Q16 (3 * H5_Q16_ONE / 10)  /* 0.3 */
#define H5_STD_CLICK_FLOOR_Q16 (4 * H5_Q16_ONE / 10)   /* 0.4 */

/* Kick path weights and constants, Q16. */
#define H5_KICK_TONE_WEIGHT_Q16 (85 * H5_Q16_ONE / 100) /* 0.85 */
#define H5_KICK_NOISE_WEIGHT_Q16 (4 * H5_Q16_ONE / 10) /* 0.4 */
#define H5_KICK_CLICK_WEIGHT_Q16 (66 * H5_Q16_ONE / 100) /* 0.66 */
#define H5_KICK_FILTER_BEND_Q16 (H5_Q16_ONE / 2)
#define H5_KICK_RESONANCE_Q16 H5_Q16_ONE
#define H5_KICK_OUT_HP_HZ 15
#define H5_KICK_OUT_Q16 (46341) /* 1/sqrt(2) */

/* Tom path constants, Q16. */
#define H5_TOM_RESONANCE_Q16 H5_Q16_ONE
#define H5_TOM_OUT_HP_HZ 25
#define H5_TOM_OUT_Q16 (46341) /* 1/sqrt(2) */
#define H5_TOM_NOISE_WEIGHT_Q16 (45 * H5_Q16_ONE / 100)
#define H5_TOM_CLICK_GAIN_Q16 (75 * H5_Q16_ONE / 100)
#define H5_TOM_TONE_SCALE_Q16 (46 * H5_Q16_ONE / 100)
#define H5_TOM_FILTER_BEND_1_Q16 (55 * H5_Q16_ONE / 100)
#define H5_TOM_FILTER_BEND_2_Q16 (57 * H5_Q16_ONE / 100)
#define H5_TOM_FILTER_BEND_3_Q16 (56 * H5_Q16_ONE / 100)

/* Snare path constants, Q16. */
#define H5_SNARE_OUT_Q16 H5_Q16_ONE
#define H5_SNARE_ATTACK_Q16 H5_MS_Q16(1)
#define H5_SNARE_CLICK_ATTACK_Q16 H5_MS_Q16(1)
#define H5_SNARE_NOISE_WEIGHT_Q16 (3 * H5_Q16_ONE / 10)
#define H5_SNARE_CLICK_WEIGHT_Q16 (63 * H5_Q16_ONE / 80)
#define H5_SNARE_SWEEP_DEPTH_Q16 H5_Q16_ONE

/* The Snare's filter-envelope contour depth. The original snare card has no
 * sweep knob, so the engine's default value is used and is not exposed as a
 * parameter. */
#define H5_SNARE_SWEEP_Q16 30802

/* Output high-pass after the swept low-pass. A 200 Hz Butterworth section
 * thins the default shell tone too much; 50 Hz removes sub-bass rumble
 * while leaving the 95.8 Hz fundamental essentially level. */
#define H5_SNARE_OUT_HP_HZ (50 * H5_Q16_ONE)
#define H5_SNARE_OUT_HP_Q16 (46341) /* 1/sqrt(2), Butterworth */

/* The Snare's drive stage. The driven tone carries a third harmonic about
 * 24 dB below its fundamental (always rendered) and a second harmonic about
 * 40 dB below it (LIBHIVE_XTRA_PRECISION only; it is below masking in the
 * mix). The third harmonic is what carries the drive, but it is kept softer
 * than in the earlier Kick/Tom-style calibration so the shell tone does not
 * project too many harmonics and sound "melodic". The weights are relative
 * to the fundamental, which the core keeps at full scale. */
#define H5_SNARE_DRIVE_H2_Q16 (642)   /* 0.0098 */
#define H5_SNARE_DRIVE_H3_Q16 (4000)  /* 0.061 */

/* The drive harmonics add to the peak of the tone core, so the core is
 * scaled down to leave headroom. The tone gain recovers most of that
 * headroom while keeping the shell tone slightly behind the rattle.
 * Without this the core would clip on every hit. */
#define H5_SNARE_DRIVE_HEADROOM_Q16 (58368) /* 0.891 */
#define H5_SNARE_TONE_BOOST_Q16 (49152)    /* 0.750 */

/* Rattle bank: the wire noise is broadband with four resonances. The
 * measured engine spectrum shows a wire body near 755 Hz, a strong narrow
 * wire peak near 1.07 kHz, a broad upper-mid lift around 5.5 kHz, and a
 * high rattle/click band near 12 kHz. The broadband term carries the
 * noise floor; the resonances shape the body and the attack snap. */
#define H5_SNARE_RATTLE_FREQ_1 (755 * H5_Q16_ONE)
#define H5_SNARE_RATTLE_FREQ_2 (1070 * H5_Q16_ONE)
#define H5_SNARE_RATTLE_FREQ_3 (5500 * H5_Q16_ONE)
#define H5_SNARE_RATTLE_FREQ_4 (12000 * H5_Q16_ONE)
#define H5_SNARE_RATTLE_Q_1 (H5_Q16_ONE)
#define H5_SNARE_RATTLE_Q_2 (7 * H5_Q16_ONE / 2)
#define H5_SNARE_RATTLE_Q_3 (7 * H5_Q16_ONE / 10)
#define H5_SNARE_RATTLE_Q_4 (4 * H5_Q16_ONE / 5)
#define H5_SNARE_RATTLE_WIRE_Q16 (H5_Q16_ONE)
#define H5_SNARE_RATTLE_WEIGHT_1 (245760)  /* 3.75 */
#define H5_SNARE_RATTLE_WEIGHT_2 (196608)  /* 3.00 */
#define H5_SNARE_RATTLE_WEIGHT_3 (98304)   /* 1.50 */
#define H5_SNARE_RATTLE_WEIGHT_4 (98304)   /* 1.50 */

/* Milliseconds to Q16 seconds (round-half-up). */
#define H5_MS_Q16(ms) (((ms) * H5_Q16_ONE + 500) / 1000)

/* Q30 unity: the one-pole coefficient that passes the input through. */
#define H5_PF_PASS_Q30 (1 << 30)

/* The one-pole coefficient k = tan/(1+tan) in Q30. A cutoff at or above
 * Nyquist is bypassed: tan(pi*f/sr) is zero at sr/2 and negative beyond it,
 * so the analogue prototype has no usable coefficient there. h5_svf_tune()
 * guards its swept filters the same way by clamping their cutoffs below
 * Nyquist. */
static int32_t h5_pf_k(int32_t f_q16, uint32_t sample_rate)
{
    int32_t t;
    int64_t den, q;

    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }
    if (f_q16 <= 0
        || (uint64_t)(uint32_t)f_q16 * 2u
            >= (uint64_t)sample_rate * H5_Q16_ONE) {
        return H5_PF_PASS_Q30;
    }
    t = h5_tanpi_q16(f_q16, sample_rate);
    den = (int64_t)H5_Q16_ONE + t;
    if (t <= 0 || den <= 0) {
        return H5_PF_PASS_Q30;
    }
    q = ((int64_t)t << 30) / den;
    if (q >= H5_PF_PASS_Q30) {
        return H5_PF_PASS_Q30;
    }
    return (int32_t)q;
}

/* The fixed 50 Hz high-pass / 20 kHz low-pass band limit on the held noise.
 * The states live in the filter, so the band limit is stateful. */
static int32_t h5_pf_run(struct h5_pf_s *pf, int32_t x)
{
    int32_t hp, lp;

    lp = h5_sat_add_q31(pf->l50, h5_mul_q30(h5_sat_sub_q31(x, pf->l50),
        pf->k50));
    pf->l50 = lp;
    hp = h5_sat_sub_q31(x, lp);
    lp = h5_sat_add_q31(pf->l20, h5_mul_q30(h5_sat_sub_q31(hp, pf->l20),
        pf->k20));
    pf->l20 = lp;
    return lp;
}

/* Common fixed one-pole setup for one sample rate. */
static void h5_pf_setup(struct h5_pf_s *pf, uint32_t sample_rate)
{
    pf->k50 = h5_pf_k(H5_PF_LO_HZ, sample_rate);
    pf->k20 = h5_pf_k(H5_PF_HI_HZ, sample_rate);
}

/* x^10 in Q30: (v^8 * v^2), three squarings. */
static int32_t h5_pow10_q30(int32_t v)
{
    int32_t v2 = (int32_t)h5_shr_round((int64_t)v * v, 30u);
    int32_t v4 = (int32_t)h5_shr_round((int64_t)v2 * v2, 30u);
    int32_t v8 = (int32_t)h5_shr_round((int64_t)v4 * v4, 30u);

    return (int32_t)h5_shr_round((int64_t)v8 * v2, 30u);
}

/* Cutoff laws for the three swept filters, in Hz Q16. env16 is the cubed
 * envelope as Q16 (0..H5_Q16_ONE) and sweep_q16 is the sweep amount at
 * env = 1 (2000 Hz scaled by the strike level). These are called once per
 * sample with the current envelope and, for the table builders, at the two
 * endpoints. */
static int32_t h5_sweep_cutoff(int32_t noise_pitch_q16, int32_t bend_q16,
    int32_t env16, int32_t sweep_q16)
{
    int32_t base = h5_sat_i32((int64_t)noise_pitch_q16
        + h5_mul_q16(sweep_q16, env16));

    return h5_sat_i32((int64_t)base
        * (int64_t)(H5_Q16_ONE
            - h5_mul_q16(bend_q16, H5_Q16_ONE - env16))
        >> 16);
}

static int32_t h5_snare_cutoff(int32_t noise_pitch_q16, int32_t env16)
{
    int32_t sweep = h5_mul_q16(H5_SNARE_SWEEP_DEPTH_Q16, H5_SNARE_SWEEP_Q16);

    return h5_mul_q16(noise_pitch_q16,
        H5_Q16_ONE - h5_mul_q16(sweep, H5_Q16_ONE - env16));
}

/* The velocity laws of one hit. level_q16 is the strike level (0..1) and
 * click_floor_q16 is the transient weight of a silent strike. */
static void h5_velocity_compute(struct h5_velocity_s *vel, int32_t level_q16,
    int32_t click_floor_q16)
{
    int32_t g2 = h5_scale_q16(level_q16, level_q16);
    int32_t g4 = h5_scale_q16(g2, g2);
    int32_t rest = H5_Q16_ONE - g4;

    vel->level_q16 = level_q16;
    /* The depth follows the strike level linearly, with a short floor, so the
     * bend changes audibly across the whole velocity range and drops further
     * on soft strikes instead of only near the top. */
    vel->depth_q16 = h5_sat_i32((int64_t)H5_VEL_DEPTH_BASE_Q16
        + h5_scale_q16(H5_VEL_DEPTH_GAIN_Q16, level_q16));
    vel->sweep_q16 = g4;
    vel->click_q16 = H5_Q16_ONE
        - h5_scale_q16(H5_Q16_ONE - click_floor_q16, rest);
}

/* The strike level of a hit at full velocity: the pitch envelope sweeps the
 * whole Bend setting, the noise cutoff sweeps the full 2000 Hz and the
 * transient carries its full weight. */
static void h5_velocity_full(struct h5_velocity_s *vel)
{
    vel->level_q16 = H5_Q16_ONE;
    vel->depth_q16 = H5_Q16_ONE;
    vel->sweep_q16 = H5_Q16_ONE;
    vel->click_q16 = H5_Q16_ONE;
}

/* The bend of this hit, in semitones: the setting scaled by the velocity
 * depth factor. */
static int32_t h5_velocity_bend(int32_t bend_q16, const struct h5_velocity_s *vel)
{
    return h5_scale_q16(bend_q16, vel->depth_q16);
}

/* The noise cutoff sweep amount of this hit, in Hz Q16. */
static int32_t h5_velocity_sweep(const struct h5_velocity_s *vel)
{
    return h5_scale_q16(H5_NOISE_SWEEP_Q16, vel->sweep_q16);
}

/* Oscillator increment for f = TonePitch * 1.5^(Bend*env^3/12). env3 is the
 * cubed Q30 envelope; the product with the Q16 bend depth is scaled by
 * log2(1.5) and looked up in the semitone table, which realizes 2^(n/12). */
static int32_t h5_pitch_inc(int32_t base_inc, int32_t bend_q16, int32_t env3)
{
    int32_t semitones_q16, ratio;

    semitones_q16 = (int32_t)h5_shr_round((int64_t)bend_q16 * env3, 30u);
    semitones_q16 = (int32_t)h5_shr_round(
        (int64_t)semitones_q16 * H5_BEND_LOG2_3_2_Q16, 16u);
    ratio = h5_semitone_ratio_interp(semitones_q16);
    return (int32_t)h5_shr_round(h5_mulwide(base_inc, ratio), 16u);
}

/* Fill the pitch-increment curve for one voice. Runs only from configure. */
static void h5_pitch_build(struct h5_pitch_curve_s *curve, int32_t base_inc,
    int32_t bend_q16)
{
    int i;

    for (i = 0; i <= H5_PITCH_POINTS; i++) {
        int32_t env3 = (int32_t)(((int64_t)i << 30) / H5_PITCH_POINTS);
        int32_t inc = h5_pitch_inc(base_inc, bend_q16, env3);

        curve->inc[i] = (inc < 1) ? 1 : inc;
    }
}

/* Linear interpolation over the precomputed curve. env3 is Q30. */
static int32_t h5_pitch_lookup(const struct h5_pitch_curve_s *curve,
    int32_t env3)
{
    uint32_t index, frac;
    int32_t a, b;

    if (env3 <= 0) {
        return curve->inc[0];
    }
    if (env3 >= (1 << 30)) {
        return curve->inc[H5_PITCH_POINTS];
    }

    index = (uint32_t)env3 >> H5_PITCH_SHIFT;
    frac = (uint32_t)env3 & ((1u << H5_PITCH_SHIFT) - 1u);
    a = curve->inc[index];
    b = curve->inc[index + 1u];
    return a + (int32_t)h5_shr_round(h5_mulwide(b - a, (int32_t)frac),
        H5_PITCH_SHIFT);
}

/* The shared pitch setup: the oscillator increment at TonePitch and the
 * precomputed increment curve. All three engines use it unchanged. */
static void h5_setup_pitch(struct h5_pitch_curve_s *curve, int32_t *base_inc,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate,
    int32_t depth_q16)
{
    /* cycles/sample = pitch/rate; the 28-bit phase unit adds 2^28. pitch is
     * Q16 Hz, so the numerator is pitch << 12. */
    int64_t inc = ((int64_t)params[H5_PARAM_TONE_PITCH] << 12) / sample_rate;

    if (inc < 1) {
        inc = 1;
    }
    *base_inc = (int32_t)inc;
    h5_pitch_build(curve, *base_inc, h5_scale_q16(params[H5_PARAM_BEND],
        depth_q16));
}

/* The tone oscillator: advance the phase by the increment the pitch curve
 * gives for the current envelope, then read the sine and the triangle of the
 * same phase.
 *
 * The triangle is the shape the tone core is built from: it starts at zero
 * and reaches its peak with the sine, and its odd harmonics are what give
 * the tone its body. The piecewise value rises over the first quarter of the
 * period, falls over the middle half and rises over the last quarter, peaking
 * at 2^30, so it needs no table. */
static void h5_tone_osc(uint32_t *phase, const struct h5_pitch_curve_s *curve,
    int32_t env3, int32_t *s, int32_t *st)
{
    int32_t inc = h5_pitch_lookup(curve, env3);
    uint32_t frac;
    int32_t d;

    *phase = (*phase + (uint32_t)inc) & 0x0FFFFFFFu;
    *s = h5_sine_interp(*phase);

    /* frac is the phase as a 30-bit fraction. The triangle rises over the
     * first quarter, falls over the middle half and rises over the last
     * quarter: it starts at zero with the sine, peaks where the sine peaks
     * and is an odd function of the phase, so it carries odd harmonics
     * only. The value is Q30, with the peak at 2^30. */
    frac = (*phase << 2) & 0x3FFFFFFFu;
    if (frac < (1u << 28)) {
        d = (int32_t)(frac << 2);
    } else if (frac < (3u << 28)) {
        d = (int32_t)((1ll << 31) - ((int64_t)frac << 2));
    } else {
        d = (int32_t)(((int64_t)frac << 2) - (1ll << 32));
    }
    *st = d;
}

/* The tone core: the sine and the triangle of the same phase, mixed with
 * the Drive weight. Drive 0 leaves the sine alone; Drive 1 is the triangle
 * alone. Both inputs are Q31, so the mix is Q31. */
static int32_t h5_tone_core(int32_t s, int32_t st, int32_t drive_q16)
{
    int32_t tri = h5_mul_q16(st, drive_q16);
    return h5_sat_add_q31(
        h5_mul_q16(s, H5_Q16_ONE - drive_q16),
        h5_sat_add_q31(tri, tri));
}

/* The transient burst: the source scaled by the click gain and by the tenth
 * power of the burst envelope. */
static int32_t h5_burst(int32_t src, int32_t gain_q16, int32_t env_c)
{
    int32_t burst = h5_mul_q16(src, gain_q16);

    return h5_mul_q30(burst, h5_pow10_q30(env_c));
}

/* The body mix: the tone under its gain plus the noise under its gain, both
 * under the cubed body envelope. The two gains carry their own Click
 * factors: the tone follows the soft (1-Click)^0.25 law and the noise the
 * steeper (1-Click)^0.7 one, so the control trades the noise body away much
 * faster than the tone. */
static int32_t h5_body_mix(int32_t core, int32_t noise_src, int32_t env3,
    int32_t tone_gain_q16, int32_t noise_gain_q16)
{
    int32_t tone = h5_mul_q16(core, tone_gain_q16);
    int32_t sum = h5_sat_add_q31(tone, h5_mul_q16(noise_src, noise_gain_q16));

    return h5_mul_q30(sum, env3);
}

/* The output stage: the soft-clip shaper, the fixed output gain and the
 * output filter. */
static int32_t h5_output_stage(int32_t mix, struct h5_svf_s *out_filter)
{
    int32_t out = h5_softclip_q31(mix);

    out = h5_mul_q16(out, H5_OUT_GAIN_Q16);
    return h5_svf_run(out_filter, out);
}

/* The noise path the Kick and the Tom share: the band-limited held noise
 * through the swept low-pass. */
static int32_t h5_noise_band(struct h5_pf_s *pf, struct h5_noise_s *noise,
    struct h5_svf_s *filter, struct h5_sweep_s *sweep, int32_t cutoff_q16)
{
    h5_sweep_apply(sweep, cutoff_q16, filter);
    return h5_svf_run(filter, h5_pf_run(pf, h5_noise_next(noise)));
}

/* Per-band rattle weights (with the folded output scale), Q16. */
static const int32_t h5_snare_rattle_weight[4] = {
    H5_SNARE_RATTLE_WEIGHT_1, H5_SNARE_RATTLE_WEIGHT_2,
    H5_SNARE_RATTLE_WEIGHT_3, H5_SNARE_RATTLE_WEIGHT_4
};

/*
 * Kick engine.
 *
 * Signal flow:
 *   env_n  = AR(~2 ms, Decay)
 *   env_t  = AR(0.005*Decay, Decay), used cubed
 *   env_c  = AR^10(1 ms, 0.1*Decay+0.04)
 *   fc_n   = (NoisePitch + 2000*env_n^3) * (1 - 0.5*(1 - env_n^3))
 *   nf     = SVF_LP(bandlimit(u_hold); fc_n, Resonance)
 *   noise  = nf * env_n * 0.4*(1-NoiseTone)^0.7 * (1-Click)^0.7
 *   click  = nf * env_c^10 * 0.66*Click^0.25
 *   f      = TonePitch * 1.5^(Bend*env_t^3/12)
 *   s      = sin(2*pi*phi), st = the triangle of the same phase
 *   core   = 0.55*s + 0.45*st
 *   tone   = env_t^3 * 0.85*NoiseTone^0.125*(1-Click)^0.25 * core
 *   out    = tanh(1.5*g*(tone+noise+click)) * 1.10 -> HP15
 *   (the single Decay control sets both the tone and noise envelopes;
 *    g is the strike level of the hit, and the bend depth and the noise
 *    sweep follow it as well)
 */

/* Apply the parameters to the Kick engine. A parameter change goes through
 * here; the engine state is left alone so a sounding voice is not cut. */
static void h5_kick_apply(void *engine,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    struct h5_kick_s *k = (struct h5_kick_s *)engine;
    int32_t click, decay;

    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }

    click = params[H5_PARAM_CLICK_TONE];
    decay = params[H5_PARAM_DECAY];

    k->bend_q16 = params[H5_PARAM_BEND];
    k->noise_pitch_q16 = params[H5_PARAM_NOISE_PITCH];
    k->drive_q16 = params[H5_PARAM_DRIVE];

    /* NoiseTone is the tone amount; Click splits tone and sustain noise
     * through mirrored x^0.7 curves. */
    k->tone_gain_q16 = h5_mul_q16(H5_KICK_TONE_WEIGHT_Q16,
        h5_mul_q16(h5_pow0125_q16(params[H5_PARAM_NOISE_TONE]),
            h5_pow025_q16(H5_Q16_ONE - click)));
    k->noise_gain_q16 = h5_mul_q16(H5_KICK_NOISE_WEIGHT_Q16,
        h5_mul_q16(h5_pow07_q16(H5_Q16_ONE - params[H5_PARAM_NOISE_TONE]),
            h5_pow07_q16(H5_Q16_ONE - click)));
    k->click_gain_q16 = h5_mul_q16(H5_KICK_CLICK_WEIGHT_Q16,
        h5_pow025_q16(click));

    h5_setup_pitch(&k->pitch_curve, &k->base_inc, params, sample_rate,
        k->vel.depth_q16);

    /* Envelopes. The tone env drives both amplitude and pitch and starts
     * fast; the noise env shares the tone release; the click burst keeps
     * a ~1 ms attack with its own 0.2*Decay+0.1 release. */
    h5_env_set_times(&k->env_tone, decay / 200, decay, sample_rate);
    h5_env_set_times(&k->env_noise, H5_MS_Q16(2), decay, sample_rate);
    h5_env_set_times(&k->env_click, H5_MS_Q16(1),
        (decay + (2 * H5_Q16_ONE) / 5) / 10, sample_rate);

    h5_pf_setup(&k->pf, sample_rate);
    h5_svf_tune(&k->out_filter, H5_SVF_HP,
        H5_KICK_OUT_HP_HZ * H5_Q16_ONE, H5_KICK_OUT_Q16, sample_rate);
    h5_sweep_build(&k->noise_sweep, H5_SVF_LP, H5_KICK_RESONANCE_Q16,
        h5_sweep_cutoff(k->noise_pitch_q16, H5_KICK_FILTER_BEND_Q16, 0,
            h5_velocity_sweep(&k->vel)),
        h5_sweep_cutoff(k->noise_pitch_q16, H5_KICK_FILTER_BEND_Q16,
            H5_Q16_ONE, h5_velocity_sweep(&k->vel)),
        sample_rate);
}

void h5_kick_setup(void *engine, h5_voice voice,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    struct h5_kick_s *k = (struct h5_kick_s *)engine;

    (void)voice;

    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }

    /* Initial state. */
    h5_env_init(&k->env_noise);
    h5_env_init(&k->env_tone);
    h5_env_init(&k->env_click);
    h5_noise_init_voice(&k->noise, H5_VOICE_KICK, sample_rate);
    h5_svf_reset(&k->noise_filter);
    h5_svf_reset(&k->out_filter);
    k->phase = 0u;
    k->base_inc = 0;
    k->bend_q16 = 0;
    k->noise_pitch_q16 = 0;
    k->tone_gain_q16 = 0;
    k->noise_gain_q16 = 0;
    k->click_gain_q16 = 0;
    k->pf.k50 = 0;
    k->pf.k20 = 0;
    k->pf.l50 = 0;
    k->pf.l20 = 0;
    k->head.active = 0u;
    h5_velocity_full(&k->vel);

    h5_kick_apply(engine, params, sample_rate);
}

void h5_kick_trigger(void *engine, int32_t level_q16,
    uint32_t sample_rate, uint32_t variant)
{
    struct h5_kick_s *k = (struct h5_kick_s *)engine;

    (void)variant; /* the Kick has no open character */

    /* The strike level of this hit: it scales the level of the whole hit,
     * the depth of the pitch envelope, the noise cutoff sweep and the
     * weight of the transient. The pitch curve and the swept-filter
     * coefficients depend on it, so both are rebuilt for the hit. */
    h5_velocity_compute(&k->vel, level_q16, H5_KICK_CLICK_FLOOR_Q16);
    h5_pitch_build(&k->pitch_curve, k->base_inc,
        h5_velocity_bend(k->bend_q16, &k->vel));
    h5_sweep_build(&k->noise_sweep, H5_SVF_LP, H5_KICK_RESONANCE_Q16,
        h5_sweep_cutoff(k->noise_pitch_q16, H5_KICK_FILTER_BEND_Q16, 0,
            h5_velocity_sweep(&k->vel)),
        h5_sweep_cutoff(k->noise_pitch_q16, H5_KICK_FILTER_BEND_Q16,
            H5_Q16_ONE, h5_velocity_sweep(&k->vel)),
        sample_rate);

    k->phase = 0u;
    h5_env_trigger(&k->env_noise);
    h5_env_trigger(&k->env_tone);
    h5_env_trigger(&k->env_click);
    h5_svf_reset(&k->noise_filter);
    h5_svf_reset(&k->out_filter);
    k->pf.l50 = 0;
    k->pf.l20 = 0;
    k->head.active = 1u;
}

int32_t h5_kick_process(void *engine)
{
    struct h5_kick_s *k = (struct h5_kick_s *)engine;
    int32_t env_n, env_t, env_c, tone3, env3;
    int32_t s, st, cutoff, nf, noise, trans, tone, sum;

    if (!k->head.active) {
        return 0;
    }

    env_n = h5_env_next(&k->env_noise);
    env_c = h5_env_next(&k->env_click);
    env_t = h5_env_next(&k->env_tone);
    if (env_n <= 0 && env_t <= 0 && env_c <= 0
        && k->env_noise.state == H5_ENV_DONE
        && k->env_tone.state == H5_ENV_DONE
        && k->env_click.state == H5_ENV_DONE) {
        k->head.active = 0u;
        return 0;
    }

    /* The tone envelope value, cubed once; the cube drives both the tone
     * amplitude and the pitch exponent below. */
    tone3 = h5_cube_q30(env_t);

    /* Pitch: f = TonePitch * 1.5^(Bend * tone3 / 12). */
    h5_tone_osc(&k->phase, &k->pitch_curve, tone3, &s, &st);

    /* Noise band limit then swept low-pass:
     * fc_n = (NoisePitch + 2000*env_n^3) * (1 - 0.5*(1 - env_n^3)). */
    env3 = h5_cube_q30(env_n);
    cutoff = h5_sweep_cutoff(k->noise_pitch_q16, H5_KICK_FILTER_BEND_Q16,
        env3 >> 14, h5_velocity_sweep(&k->vel));
    nf = h5_noise_band(&k->pf, &k->noise, &k->noise_filter, &k->noise_sweep,
        cutoff);

    /* Sustained noise: nf * env_n * 0.4*(1-NoiseTone)^0.7*(1-Click)^0.25. */
    noise = h5_mul_q30(nf, env_n);
    noise = h5_mul_q16(noise, k->noise_gain_q16);

    /* Transient burst: nf * env_c^10 * 0.66*Click^0.25. The burst
     * carries one headroom bit past the sustained branches. The branch
     * is silent once the burst envelope has decayed, so skip its work. */
    trans = 0;
    if (env_c > 0) {
        trans = h5_burst(nf,
            h5_scale_q16(k->click_gain_q16, k->vel.click_q16), env_c);
    }

    /* Tone: env^3 * 0.85*NoiseTone^0.125*(1-Click)^0.25
     *       * ((1-Drive)*s + 2*Drive*st). */
    tone = h5_mul_q30(h5_tone_core(s, st, k->drive_q16), tone3);
    tone = h5_mul_q16(tone, k->tone_gain_q16);

    sum = h5_sat_add_q31(tone, noise);
    sum = h5_sat_add_q31(sum, trans);

    /* The strike level scales the whole hit before the shaper, so a soft
     * hit is quieter and the saturation curve sees a smaller argument. */
    sum = h5_scale_q16(sum, k->vel.level_q16);

    /* Fixed output gain 1.10479 into the 15 Hz high-pass. */
    return h5_output_stage(sum, &k->out_filter);
}

/*
 * Tom engine.
 *
 * Signal flow:
 *   env     = AR^3(0.005*Decay, Decay)
 *   env_c   = AR^10(1 ms, 0.1*Decay+0.04)
 *   fc_n    = (NoisePitch + 2000*env^3) * (1 - FB*(1 - env^3))
 *   nf      = SVF_LP(bandlimit(u_hold); fc_n, Resonance)
 *   body    = env^3 * (0.46*NoiseTone^0.125*(1-Click)^0.25*core
 *                     + 0.45*(1-NoiseTone)^0.7*(1-Click)^0.7*nf)
 *   click   = env_c^10 * 0.75*Click^0.25 * nf
 *   out     = tanh(1.5*g*mix) * 1.10 -> HP25
 *   (each Tom instance tunes its own filter-bend depth; g is the strike
 *    level of the hit, and the bend depth and the noise sweep follow it)
 */

/* Apply the parameters to the Tom engine. */
static void h5_tom_apply(void *engine,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    struct h5_tom_s *t = (struct h5_tom_s *)engine;
    int32_t noise_tone, click, decay;

    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }

    noise_tone = params[H5_PARAM_NOISE_TONE];
    click = params[H5_PARAM_CLICK_TONE];
    decay = params[H5_PARAM_DECAY];

    t->bend_q16 = params[H5_PARAM_BEND];
    t->noise_pitch_q16 = params[H5_PARAM_NOISE_PITCH];
    t->drive_q16 = params[H5_PARAM_DRIVE];

    t->tone_gain_q16 = h5_mul_q16(h5_pow0125_q16(noise_tone),
        h5_mul_q16(H5_TOM_TONE_SCALE_Q16,
            h5_pow025_q16(H5_Q16_ONE - click)));
    t->noise_gain_q16 = h5_mul_q16(
        h5_mul_q16(h5_pow07_q16(H5_Q16_ONE - noise_tone),
            H5_TOM_NOISE_WEIGHT_Q16),
        h5_pow07_q16(H5_Q16_ONE - click));
    t->click_gain_q16 = h5_mul_q16(h5_pow025_q16(click),
        H5_TOM_CLICK_GAIN_Q16);

    h5_setup_pitch(&t->pitch_curve, &t->base_inc, params, sample_rate,
        t->vel.depth_q16);

    /* Body envelope: AR^3 with a 0.005*Decay attack and the Decay release.
     * Transient envelope: AR^10, 1 ms attack, 0.25*Decay release. */
    h5_env_set_times(&t->env, decay / 200, decay, sample_rate);
    h5_env_set_times(&t->env_click, H5_MS_Q16(1),
        (decay + (2 * H5_Q16_ONE) / 5) / 10, sample_rate);

    h5_pf_setup(&t->pf, sample_rate);
    h5_svf_tune(&t->out_filter, H5_SVF_HP,
        H5_TOM_OUT_HP_HZ * H5_Q16_ONE, H5_TOM_OUT_Q16, sample_rate);
    h5_sweep_build(&t->noise_sweep, H5_SVF_LP, H5_TOM_RESONANCE_Q16,
        h5_sweep_cutoff(t->noise_pitch_q16, t->filter_bend_q16, 0,
            h5_velocity_sweep(&t->vel)),
        h5_sweep_cutoff(t->noise_pitch_q16, t->filter_bend_q16, H5_Q16_ONE,
            h5_velocity_sweep(&t->vel)),
        sample_rate);
}

void h5_tom_setup(void *engine, h5_voice voice,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    struct h5_tom_s *t = (struct h5_tom_s *)engine;

    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }

    /* Initial state. */
    h5_env_init(&t->env);
    h5_env_init(&t->env_click);
    h5_noise_init_voice(&t->noise, voice, sample_rate);
    h5_svf_reset(&t->noise_filter);
    h5_svf_reset(&t->out_filter);
    t->phase = 0u;
    t->base_inc = 0;
    t->bend_q16 = 0;
    t->noise_pitch_q16 = 0;
    t->filter_bend_q16 = H5_TOM_FILTER_BEND_1_Q16;
    t->tone_gain_q16 = 0;
    t->noise_gain_q16 = 0;
    t->click_gain_q16 = 0;
    t->pf.k50 = 0;
    t->pf.k20 = 0;
    t->pf.l50 = 0;
    t->pf.l20 = 0;
    t->head.active = 0u;
    h5_velocity_full(&t->vel);
    if (voice == H5_VOICE_TOM_2) {
        t->filter_bend_q16 = H5_TOM_FILTER_BEND_2_Q16;
    } else if (voice == H5_VOICE_TOM_3) {
        t->filter_bend_q16 = H5_TOM_FILTER_BEND_3_Q16;
    }

    h5_tom_apply(engine, params, sample_rate);
}

void h5_tom_trigger(void *engine, int32_t level_q16,
    uint32_t sample_rate, uint32_t variant)
{
    struct h5_tom_s *t = (struct h5_tom_s *)engine;

    (void)variant; /* the Toms have no open character */

    /* The strike level of this hit; see the Kick engine for the laws. */
    h5_velocity_compute(&t->vel, level_q16, H5_STD_CLICK_FLOOR_Q16);
    h5_pitch_build(&t->pitch_curve, t->base_inc,
        h5_velocity_bend(t->bend_q16, &t->vel));
    h5_sweep_build(&t->noise_sweep, H5_SVF_LP, H5_TOM_RESONANCE_Q16,
        h5_sweep_cutoff(t->noise_pitch_q16, t->filter_bend_q16, 0,
            h5_velocity_sweep(&t->vel)),
        h5_sweep_cutoff(t->noise_pitch_q16, t->filter_bend_q16, H5_Q16_ONE,
            h5_velocity_sweep(&t->vel)),
        sample_rate);

    t->phase = 0u;
    h5_env_trigger(&t->env);
    h5_env_trigger(&t->env_click);
    h5_svf_reset(&t->noise_filter);
    h5_svf_reset(&t->out_filter);
    t->pf.l50 = 0;
    t->pf.l20 = 0;
    t->head.active = 1u;
}

int32_t h5_tom_process(void *engine)
{
    struct h5_tom_s *t = (struct h5_tom_s *)engine;
    int32_t env, env_click, env3;
    int32_t s, st, cutoff, nf, body, click, mix;

    if (!t->head.active) {
        return 0;
    }

    env = h5_env_next(&t->env);
    env_click = h5_env_next(&t->env_click);
    if (env <= 0 && env_click <= 0
        && t->env.state == H5_ENV_DONE
        && t->env_click.state == H5_ENV_DONE) {
        t->head.active = 0u;
        return 0;
    }

    env3 = h5_cube_q30(env);

    /* Pitch envelope: f = TonePitch * 1.5^(Bend * env^3 / 12). */
    h5_tone_osc(&t->phase, &t->pitch_curve, env3, &s, &st);

    /* fc_n = (NoisePitch + 2000*env^3)*(1 - FB*(1 - env^3)). */
    cutoff = h5_sweep_cutoff(t->noise_pitch_q16, t->filter_bend_q16,
        env3 >> 14, h5_velocity_sweep(&t->vel));
    nf = h5_noise_band(&t->pf, &t->noise, &t->noise_filter, &t->noise_sweep,
        cutoff);

    /* Body: env^3 * (0.46*NoiseTone^0.125*core
     *                 + 0.45*(1-NoiseTone)^0.7*(1-Click)^0.7*noise). */
    body = h5_body_mix(h5_tone_core(s, st, t->drive_q16), nf, env3,
        t->tone_gain_q16, t->noise_gain_q16);

    /* Transient burst: env_c^10 * Click^0.7 * nf. Skip it once the burst
     * envelope is silent. */
    click = 0;
    if (env_click > 0) {
        click = h5_burst(nf,
            h5_scale_q16(t->click_gain_q16, t->vel.click_q16), env_click);
    }
    mix = h5_sat_add_q31(body, click);

    /* The strike level scales the whole hit before the shaper. */
    mix = h5_scale_q16(mix, t->vel.level_q16);

    return h5_output_stage(mix, &t->out_filter);
}

/*
 * Snare engine.
 *
 * Signal flow:
 *   env     = AR^3(1 ms, Decay)
 *   env_c   = AR^10(1 ms, 0.1*Decay+0.04)
 *   rattle  = broadband wire noise plus three weighted band-passes over
 *             the band-limited held noise, then a swept low-pass
 *   body    = env^3 * (NoiseTone^0.125*(1-Click)^0.25*core
 *                     + 0.3*(1-NoiseTone)^0.7*(1-Click)^0.7*rattle)
 *   click   = env_c^10 * 63/80*Click^0.25 * rattle
 *   mix     = body + click
 *   out     = tanh(1.5*g*mix) * 1.10 -> HP_SVF(~50 Hz, Q = 1/sqrt(2))
 *
 * The strike level g of the hit scales the whole mix and the transient, and
 * the bend depth follows it; the rattle path's filter contour is the
 * engine's default, because the original snare card has no sweep knob.
 *
 * The swept low-pass sits on the rattle path only, so the shell tone and
 * the click burst keep their full bandwidth. The fixed high-pass thins
 * the shell tone and removes sub-bass rumble.
 */

/* Apply the parameters to the Snare engine. */
static void h5_snare_apply(void *engine,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    struct h5_snare_s *sn = (struct h5_snare_s *)engine;
    int32_t noise_tone, click, decay;

    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }

    noise_tone = params[H5_PARAM_NOISE_TONE];
    click = params[H5_PARAM_CLICK_TONE];
    decay = params[H5_PARAM_DECAY];

    sn->bend_q16 = params[H5_PARAM_BEND];
    sn->noise_pitch_q16 = params[H5_PARAM_NOISE_PITCH];
    sn->drive_q16 = params[H5_PARAM_DRIVE];

    /* The Snare's Noise/Tone control is a balance, not a tone amount: the
     * shell tone follows the eighth root of the setting (NoiseTone^0.125)
     * while the rattle follows (1-NoiseTone)^0.7, so both sides move together
     * and the knob sweeps from a bare shell to a bare rattle. The eighth root
     * is three integer square roots, so no table is needed. */
    sn->tone_gain_q16 = h5_mul_q16(h5_mul_q16(h5_pow0125_q16(noise_tone),
        h5_pow025_q16(H5_Q16_ONE - click)), H5_SNARE_TONE_BOOST_Q16);
    sn->noise_gain_q16 = h5_mul_q16(
        h5_mul_q16(h5_pow07_q16(H5_Q16_ONE - noise_tone),
            H5_SNARE_NOISE_WEIGHT_Q16),
        h5_pow07_q16(H5_Q16_ONE - click));
    sn->click_gain_q16 = h5_mul_q16(h5_pow025_q16(click),
        H5_SNARE_CLICK_WEIGHT_Q16);

    h5_setup_pitch(&sn->pitch_curve, &sn->base_inc, params, sample_rate,
        sn->vel.depth_q16);

    /* Body envelope: AR^3, 1 ms attack, Decay release. Transient: AR^10,
     * 1 ms attack, a short release that follows the decay loosely. */
    h5_env_set_times(&sn->env, H5_SNARE_ATTACK_Q16, decay, sample_rate);
    h5_env_set_times(&sn->env_click, H5_SNARE_CLICK_ATTACK_Q16,
        (decay + (2 * H5_Q16_ONE) / 5) / 10, sample_rate);

    /* Rattle resonators and output filters: parameter change. The swept
     * low-pass sits on the rattle path only; the fixed high-pass sits on
     * the mixed output. */
    h5_pf_setup(&sn->pf, sample_rate);
    h5_svf_tune(&sn->rattle[0], H5_SVF_BP, H5_SNARE_RATTLE_FREQ_1,
        H5_SNARE_RATTLE_Q_1, sample_rate);
    h5_svf_tune(&sn->rattle[1], H5_SVF_BP, H5_SNARE_RATTLE_FREQ_2,
        H5_SNARE_RATTLE_Q_2, sample_rate);
    h5_svf_tune(&sn->rattle[2], H5_SVF_BP, H5_SNARE_RATTLE_FREQ_3,
        H5_SNARE_RATTLE_Q_3, sample_rate);
    h5_svf_tune(&sn->rattle[3], H5_SVF_BP, H5_SNARE_RATTLE_FREQ_4,
        H5_SNARE_RATTLE_Q_4, sample_rate);
    h5_svf_tune(&sn->out_filter, H5_SVF_LP, params[H5_PARAM_NOISE_PITCH],
        H5_SNARE_OUT_Q16, sample_rate);
    h5_svf_tune(&sn->hp_filter, H5_SVF_HP, H5_SNARE_OUT_HP_HZ,
        H5_SNARE_OUT_HP_Q16, sample_rate);
    h5_sweep_build(&sn->out_sweep, H5_SVF_LP, H5_SNARE_OUT_Q16,
        h5_snare_cutoff(sn->noise_pitch_q16, 0),
        h5_snare_cutoff(sn->noise_pitch_q16, H5_Q16_ONE),
        sample_rate);
}

void h5_snare_setup(void *engine, h5_voice voice,
    const int32_t params[H5_PARAM_COUNT], uint32_t sample_rate)
{
    struct h5_snare_s *sn = (struct h5_snare_s *)engine;
    size_t i;

    (void)voice;

    if (sample_rate == 0u) {
        sample_rate = H5_DEFAULT_SAMPLE_RATE;
    }

    /* Initial state. */
    h5_env_init(&sn->env);
    h5_env_init(&sn->env_click);
    h5_noise_init_voice(&sn->noise, H5_VOICE_SNARE, sample_rate);
    for (i = 0u; i < 4u; i++) {
        h5_svf_reset(&sn->rattle[i]);
    }
    h5_svf_reset(&sn->out_filter);
    h5_svf_reset(&sn->hp_filter);
    sn->phase = 0u;
    sn->base_inc = 0;
    sn->bend_q16 = 0;
    sn->noise_pitch_q16 = 0;
    sn->tone_gain_q16 = 0;
    sn->noise_gain_q16 = 0;
    sn->click_gain_q16 = 0;
    sn->pf.k50 = 0;
    sn->pf.k20 = 0;
    sn->pf.l50 = 0;
    sn->pf.l20 = 0;
    sn->head.active = 0u;
    h5_velocity_full(&sn->vel);

    h5_snare_apply(engine, params, sample_rate);
}

void h5_snare_trigger(void *engine, int32_t level_q16,
    uint32_t sample_rate, uint32_t variant)
{
    struct h5_snare_s *sn = (struct h5_snare_s *)engine;
    size_t i;

    (void)variant; /* the Snare has no open character */

    /* The strike level of this hit; see the Kick engine for the laws. The
     * pitch curve depends on it, so it is rebuilt for the hit. */
    (void)sample_rate;
    h5_velocity_compute(&sn->vel, level_q16, H5_STD_CLICK_FLOOR_Q16);
    h5_pitch_build(&sn->pitch_curve, sn->base_inc,
        h5_velocity_bend(sn->bend_q16, &sn->vel));

    sn->phase = 0u;
    h5_env_trigger(&sn->env);
    h5_env_trigger(&sn->env_click);
    for (i = 0u; i < 4u; i++) {
        h5_svf_reset(&sn->rattle[i]);
    }
    h5_svf_reset(&sn->out_filter);
    h5_svf_reset(&sn->hp_filter);
    sn->pf.l50 = 0;
    sn->pf.l20 = 0;
    sn->head.active = 1u;
}

int32_t h5_snare_process(void *engine)
{
    struct h5_snare_s *sn = (struct h5_snare_s *)engine;
    int32_t env, env_click, env3, s, st, s3, core, u;
#ifdef LIBHIVE_XTRA_PRECISION
    int32_t s2;
#endif
    int32_t rattle, body, click, mix;
    int32_t cutoff;
    size_t i;

    if (!sn->head.active) {
        return 0;
    }

    env = h5_env_next(&sn->env);
    env_click = h5_env_next(&sn->env_click);
    if (env <= 0 && env_click <= 0
        && sn->env.state == H5_ENV_DONE
        && sn->env_click.state == H5_ENV_DONE) {
        sn->head.active = 0u;
        return 0;
    }

    env3 = h5_cube_q30(env);

    /* Pitch envelope: f = TonePitch * 1.5^(Bend * env^3 / 12). */
    h5_tone_osc(&sn->phase, &sn->pitch_curve, env3, &s, &st);

    /* The drive stage: a sine-heavy core plus (with LIBHIVE_XTRA_PRECISION)
     * a second harmonic about 40 dB down and a subtracted third harmonic.
     * The Snare shell needs to sit behind the rattle, so the triangle weight
     * is lower than Kick/Tom to keep the tone from dominating with odd
     * harmonics. The third harmonic of the drive is subtracted so it lifts
     * the triangle's third harmonic rather than cancelling it. */
#ifdef LIBHIVE_XTRA_PRECISION
    s2 = h5_sine_interp(sn->phase * 2u);
#endif
    s3 = h5_sine_interp(sn->phase * 3u);
    int32_t snare_core = h5_tone_core(s, st, sn->drive_q16);

#ifdef LIBHIVE_XTRA_PRECISION
    core = h5_sat_add_q31(snare_core,
        h5_sat_add_q31(h5_mul_q16(s2, H5_SNARE_DRIVE_H2_Q16),
            -h5_mul_q16(s3, H5_SNARE_DRIVE_H3_Q16)));
#else
    core = h5_sat_sub_q31(snare_core,
        h5_mul_q16(s3, H5_SNARE_DRIVE_H3_Q16));
#endif
    core = h5_mul_q16(core, H5_SNARE_DRIVE_HEADROOM_Q16);

    /* The rattle: broadband wire noise plus four resonances. */
    u = h5_pf_run(&sn->pf, h5_noise_next(&sn->noise));
    rattle = h5_mul_q16(u, H5_SNARE_RATTLE_WIRE_Q16);
    for (i = 0u; i < 4u; i++) {
        rattle = h5_sat_add_q31(rattle, h5_mul_q16(
            h5_svf_run(&sn->rattle[i], u), h5_snare_rattle_weight[i]));
    }

    /* The rattle passes the swept low-pass. Keeping the sweep on the rattle
     * path only leaves the shell tone and the click burst at full bandwidth,
     * which gives the attack its snap and keeps the tone pitch clean. */
    cutoff = h5_snare_cutoff(sn->noise_pitch_q16, env3 >> 14);
    h5_sweep_apply(&sn->out_sweep, cutoff, &sn->out_filter);
    rattle = h5_svf_run(&sn->out_filter, rattle);

    /* Body: env^3 * (1-Click)^0.25
     *       * (NoiseTone^0.125*core + 0.3*(1-NoiseTone)^0.7*rattle). */
    body = h5_body_mix(core, rattle, env3, sn->tone_gain_q16,
        sn->noise_gain_q16);

    /* Transient: env_c^10 * Click^0.25 * rattle. Skip it once the burst
     * envelope is silent. */
    click = 0;
    if (env_click > 0) {
        click = h5_burst(rattle,
            h5_scale_q16(sn->click_gain_q16, sn->vel.click_q16), env_click);
    }
    mix = h5_sat_add_q31(body, click);

    /* The strike level scales the whole hit before the shaper. */
    mix = h5_scale_q16(mix, sn->vel.level_q16);

    /* Output stage: tanh, the fixed 1.10 gain, and a fixed high-pass. */
    mix = h5_mul_q16(h5_softclip_q31(mix), H5_OUT_GAIN_Q16);
    return h5_svf_run(&sn->hp_filter, mix);
}

/* The engine operation sets. The voice table in h5_voice.c pairs each set
 * with the engine state it belongs to. */
const struct h5_engine_ops h5_kick_ops = {
    h5_kick_setup, h5_kick_apply, h5_kick_trigger, h5_kick_process
};

const struct h5_engine_ops h5_snare_ops = {
    h5_snare_setup, h5_snare_apply, h5_snare_trigger, h5_snare_process
};

const struct h5_engine_ops h5_tom_ops = {
    h5_tom_setup, h5_tom_apply, h5_tom_trigger, h5_tom_process
};
