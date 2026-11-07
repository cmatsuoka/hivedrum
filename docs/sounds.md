# Sound Generation

The kit is built from five engine types: Kick, Snare, Tom (three tunings of
one engine), Cymbal, and the metallic engine used by the Hi-hat. Each
voice is a small analog-style synthesizer chain: oscillators and noise feed
VCAs driven by envelope generators, the result passes filters, and the final
mix runs through a soft-clip output stage.

## Shared building blocks

- **Tone oscillator.** A sine oscillator and a triangle shaper run from the
  same phase. A Drive control crossfades between them, like a variable
  waveform shaper: sine at one end, the odd-harmonic triangle at the other.
- **Noise source.** A broadband pseudo-random source with a sample-and-hold
  stage, used as the analog noise generator for the drum and metallic
  bodies.
- **Envelopes.** Linear attack/release generators. Their outputs are raised
  to higher powers (cubed, fourth, fifth, tenth) to approximate the sharp
  attack and fast-then-long decay of a struck membrane or metal plate.
- **Filters.** A second-order state-variable filter section configurable as
  low-pass, band-pass or high-pass, with a resonance control. Cutoff can be
  swept by an envelope, the way a filter-envelope contour sweeps an analog
  filter.
- **Drum output stage.** The Kick, Snare and Tom use a `tanh` soft clipper
  with a fixed 1.10 gain, followed by a fixed high-pass to remove sub-bass
  and DC. The metallic voices use a separate dry/saturated blend instead.

Diagram legend:

```
[ block ]   circuit block
( text )    parameter / control
│ ▼         audio signal
◀┄┄ ┄┄▶     control input
BC = Bell/Cymbal, NT = Noise/Tone (metallic diagrams)
```

## Kick

- **Tone branch.** Sine/triangle oscillator through the Drive blend, then a
  VCA driven by the cubed tone envelope. The same cubed envelope bends the
  oscillator frequency upward at the start of the hit.
- **Noise branch.** Broadband noise is band-limited (50 Hz high-pass,
  20 kHz low-pass), then passes a resonant low-pass whose cutoff is swept
  downward by the noise envelope.
- **Transient branch.** The same filtered noise through a very fast
  envelope (tenth power) adds the beater click.
- **Output.** Tone, noise and transient are summed, scaled by the strike
  level, soft-clipped, given 1.10 fixed gain and passed through a 15 Hz
  high-pass.

The single Decay control sets the tone, noise and click envelopes; Noise/Tone
balances the body, Click sets the beater transient, Bend sets the pitch drop
and Noise Pitch sets the noise filter cutoff.

```
 Tone Pitch ─┐                                          Noise
             │                                            │
 Bend ───────┴─▶( pitch bend )                            │
                       │                                  ▼
                       ▼                            [ 50 Hz HP ]
              [ sine / triangle ]◀┄┄┄ Drive               │
                       │                                  ▼
                       ▼                            [ 20 kHz LP ]
                 [ tone VCA ]◀┄┄┄ env³, Noise Tone, Click │
                       │                                  ▼
                       │                            [ swept LP ]◀┄┄┄ Noise Pitch, sweep
                       │                                  ├───────────────────────────┐
                       │                                  ▼                           ▼
                       │      Noise Tone, Click ┄┄┄▶[ noise VCA ]              [ burst env¹⁰ ]
                       │                                  │                           │
                       │                                  │                           ▼
                       │                                  │           Click ┄┄┄▶[ click VCA ]
                       │                                  │                           │
                       └──────────────────────────────────┼───────────────────────────┘
                                                          │
                                                          ▼
                                                        [ Σ ]
                                                          │
                                                          ▼
                                                      [ tanh ]
                                                          │
                                                          ▼
                                                      [ ×1.10 ]
                                                          │
                                                          ▼
                                                    [ HP 15 Hz ]
                                                          │
                                                          ▼
                                                         OUT
```

## Snare

- **Shell tone branch.** Sine/triangle drive core with a subtracted third
  harmonic (and, in the high-precision build, an added second harmonic) to
  give the shell its driven character. It passes a VCA driven by the cubed
  body envelope and keeps full bandwidth.
- **Rattle branch.** Broadband noise is band-limited, then split into four
  parallel band-pass resonators (roughly 755 Hz, 1.07 kHz, 5.5 kHz and
  12 kHz) summed with a broadband wire-noise term. The rattle then passes a
  resonant low-pass whose cutoff is swept by the envelope.
