/*
 * h5_voice.c - per-voice lifecycle and dispatch.
 *
 * Each public voice maps to one of the three engine types. The Tom engine is
 * instantiated three times, so the Tom voices keep independent state.
 *
 * The voice-to-engine map lives in one table: every voice names the engine
 * operation set it runs and where its engine state sits inside the context.
 * The dispatch functions are then a table lookup and an indirect call, and
 * the engine's activity flag is at a fixed offset in every engine state, so
 * the render loop learns that a voice has finished without a second
 * dispatch.
 */

#include "h5_internal.h"

/* The voice-to-engine map. The offsets are fixed by the context layout, so
 * the table is shared by every context and a context carries no per-voice
 * pointers. */
const struct h5_voice_slot h5_voice_slots[H5_VOICE_COUNT] = {
    { &h5_kick_ops, offsetof(struct h5_context_s, kick) },
    { &h5_snare_ops, offsetof(struct h5_context_s, snare) },
    { &h5_tom_ops, offsetof(struct h5_context_s, tom[0]) },
    { &h5_tom_ops, offsetof(struct h5_context_s, tom[1]) },
    { &h5_tom_ops, offsetof(struct h5_context_s, tom[2]) }
};

void h5_voice_setup_all(struct h5_context_s *ctx)
{
    h5_voice voice;

    /* Setting every voice up resets it, so no voice is sounding any more. */
    ctx->active_mask = 0u;
    for (voice = 0; voice < H5_VOICE_COUNT; voice++) {
        ctx->voice_level[voice] = 0;
        h5_voice_slots[voice].ops->setup(h5_voice_engine(ctx, voice), voice,
            ctx->voice_params[voice], ctx->sample_rate);
    }
}

void h5_voice_configure(struct h5_context_s *ctx, h5_voice voice)
{
    h5_voice_slots[voice].ops->configure(h5_voice_engine(ctx, voice),
        ctx->voice_params[voice], ctx->sample_rate);
}

int32_t h5_voice_strike_level(const struct h5_context_s *ctx, h5_voice voice,
    uint32_t velocity)
{
    int32_t sensitivity = ctx->voice_params[voice][H5_PARAM_SENSITIVITY];
    int32_t level;

    if (velocity > 127u) {
        velocity = 127u;
    }

    /* The strike level of the hit is velocity/127. */
    level = (int32_t)(((int64_t)velocity * H5_Q16_ONE + 63) / 127);

    /* The Sensitivity scales how much of the velocity the hit responds to.
     * At 0 the velocity is ignored and the strike level is full; at 1 the
     * strike level is the velocity itself. */
    return (int32_t)((int64_t)(H5_Q16_ONE - sensitivity)
        + h5_scale_q16(sensitivity, level));
}

void h5_voice_trigger(struct h5_context_s *ctx, h5_voice voice)
{
    if ((unsigned)voice >= (unsigned)H5_VOICE_COUNT) {
        return;
    }
    h5_voice_slots[voice].ops->trigger(h5_voice_engine(ctx, voice),
        h5_voice_strike_level(ctx, voice, 127u), ctx->sample_rate);
    ctx->active_mask |= (uint32_t)1u << (unsigned)voice;
}

void h5_voice_trigger_velocity(struct h5_context_s *ctx, h5_voice voice,
    uint32_t velocity)
{
    if ((unsigned)voice >= (unsigned)H5_VOICE_COUNT) {
        return;
    }
    h5_voice_slots[voice].ops->trigger(h5_voice_engine(ctx, voice),
        h5_voice_strike_level(ctx, voice, velocity), ctx->sample_rate);
    ctx->active_mask |= (uint32_t)1u << (unsigned)voice;
}

void h5_voice_trigger_instrument(struct h5_context_s *ctx,
    h5_instrument instrument)
{
    /* The instrument and voice enums share their order (checked at compile
     * time), so the mapping is a cast. */
    h5_voice_trigger(ctx, (h5_voice)instrument);
}

bool h5_is_active(const h5_context handle)
{
    const struct h5_context_s *ctx = (const struct h5_context_s *)handle;

    return ctx != 0 && ctx->active_mask != 0u;
}
