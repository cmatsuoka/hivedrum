/*
 * h5.h - public API of the Hive-V library.
 *
 * Hive-V is a seven-voice percussion synthesizer arranged as a drum kit:
 * Kick, Snare, Tom 1, Tom 2, Tom 3, Cymbal and Hi-hat. The library renders a
 * single stereo mix into a caller-supplied buffer. It has no I/O and no
 * floating point. A context is created with h5_create_context().
 *
 * Prefixes: macros and enum constants use H5_, functions and types use h5_.
 * The context is an opaque handle (h5_context).
 */

#ifndef HIVE_V_H5_H
#define HIVE_V_H5_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Library version. major/minor/patch are packed as 0x00MMmmpp.
 * The plain integer constants keep H5_VERSION_NUMBER usable in #if. */
#define H5_VERSION_MAJOR 0
#define H5_VERSION_MINOR 6
#define H5_VERSION_PATCH 0
#define H5_VERSION_NUMBER \
    ((H5_VERSION_MAJOR << 16) | (H5_VERSION_MINOR << 8) | H5_VERSION_PATCH)

/* Sample rate used when the caller passes 0 to h5_create_context() or
 * h5_set_sample_rate(). */
#define H5_DEFAULT_SAMPLE_RATE 44100u

/* Q16.16 fixed-point unit: 1.0 is represented by this integer value. */
#define H5_Q16_ONE 65536

/* The opaque context. */
typedef void *h5_context;

/* Return the version of the library the process is linked against, packed as
 * H5_VERSION_NUMBER. This reports the compiled library, so a host can compare
 * it with the header's H5_VERSION_NUMBER to detect a mismatched build. */
uint32_t h5_version_number(void);

/* The seven voice instances, built from four engine types. The Tom engine is
 * instantiated three times so the Toms have independent state and can sound
 * simultaneously. The metallic engine serves the Cymbal and the Hi-hat. */
typedef enum {
    H5_VOICE_KICK = 0,
    H5_VOICE_SNARE,
    H5_VOICE_TOM_1,
    H5_VOICE_TOM_2,
    H5_VOICE_TOM_3,
    H5_VOICE_CYMBAL,
    H5_VOICE_HIHAT,
    H5_VOICE_COUNT
} h5_voice;

/* The eight trigger targets. Each Tom instrument triggers its own Tom voice.
 * The Cymbal instrument triggers the Cymbal voice. The Hi-hat is one voice
 * reachable through two instruments: H5_INSTR_HIHAT_OPEN plays the open
 * character with a longer release, H5_INSTR_HIHAT_CLOSED plays the closed
 * character; triggering the closed instrument chokes a sounding open hat.
 * Both instruments share the Hi-hat voice, its parameters and its mixer
 * values. */
typedef enum {
    H5_INSTR_KICK = 0,
    H5_INSTR_SNARE,
    H5_INSTR_TOM_1,
    H5_INSTR_TOM_2,
    H5_INSTR_TOM_3,
    H5_INSTR_CYMBAL,
    H5_INSTR_HIHAT_CLOSED,
    H5_INSTR_HIHAT_OPEN,
    H5_INSTR_COUNT
} h5_instrument;

/* The metallic voices are optional engines. The Hi-hat is compiled in behind
 * LIBHIVE_ENGINE_HIHAT and the Cymbal behind LIBHIVE_ENGINE_CYMBAL; both are
 * defined by default. A voice whose engine is not compiled in stays in this
 * API, but it renders silence and supports no parameter:
 * h5_param_supported() reports false for it and the setters and getters
 * return H5_ERR_UNSUPPORTED. */

/* Public sound parameters. The standard drum voices (Kick, Snare, Tom 1,
 * Tom 2, Tom 3) share the six faceplate controls Noise, Tone, Bend, Decay,
 * Noise/Tone and Click/Drum. The metallic voices (Cymbal, Hi-hat) have their
 * own faceplate: Sweep, Tone Pitch, Bend, Decay, Noise/Tone and Bell/Cymbal,
 * so they support H5_PARAM_SWEEP and H5_PARAM_BELL_CYMBAL instead of
 * H5_PARAM_NOISE_PITCH and H5_PARAM_CLICK_TONE.
 *
 * Volume and Pan are the per-voice mixer controls and Sensitivity is the
 * per-voice strike sensitivity; all three are supported on every voice. Not
 * every parameter applies to every voice; h5_param_supported() reports the
 * supported pairs. */
typedef enum {
    H5_PARAM_NOISE_PITCH = 0,
    H5_PARAM_TONE_PITCH,
    H5_PARAM_BEND,
    H5_PARAM_DECAY,
    H5_PARAM_NOISE_TONE,
    H5_PARAM_CLICK_TONE,
    H5_PARAM_SWEEP,
    H5_PARAM_BELL_CYMBAL,
    H5_PARAM_VOLUME,
    H5_PARAM_PAN,
    H5_PARAM_SENSITIVITY,
    H5_PARAM_FX_SEND,
    H5_PARAM_COUNT
} h5_param;

