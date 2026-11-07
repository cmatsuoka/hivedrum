<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="hive-logo-dark.svg">
    <img src="hive-logo.svg" alt="Libhive logo" width="280">
  </picture>
</p>

# Libhive

*Libhive is under development and the API is currently unstable.*

Libhive is a small, dependency-free C library that synthesizes a
percussion kit inspired by analog electronic drums from the 80s. It
renders a stereo mix into a caller-supplied buffer and performs no I/O.
All DSP is integer fixed-point, with no memory allocation performed
after the library initialization.

## Features

- Seven simultaneously playable voices built from five engine types (Kick,
  Snare, Tom, Cymbal and the metallic engine for the Hi-hat). The Hi-hat is
  one voice reachable through two instruments, closed and open.
- The Cymbal pairs a short measured strike table with a swept noise wash;
  the Hi-hat is built from a ten-oscillator triangle bank, a swept resonant
  low-pass and a saturation stage.
- Per-instrument volume and pan.
- Per-voice sound parameters, exposed in physical units.
- Integrated reverb and noise gate effects with per instrument send levels.
- Deterministic integer DSP: 28-bit phase sine, state-variable filters,
  shaped envelopes and a table-driven soft clipper.
- Plain C99 with a single public header. No external dependencies.

## Building

```sh
make          # builds libhive.a
make check    # builds and runs the test harness (writes WAV files to build/)
```

The library is built with `-std=c99 -Wall -Wextra -Werror`, and the build
enables floating-point diagnostics when the compiler supports them to keep
the library integer-only. Unwind tables are disabled where the compiler
supports the flags, and the library is built without the stack-protector
canary.

The default is size (`-Os`). The translation units that run once per sample
(the engines, the filter, the envelope, the noise source, the mixer and the
tables) are compiled at `-O2`, because their speed matters more than their
size; everything else stays at `-Os`.

## Build options

The Cymbal and the Hi-hat are optional and gated by preprocessor macros.
Undefine `LIBHIVE_ENGINE_HIHAT` or `LIBHIVE_ENGINE_CYMBAL` to remove each
engine. A disabled voice stays in the public API but renders silence and
accepts no parameters. The same definitions are passed to the test harness,
which skips the stages of the engines that are not compiled in.

## Usage

```c
#include "h5.h"

#include <stdint.h>

int main(void)
{
    h5_context ctx = h5_create_context(44100u);
    if (ctx == 0) {
        return 1;
    }

    /* Pitch in Hz, decay in seconds, balances in 0..1, all Q16.16. */
    h5_set_param(ctx, H5_VOICE_SNARE, H5_PARAM_TONE_PITCH, 180 * H5_Q16_ONE);
    h5_set_param(ctx, H5_VOICE_SNARE, H5_PARAM_DECAY, H5_Q16_ONE / 4);
    h5_set_instrument_pan(ctx, H5_INSTR_SNARE, H5_Q16_ONE / 2);

    h5_trigger(ctx, H5_INSTR_SNARE);

    int16_t buffer[2 * 512];
    h5_process(ctx, buffer, 512); /* stereo interleaved */

    h5_destroy_context(ctx);
    return 0;
}
```

## API

The public interface is in `include/h5.h`. A context is an opaque handle
created with `h5_create_context()` and released with `h5_destroy_context()`.

Key entry points:

| Function | Purpose |
| --- | --- |
| `h5_create_context(sample_rate)` | Allocate a context; `0` selects 44100 Hz. |
| `h5_destroy_context(ctx)` | Release a context. |
| `h5_version_number()` | Return the linked library version, packed as `H5_VERSION_NUMBER`. |
| `h5_set_sample_rate(ctx, rate)` | Change the sample rate; sounding voices are cut. |
| `h5_reset(ctx)` | Restore parameters and mixer defaults, keeping the rate. |
| `h5_set_param(ctx, voice, param, value)` | Set a voice parameter (Q16.16 physical units). |
| `h5_get_param(ctx, voice, param, &out)` | Read a voice parameter. |
| `h5_param_supported(voice, param)` | Query the control surface. |
| `h5_set_instrument_volume` / `h5_get_instrument_volume` | Per-instrument mixer level (alias of `h5_set_param` / `h5_get_param` on `H5_PARAM_VOLUME`). |
| `h5_set_instrument_pan` / `h5_get_instrument_pan` | Per-instrument pan (alias on `H5_PARAM_PAN`). |
| `h5_trigger(ctx, instrument)` | Start one instrument at full velocity. |
| `h5_trigger_velocity(ctx, instrument, velocity)` | Start one instrument with a MIDI velocity (0..127). |
| `h5_process(ctx, out, frames)` | Render stereo interleaved `int16_t` frames. |
| `h5_is_active(ctx)` | True while any voice is still sounding. |

