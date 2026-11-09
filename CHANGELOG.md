# Changelog

## 0.9.0 (2026-11-09)

Project renamed from libhive to Hivedrum for a more distinctive identity.

- Headers, macros, build files, examples and docs updated for the new name
- Logo files consolidated (dark/light)
- No functional audio changes

## 0.8.0 (2026-11-07)

A large sound-design and packaging release.

- New drive parameter for bass, snare and tom voices
- Cymbal engine substantially redesigned: bell oscillator, sampled body,
  synthesized attack, new metallic approach, and reworked loop/tone
- Increased snare attack transient
- Cymbal and hi-hat gain adjustments
- Reverb and noise gate made optional at build time
- Dead code removed; extra-precision paths gated behind a compile-time macro
- Sound-engine documentation and diagrams added

## 0.7.0 (2026-11-05)

Made the hi-hat's noise character adjustable.

- Exposed configurable hi-hat noise pitch and body parameters
- Parameter validation, getters/setters, and tests updated accordingly
- Documentation updated for the new parameters

## 0.6.0 (2026-11-05)

Normalized voice gains and improved hi-hat response.

- Engine gains moved to a consistent 0–2x range with 1x defaults
- Improved hi-hat velocity dynamics
- Test and documentation updates for the new gain convention

## 0.5.0 (2026-11-05)

Added built-in effects processing to the mix.

- New reverb effect
- New noise gate effect
- Effects wired into the mixer and exposed through the API
- Effects-processing fixes and improved metallic filtering for the hi-hat
- Bend made more sensitive to velocity
- Dedicated effects test suite added

## 0.4.0 (2026-11-04)

Reworked the metallic engine's noise filtering and general cleanup.

- Large rewrite of the metallic engine and its filtering
- New/updated parameters and matching test coverage
- Minor refactoring and optimization
- Logo added to the README

## 0.3.0 (2026-11-04)

Introduced a dedicated metallic synthesis engine for the hi-hat and cymbal
voices.

- New metallic engine replacing the simpler noise-based approach for cymbals
  and hi-hat
- Engine exposed through the existing parameter API
- Updated examples and build to include the new source

## 0.2.0 (2026-11-03)

Added velocity-sensitive performance so a trigger can vary the sound instead of
producing a fixed hit.

- New velocity-based trigger entry point
- Velocity drives generator parameters such as level, bend and timbre
- Some velocity-dependent calculations gated behind a compile-time macro
- Response curve tuned after initial implementation

## 0.1.0 (2026-11-02)

First released version of the seven-voice percussion synthesizer, arranged as a
standard drum kit (kick, snare, tom, cymbal, closed and open hi-hat).

- Fixed-point DSP core with precomputed pitch and filter-coefficient tables
- Kick, snare and tom sound generators; one shared tom generator for tom 1-3
- Stereo voice mixer with per-voice volume and pan parameters
- Public API for context lifecycle, processing, and parameter get/set
- Size and 32-bit optimizations, silent-voice skipping, and a usage example
- License, README, and test harness added
