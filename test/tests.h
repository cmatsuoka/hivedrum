/*
 * tests.h - per-voice render checks for the Hivedrum test harness.
 */

#ifndef TESTS_H
#define TESTS_H

/* The metallic engines are optional: the Hi-hat is compiled in behind
 * HIVEDRUM_H5_ENGINE_HIHAT and the Cymbal behind HIVEDRUM_H5_ENGINE_CYMBAL. The
 * Makefile passes the same definitions to the library and to the harness,
 * so a trimmed build skips the stages of the engines it does not contain
 * instead of failing them. */
#if defined(HIVEDRUM_H5_ENGINE_HIHAT)
#define H5_TEST_HIHAT 1
#else
#define H5_TEST_HIHAT 0
#endif

#if defined(HIVEDRUM_H5_ENGINE_CYMBAL)
#define H5_TEST_CYMBAL 1
#else
#define H5_TEST_CYMBAL 0
#endif

/* The FX bus is optional too: the reverb is compiled in behind
 * HIVEDRUM_FX_REVERB and the noise gate behind HIVEDRUM_FX_NOISE_GATE. The
 * Makefile passes the same definitions to the library and to the harness,
 * so a trimmed build skips the FX stages it cannot run. */
#if defined(HIVEDRUM_FX_REVERB)
#define H5_TEST_FX_REVERB 1
#else
#define H5_TEST_FX_REVERB 0
#endif

#if defined(HIVEDRUM_FX_NOISE_GATE)
#define H5_TEST_FX_GATE 1
#else
#define H5_TEST_FX_GATE 0
#endif

int command_kick(void);
int command_tom(void);
int command_snare(void);
int command_hihat(void);
int command_cymbal(void);
int command_mixer(void);
int command_velocity(void);
int command_fx(void);

#endif /* TESTS_H */