- **Transient branch.** The filtered rattle through a tenth-power envelope
  adds the wire snap.
- **Output.** Shell tone and rattle are balanced by the Noise/Tone control,
  the transient is added, the mix is soft-clipped, given 1.10 fixed gain and
  passed through a 50 Hz high-pass.

The sweep sits on the rattle path only, so the shell tone and the click keep
their bandwidth. Noise Pitch sets the rattle low-pass, Click sets the snap,
and the filter contour follows the rattle envelope.

```
 Tone Pitch ─┐                       Noise
             │                         │
 Bend ───────┴─▶( pitch bend )         │
                       │               ▼
                       ▼     [ 50/20k band limit ]
    Drive ┄┄┄▶[ sine / triangle ]      ├─▶───────[ 750 Hz BP ]─────────┐
                       │               │                               │
                       │               ├─▶──────[ 1.0 kHz BP ]─────────┤
                       │               │                               │
                       │               ├─▶──────[ 5.5 kHz BP ]─────────┤
                       │               │                               │
                       │               ├─▶───────[ 12 kHz BP ]─────────┤
                       │               │                               │
                       │               └─▶───────[ broadband ]─────────┤
                       ▼                                               │
                [ drive − H3 ]                                         ▼
                       │                                             [ Σ ]
                       ▼                                               │
                 [ shell VCA ]◀┄┄┄ env³, Noise Tone, Click             ▼
                       │                   Noise Pitch, sweep ┄┄┄▶[ swept LP ]
                       │                                               ├───────────────┐
                       │                                               │               │
                       │                                               ▼               ▼
                       │                   Noise Tone, Click ┄┄┄▶[ rattle VCA ]  [ burst env¹⁰ ]
                       │                                               │               │
                       │                                               │               ▼
                       │                                               │ Click ┄┄┄▶[ click VCA ]
                       │                                               │               │
                       └───────────────────────────────────────────────┼───────────────┘
                                                                       │
                                                                       ▼
                                                                     [ Σ ]
                                                                       │
                                                                       ▼
                                                                    [ tanh ]
                                                                       │
                                                                       ▼
                                                                    [ ×1.10 ]
                                                                       │
                                                                       ▼
                                                                  [ HP 50 Hz ]
                                                                       │
                                                                       ▼
                                                                      OUT
```

## Toms 1-3

- **Tone branch.** Sine/triangle oscillator through the Drive blend, its
  frequency bent by the cubed body envelope, through a VCA driven by the same
  cubed envelope.
- **Noise branch.** Band-limited broadband noise through a resonant
  low-pass swept by the envelope, then a VCA.
- **Transient branch.** The filtered noise through a tenth-power envelope
  adds the stick transient.
- **Output.** Tone and noise body are mixed, the transient is added, the
  strike level scales the hit, and the result is soft-clipped, given 1.10
  fixed gain and passed through a 25 Hz high-pass.

The three Tom instances share the engine but differ in tuning and in their
filter-bend depth, so their noise sweeps close at slightly different rates.

```
 Tone Pitch ─┐                                         Noise
             │                                            │
 Bend ───────┴─▶( pitch bend )                            │
                       │                                  ▼
                       ▼                            [ 50 Hz HP ]
              [ sine / triangle ]◀┄┄┄ Drive               │
                       │                                  ▼
                       ▼                            [ 20 kHz LP ]
                 [ tone VCA ]◀┄┄┄ env³, Noise Tone, Click │
                       │                                  ▼
                       │                            [ swept LP ]◀┄┄┄ Noise Pitch, sweep
                       │                                  ├───────────────────────────┐
                       │                                  ▼                           ▼
                       │      Noise Tone, Click ┄┄┄▶[ noise VCA ]              [ burst env¹⁰ ]
                       │                                  │                           │
                       │                                  │                           ▼
                       │                                  │           Click ┄┄┄▶[ click VCA ]
                       │                                  │                           │
                       └──────────────────────────────────┼───────────────────────────┘
                                                          │
                                                          ▼
                                                        [ Σ ]
                                                          │
                                                          ▼
                                                      [ tanh ]
                                                          │
                                                          ▼
                                                      [ ×1.10 ]
                                                          │
                                                          ▼
                                                    [ HP 25 Hz ]
                                                          │
                                                          ▼
                                                         OUT
```

## Cymbal