/* The shared FX bus parameters. The bus holds one reverb and one noise gate;
 * a voice feeds it through H5_PARAM_FX_SEND. The bus is active exactly when
 * at least one send is non-zero. Scalar parameters are Q16.16 in 0..1 and
 * the time parameters are Q16.16 seconds. */
typedef enum {
    H5_FX_ROOM_SIZE = 0,  /* 0 -> 0.6x room, 1 -> 2.0x, linear */
    H5_FX_DAMPING,        /* 0 -> bright tail, 1 -> dark tail */
    H5_FX_WIDTH,          /* 1 -> full stereo, 0 -> mono */
    H5_FX_WET,            /* return trim after the gate */
    H5_FX_GATE_THRESHOLD, /* pre-reverb key threshold, linear amplitude */
    H5_FX_GATE_ATTACK,    /* seconds */
    H5_FX_GATE_HOLD,      /* seconds */
    H5_FX_GATE_RELEASE,   /* seconds */
    H5_FX_GATE_RANGE,     /* 1 -> full mute when closed, 0 -> gate bypass */
    H5_FX_PARAM_COUNT
} h5_fx_param;

/* Return status for setters and getters. */
typedef enum {
    H5_OK = 0,
    H5_ERR_INVALID_CONTEXT,
    H5_ERR_INVALID_VOICE,
    H5_ERR_INVALID_INSTRUMENT,
    H5_ERR_INVALID_PARAM,
    H5_ERR_UNSUPPORTED,
    H5_ERR_RANGE
} h5_status;

/* Allocate and initialize a context. A sample_rate of 0 selects
 * H5_DEFAULT_SAMPLE_RATE. Returns NULL when the allocation fails.
 *
 * The caller owns the returned context and releases it with
 * h5_destroy_context(). */
h5_context h5_create_context(uint32_t sample_rate);

/* Release a context returned by h5_create_context(). A null context is
 * ignored. Every context must be released exactly once. */
void h5_destroy_context(h5_context ctx);

/* Change the sample rate. Internal state that depends on the rate is
 * re-initialized, so any sounding voice is cut. */
void h5_set_sample_rate(h5_context ctx, uint32_t sample_rate);

/* Reset the voices, parameters and the mix controls to their defaults, keeping
 * the sample rate. */
void h5_reset(h5_context ctx);

/* Render `frames` stereo interleaved frames into `stereo_out`.
 *
 * `stereo_out` must have room for `frames` stereo frames (2 * frames int16_t
 * values). The library is not thread-safe: h5_process() and the setters must
 * never run at the same time. h5_create_context() and h5_destroy_context()
 * must not run at the same time either.
 *
 * When no voice is sounding the output is silence. Callers that stream the
 * output continuously can poll h5_is_active() to suspend their audio path
 * while the kit is idle. */
void h5_process(h5_context ctx, int16_t *stereo_out, size_t frames);

/* Return true when at least one voice is producing sound or still in its
 * release phase, false when the kit is silent. A null context reports
 * false.
 *
 * The result changes only inside h5_process() or the trigger/setter calls,
 * so it can be used to decide whether h5_process() needs to be called at
 * all. */
bool h5_is_active(const h5_context ctx);

/* Set a sound parameter for a voice. Values are Q16.16 physical units:
 * Hz for pitch and filter frequencies, seconds for decay, signed semitones
 * for bend, and a 0..1 scalar for balance parameters.
 *
 * Out-of-range values are clamped to the documented range and H5_ERR_RANGE is
 * returned; the clamped value is stored. Unsupported (voice, param) pairs
 * return H5_ERR_UNSUPPORTED and leave state unchanged. */
h5_status h5_set_param(h5_context ctx,
    h5_voice voice,
    h5_param param,
    int32_t value);

/* Read back a voice parameter. Returns the stored Q16.16 value in *out.
 * A null *out is rejected with H5_ERR_INVALID_CONTEXT. */
h5_status h5_get_param(const h5_context ctx,
    h5_voice voice,
    h5_param param,
    int32_t *out);

/* Return true when the (voice, param) pair is part of the public control
 * surface for that voice. */
bool h5_param_supported(h5_voice voice, h5_param param);

/* Set the per-instrument mixer volume. Range is 0 .. 2 * H5_Q16_ONE;
 * H5_Q16_ONE (unity gain) is the middle of the range, so the lower half
 * reaches from silence to unity and the upper half is boost headroom.
 * Out-of-range values are clamped to that range, the clamped value is
 * stored, and H5_ERR_RANGE is returned. Equivalent to
 * h5_set_param(ctx, voice, H5_PARAM_VOLUME, volume_q16), where `voice` is
 * the instrument's parameter voice: the Hi-hat instruments both map to
 * H5_VOICE_HIHAT, the others map to the voice of the same name. Kept as a
 * function (not a macro) so the arguments stay type-checked. */
