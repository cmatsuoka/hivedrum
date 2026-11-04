# Libhive

Libhive is a small, dependency-free C library that synthesizes a five-voice
percussion kit: Kick, Snare and three Toms. It renders a stereo mix into a
caller-supplied buffer and performs no I/O. All DSP is integer fixed-point,
the library contains no floating-point code.

## Features

- Five simultaneously playable voices built from three engine types (Kick,
  Snare, Toms 1/2/3).
- Per-instrument volume and pan.
- Per-voice sound parameters, exposed in physical units.
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

`make XTRA_PRECISION=1` defines `LIBHIVE_XTRA_PRECISION` and builds
`libhive-xtra.a` / `h5_test-xtra` under `build-xtra/`. The macro keeps one
DSP stage whose effect is below masking in the mix: the Snare's second
drive harmonic (about 40 dB below the fundamental; the two builds' renders
differ by at most ~66 LSB, about -46 dB RMS including the mix). The
default build omits it to keep the per-sample path a little cheaper; leave
the macro undefined for normal use. `make check` builds and runs the test
harness against both variants.

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

Each instrument has a mixer volume and pan. Pan is 0 (left), 65536 (right) or
in between; at centre the voice contributes half its amplitude to each
channel. All instruments are summed into a 64-bit mix bus and converted to
16-bit output once per frame with rounding and saturation; there is no
separate master limiter.

The library is not thread-safe: rendering, setters and context creation or
destruction must not run concurrently.

## Parameters

Values are Q16.16 physical units: Hz for pitch and cutoff, seconds for decay,
signed semitones for bend, and a 0..1 scalar for balance controls.

| Parameter | Applies to |
| --- | --- |
| `H5_PARAM_NOISE_PITCH` | All voices |
| `H5_PARAM_TONE_PITCH` | All voices |
| `H5_PARAM_BEND` | All voices |
| `H5_PARAM_DECAY` | All voices |
| `H5_PARAM_NOISE_TONE` | All voices |
| `H5_PARAM_CLICK_TONE` | All voices |
| `H5_PARAM_VOLUME` | All voices (mixer level) |
| `H5_PARAM_PAN` | All voices (mixer pan) |
| `H5_PARAM_SENSITIVITY` | All voices (strike sensitivity) |

Use `h5_param_supported()` to test a specific voice/parameter pair rather than
hard-coding this matrix. Volume and Pan are ordinary parameters: they can be
set and read with `h5_set_param()` / `h5_get_param()`, and `h5_reset()`
restores their defaults from the same table.

## Velocity

`h5_trigger_velocity()` starts a hit with a MIDI velocity (0..127).
`h5_trigger()` is the same as `h5_trigger_velocity()` with velocity 127.

`H5_PARAM_SENSITIVITY` (0..1, default 1) scales how much of that response the
velocity gets: at 1 the velocity is used as it comes, at 0 the velocity is
ignored and every hit sounds at full strike level.

## Tests

`make check` runs the harness twice: once against the default `libhive.a`
and once against the `LIBHIVE_XTRA_PRECISION` build (`build-xtra/`). The
stages cover initialization, sine and DSP primitives, silence, the public
API, per-engine sound checks for Kick, Tom and Snare, and the mixer and
output stage. It also renders `build/silence.wav`, `build/kick.wav`,
`build/tom.wav`, `build/snare.wav` and `build/mix.wav` for listening.