- **Strike and loop.** A synthesized strike: 22 damped inharmonic modes
  plus a short high-passed noise burst, played through variable-rate phase
  accumulators. A cleaned 50 ms sustain loop, taken from the sustain where
  the loop tone is clearest and still above the recording's noise floor, is
  played forward/backward under the same envelope. Tone Pitch sets the
  playback rates; Bend sweeps them over the wash envelope.
- **Wash.** Noise/Tone crossfades the broadband noise wash against the
  sampled loop: 0 leaves only noise, 1 leaves only the loop. The noise
  passes a two-pole high-pass (Noise Body), the loop joins it, and the sum
  passes a swept low-pass (Noise and Sweep), then the wash envelope. The
  wash uses an exponential release, so its level
  falls by a nearly constant number of decibels per second through the tail.
- **Mix.** Bell/Cymbal trades the synthesized strike against the wash.
- **Output.** The mix passes a fixed high-pass, a +9.5 dB makeup gain and a
  soft clipper.

```
       Strike path                            Wash path
       Strike modes                           Loop table
            │                                     │
            ▼                                     ▼
    Variable-rate read                      Ping-pong read
            │                                     │
            ▼                                     ▼
        Strike VCA ◀┄┄ Bell/Cymbal          Noise/Tone mix ◀┄┄ Noise
            │                                     │
            │                                     ▼
            │                                 2-pole HP ◀┄┄ Noise Body
            │                                     │
            │                                     ▼
            │                                  Swept LP ◀┄┄ Noise, Sweep
            │                                     │
            │                                     ▼
            │                  Bell/Cymbal ┄┄▶ Wash VCA
            │                                     │
            └──────────────────┬──────────────────┘
                               │
                               ▼
                             [ Σ ]
                               │
                               ▼
                            HP 90 Hz
                               │
                               ▼
                            +9.5 dB
                               │
                               ▼
                           Soft clip
                               │
                               ▼
                              OUT
```

The strike modes carry the common transient of the instrument; the loop
carries the metallic body and the noise wash carries the sizzle, with the
per-register decay differences shaped by the Sweep contour.

## Hi-hat

- **Metallic bank.** A ten-triangle bank, with Tone Pitch scaling the whole
  bank. The bank passes a VCA driven by the fifth power of the metal
  envelope.
- **Noise branch.** Broadband noise through the two-pole high-pass (Noise
  Body) and the two cascaded low-pass sections (Noise), with the noise
  envelope raised to the fourth power. The Hi-hat keeps the sibilant band
  above its high-pass corner, which gives it its thin character.
- **Output.** Metal and noise are balanced by Bell/Cymbal and Noise/Tone,
  scaled by the strike level, and blended through the dry/saturated Drive
  stage. A fixed +2 dB makeup gain follows; an open hit adds another +2 dB.

Open and closed hits share the engine. The open instrument lengthens the
noise release by 0.75 s; the metal envelope is the same for open and closed
hits.

```
 Tone Pitch ───▶( bank scale )                                Noise
                       │                                        │
                       ▼                                        ▼
           [ 10 triangle osc bank ]                        [ 2-pole HP ] ◀┄┄┄ Noise Body
                       │                                        │
                       ▼                                        ▼
                     [ Σ ]                                  [ LP 1 ] ◀┄┄┄ Noise
                       │                                        │
                       ▼                                        ▼
                 [ metal VCA ]◀┄┄┄ env_m⁵, Bell/Cymbal      [ LP 2 ] ◀┄┄┄ same cutoff
                       │                                        │
                       │                                        ▼
                       │                                 [ noise VCA ]◀┄┄┄ env_n⁴, NT, 1−BC
                       │                                        │
                       │                                        │
                       │                                        │
                       │                                        │
                       │                                        │
                       │                                        │
                       │                                        │
                       │                                        │
                       │                                        │
                       │                                        │
                       │                                        │
                       │                                        │
                       │                                        │
                       │                                        │
                       │                                        │
                       └────────────────────┬───────────────────┘
                                            │
                                            ▼
                                          [ Σ ]
                                            │
                                            ├──▶[ dry 1−Drive ]─┐
                                            ▼                   │
                                        [ tanh ]                │
                                            │                   │
                                            ▼                   │
                                         [ HP ]                 │
                                            │                   │
                                            ▼                   │
                                       [ ×Drive ]───────────────┤
                                                                │
                                                                ▼
                                                               OUT
```