Setters clamp out-of-range values to the documented range, store the clamped
value and return `H5_ERR_RANGE`. Unsupported `(voice, param)` pairs return
`H5_ERR_UNSUPPORTED` and leave state unchanged.

Each instrument has a mixer volume and pan. Volume spans 0 to 131072, with
65536 (unity gain, the old maximum) at the midpoint, so the upper half of the
control is boost headroom. Pan is 0 (left), 65536 (right) or in between; at
centre the voice contributes half its amplitude to each channel. All
instruments are summed into a 64-bit mix bus and converted to
16-bit output once per frame with rounding and saturation; there is no
separate master limiter.

The library is not thread-safe: rendering, setters and context creation or
destruction must not run concurrently.

## Voices and trigger targets

The kit has seven voices and eight trigger targets. The Tom engine is
instantiated three times; the Hi-hat is one voice reachable through two
instruments:

| Instrument | Voice | Character |
| --- | --- | --- |
| `H5_INSTR_KICK` | Kick | |
| `H5_INSTR_SNARE` | Snare | |
| `H5_INSTR_TOM_1` / `H5_INSTR_TOM_2` / `H5_INSTR_TOM_3` | Tom 1 / Tom 2 / Tom 3 | |
| `H5_INSTR_CYMBAL` | Cymbal | |
| `H5_INSTR_HIHAT_CLOSED` | Hi-hat | release as set |
| `H5_INSTR_HIHAT_OPEN` | Hi-hat | a 0.7 s longer release |

Triggering the closed instrument while an open hat is sounding restarts the
shared voice, which chokes it. Both Hi-hat instruments share the Hi-hat
voice, its parameters and its mixer values.

## Parameters

Values are Q16.16 physical units: Hz for pitch and cutoff, seconds for decay,
signed semitones for bend, and a 0..1 scalar for balance controls.

| Parameter | Applies to |
| --- | --- |
| `H5_PARAM_NOISE_PITCH` | Kick, Snare, Toms, Cymbal, Hi-hat (noise brightness) |
| `H5_PARAM_TONE_PITCH` | All voices |
| `H5_PARAM_BEND` | Kick, Snare, Toms, Cymbal |
| `H5_PARAM_DECAY` | All voices |
| `H5_PARAM_NOISE_TONE` | All voices |
| `H5_PARAM_CLICK_TONE` | Kick, Snare, Toms |
| `H5_PARAM_DRIVE` | Kick, Snare, Toms (sine/triangle blend) |
| `H5_PARAM_SWEEP` | Cymbal |
| `H5_PARAM_BELL_CYMBAL` | Cymbal, Hi-hat |
| `H5_PARAM_NOISE_BODY` | Cymbal, Hi-hat (noise high-pass corner) |
| `H5_PARAM_VOLUME` | All voices (mixer level) |
| `H5_PARAM_PAN` | All voices (mixer pan) |
| `H5_PARAM_SENSITIVITY` | All voices (strike sensitivity) |

Use `h5_param_supported()` to test a specific voice/parameter pair rather than
hard-coding this matrix. Volume and Pan are ordinary parameters: they can be
set and read with `h5_set_param()` / `h5_get_param()`, and `h5_reset()`
restores their defaults from the same table. A metallic voice whose engine is
not compiled in supports no parameter at all.

## Velocity

`h5_trigger_velocity()` starts a hit with a MIDI velocity (0..127).
`h5_trigger()` is the same as `h5_trigger_velocity()` with velocity 127.

`H5_PARAM_SENSITIVITY` (0..1, default 1) scales how much of that response the
velocity gets: at 1 the velocity is used as it comes, at 0 the velocity is
ignored and every hit sounds at full strike level.

On the Hi-hat (closed or open), a soft strike also leans the balance toward
noise: the Bell/Cymbal value drops slightly and the Noise/Tone value drops more.

## Tests

`make check` runs the harness against `libhive.a`. The stages cover
initialization, sine and DSP primitives, silence, the public API,
per-engine sound checks for Kick, Tom, Snare, Hi-hat and Cymbal, and the
mixer and output stage. It also renders `build/silence.wav`,
`build/kick.wav`, `build/tom.wav`, `build/snare.wav`, `build/hihat.wav`,
`build/cymbal.wav` and `build/mix.wav` for listening. The Hi-hat and Cymbal
stages are skipped when their engine is not compiled in.