h5_status h5_set_instrument_volume(h5_context ctx,
    h5_instrument instrument,
    int32_t volume_q16);

/* Set the per-instrument mixer pan. Range is 0 .. H5_Q16_ONE;
 * 0 = left, H5_Q16_ONE / 2 = centre, H5_Q16_ONE = right. Out-of-range
 * values are clamped to that range, the clamped value is stored, and
 * H5_ERR_RANGE is returned. Equivalent to
 * h5_set_param(ctx, voice, H5_PARAM_PAN, pan_q16), with the same voice
 * mapping as h5_set_instrument_volume(). */
h5_status h5_set_instrument_pan(h5_context ctx,
    h5_instrument instrument,
    int32_t pan_q16);

/* Read back the per-instrument volume in Q16.16. A null *out is rejected
 * with H5_ERR_INVALID_CONTEXT. Equivalent to h5_get_param() on
 * H5_PARAM_VOLUME of the instrument's parameter voice. */
h5_status h5_get_instrument_volume(const h5_context ctx,
    h5_instrument instrument,
    int32_t *out);

/* Read back the per-instrument pan in Q16.16. A null *out is rejected
 * with H5_ERR_INVALID_CONTEXT. Equivalent to h5_get_param() on
 * H5_PARAM_PAN of the instrument's parameter voice. */
h5_status h5_get_instrument_pan(const h5_context ctx,
    h5_instrument instrument,
    int32_t *out);

/* Set a shared FX bus parameter. Out-of-range values are clamped to the
 * documented range, the clamped value is stored, and H5_ERR_RANGE is
 * returned. A null context is H5_ERR_INVALID_CONTEXT. */
h5_status h5_set_fx_param(h5_context ctx, h5_fx_param param, int32_t value);

/* Read back a shared FX bus parameter. A null context is
 * H5_ERR_INVALID_CONTEXT; a null *out is H5_ERR_INVALID_PARAM. */
h5_status h5_get_fx_param(const h5_context ctx, h5_fx_param param,
    int32_t *out);

/* Set the per-instrument FX send. Range is 0 .. H5_Q16_ONE; out-of-range
 * values are clamped, the clamped value is stored, and H5_ERR_RANGE is
 * returned. Equivalent to h5_set_param(ctx, voice, H5_PARAM_FX_SEND,
 * send_q16), with the same voice mapping as h5_set_instrument_volume(). */
h5_status h5_set_instrument_fx_send(h5_context ctx,
    h5_instrument instrument,
    int32_t send_q16);

/* Read back the per-instrument FX send in Q16.16. A null *out is rejected
 * with H5_ERR_INVALID_CONTEXT. Equivalent to h5_get_param() on
 * H5_PARAM_FX_SEND of the instrument's parameter voice. */
h5_status h5_get_instrument_fx_send(const h5_context ctx,
    h5_instrument instrument,
    int32_t *out);

/* Read back the per-instrument peak levels for the most recent
 * h5_process() block. `levels` must point to an array of at least
 * H5_INSTR_COUNT int32_t values. On success the array is filled with the
 * peak mono level for every instrument in Q16.16 (0 .. H5_Q16_ONE), in the
 * order of the h5_instrument enum.
 *
 * Each level is the peak of the instrument's own voice after its own volume
 * but before the stereo pan and before summing into the final mix. The
 * values are reset to zero at the start of every h5_process() call and
 * updated only while the corresponding instrument is sounding, so a host
 * can use the array to light per-instrument activity indicators.
 *
 * The two Hi-hat instruments share one voice, so they always report the
 * same level: a hit on either of them lights both entries, even when only
 * one of them was triggered.
 *
 * A null context or a null `levels` is rejected with H5_ERR_INVALID_CONTEXT. */
h5_status h5_get_instrument_levels(const h5_context ctx,
    int32_t *levels);

/* Trigger one of the eight instruments at full velocity. Each Tom instrument
 * triggers its own Tom voice, so Tom 1, Tom 2 and Tom 3 can sound
 * simultaneously. Equivalent to h5_trigger_velocity() with velocity 127. */
h5_status h5_trigger(h5_context ctx, h5_instrument instrument);

/* Trigger one of the eight instruments with a MIDI velocity.
 *
 * The velocity is the strike level of the hit, in the MIDI scale 0..127.
 * The engine turns it into a strike level of velocity/127 and applies it the
 * way the original does: the hit gets quieter, the pitch bend sweeps less
 * far, the noise gets duller and the transient gets softer.
 *
 * The per-voice Sensitivity parameter scales how much of that response the
 * velocity gets. At sensitivity 1 the velocity is used as it comes; at
 * sensitivity 0 the velocity is ignored and every hit sounds at full
 * strike level.
 *
 * A velocity above 127 is rejected with H5_ERR_RANGE and does not trigger
 * the instrument. */
h5_status h5_trigger_velocity(h5_context ctx, h5_instrument instrument,
    uint8_t velocity);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HIVE_V_H5_H */
