/*
 * h5_tables.c - flash-resident tables for the Hivedrum library.
 *
 * The sine table stores only the first quadrant, as a file-scope `const`
 * array in flash. The full period is rebuilt once, on the first
 * h5_create_context(), into a table shared by every context and every voice;
 * no context carries a copy.
 *
 * The stored values are exact: every entry equals
 *
 *     floor(32767 * sin(2*pi*i/1024) + 0.5)      for i = 0..256
 *
 * (round-half-up, the same convention the test harness uses). The peak at
 * index 256 is exactly 32767. Mirroring and negation reproduce the remaining
 * quadrants. The oscillator and the filter coefficient setup interpolate
 * between entries, so the coarser table does not quantise the output.
 *
 * Regenerate the table with: python3 tools/gen_sine_table.py
 */

#include <stdint.h>
#include <stdlib.h>

#include "h5_internal.h"

const int16_t h5_sine_quad1[H5_SINE_QUAD_SIZE] = {
         0,    201,    402,    603,    804,   1005,   1206,   1407,
      1608,   1809,   2009,   2210,   2410,   2611,   2811,   3012,
      3212,   3412,   3612,   3811,   4011,   4210,   4410,   4609,
      4808,   5007,   5205,   5404,   5602,   5800,   5998,   6195,
      6393,   6590,   6786,   6983,   7179,   7375,   7571,   7767,
      7962,   8157,   8351,   8545,   8739,   8933,   9126,   9319,
      9512,   9704,   9896,  10087,  10278,  10469,  10659,  10849,
     11039,  11228,  11417,  11605,  11793,  11980,  12167,  12353,
     12539,  12725,  12910,  13094,  13279,  13462,  13645,  13828,
     14010,  14191,  14372,  14553,  14732,  14912,  15090,  15269,
     15446,  15623,  15800,  15976,  16151,  16325,  16499,  16673,
     16846,  17018,  17189,  17360,  17530,  17700,  17869,  18037,
     18204,  18371,  18537,  18703,  18868,  19032,  19195,  19357,
     19519,  19680,  19841,  20000,  20159,  20317,  20475,  20631,
     20787,  20942,  21096,  21250,  21403,  21554,  21705,  21856,
     22005,  22154,  22301,  22448,  22594,  22739,  22884,  23027,
     23170,  23311,  23452,  23592,  23731,  23870,  24007,  24143,
     24279,  24413,  24547,  24680,  24811,  24942,  25072,  25201,
     25329,  25456,  25582,  25708,  25832,  25955,  26077,  26198,
     26319,  26438,  26556,  26674,  26790,  26905,  27019,  27133,
     27245,  27356,  27466,  27575,  27683,  27790,  27896,  28001,
     28105,  28208,  28310,  28411,  28510,  28609,  28706,  28803,
     28898,  28992,  29085,  29177,  29268,  29358,  29447,  29534,
     29621,  29706,  29791,  29874,  29956,  30037,  30117,  30195,
     30273,  30349,  30424,  30498,  30571,  30643,  30714,  30783,
     30852,  30919,  30985,  31050,  31113,  31176,  31237,  31297,
     31356,  31414,  31470,  31526,  31580,  31633,  31685,  31736,
     31785,  31833,  31880,  31926,  31971,  32014,  32057,  32098,
     32137,  32176,  32213,  32250,  32285,  32318,  32351,  32382,
     32412,  32441,  32469,  32495,  32521,  32545,  32567,  32589,
     32609,  32628,  32646,  32663,  32678,  32692,  32705,  32717,
     32728,  32737,  32745,  32752,  32757,  32761,  32765,  32766,
     32767
};

int16_t *h5_sine = 0;

static void rebuild_sine(int16_t *table)
{
    unsigned int i;

    for (i = 0u; i <= 256u; i++) {
        table[i] = h5_sine_quad1[i];
    }
    for (i = 257u; i <= 511u; i++) {
        table[i] = h5_sine_quad1[512u - i];
    }
    for (i = 512u; i <= 767u; i++) {
        table[i] = (int16_t)-h5_sine_quad1[i - 512u];
    }
    for (i = 768u; i < H5_SINE_TABLE_SIZE; i++) {
        table[i] = (int16_t)-h5_sine_quad1[1024u - i];
    }
}

void h5_sine_init(void)
{
    static int initialized = 0;
    int16_t *table;

    if (initialized) {
        return;
    }
    initialized = 1;

    table = (int16_t *)malloc((size_t)H5_SINE_TABLE_SIZE * sizeof(int16_t));
    if (table == 0) {
        /* The quadrant in flash is bit-exact with the full table, so the
         * lookups fall back to it and the library still works. */
        return;
    }

    /* The table is filled before it is published: a second thread that
     * creates a context while this one is building must not find a
     * half-written table. */
    rebuild_sine(table);
    h5_sine = table;
}

/*
 * Sine interpolation, linear, over the 1024-entry period. Phase is 28 bits;
 * the top 10 bits index the table, the low 18 bits interpolate. The
 * interpolation is computed in Q31 directly, so the fractional contribution
 * keeps sub-Q15-LSB precision; at integer table indices the output is
 * exactly h5_sine_at(i) * 65536.
 */
int32_t h5_sine_interp(uint32_t phase)
{
    uint32_t idx, frac;
    int32_t a, b;
    int64_t v;

    phase &= 0x0FFFFFFFu;
    idx = phase >> 18;
    frac = phase & 0x3FFFFu;
    a = h5_sine_at(idx);
    b = h5_sine_at((idx + 1u) & 1023u);
    /* a * 65536, not a << 16: a can be negative and signed left shift of a
     * negative value is undefined. The sum stays inside int32 because the
     * interpolant lies between the two table entries. The fraction is 18
     * bits, while the output step is 2^16 per table unit, so the product is
     * scaled by 2^16/2^18 = 1/4. It goes through the wide-multiply seam. */
    v = (int64_t)a * 65536
        + h5_shr_round(h5_mulwide(b - a, (int32_t)frac), 2u);
    return h5_sat_i32(v);
}

/*
 * Semitone ratios: 2^(n/12) as Q16.16 for n = -48..+48, index n + 48.
 * Regenerate with: python3 tools/gen_semitone_table.py
 */
const int32_t h5_semitone[H5_SEMITONE_COUNT] = {
        4096,     4340,     4598,     4871,     5161,     5468,
        5793,     6137,     6502,     6889,     7298,     7732,
        8192,     8679,     9195,     9742,    10321,    10935,
       11585,    12274,    13004,    13777,    14596,    15464,
       16384,    17358,    18390,    19484,    20643,    21870,
       23170,    24548,    26008,    27554,    29193,    30929,
       32768,    34716,    36781,    38968,    41285,    43740,
       46341,    49097,    52016,    55109,    58386,    61858,
       65536,    69433,    73562,    77936,    82570,    87480,
       92682,    98193,   104032,   110218,   116772,   123715,
      131072,   138866,   147123,   155872,   165140,   174960,
      185364,   196386,   208064,   220436,   233544,   247431,
      262144,   277732,   294247,   311744,   330281,   349920,
      370728,   392772,   416128,   440872,   467088,   494862,
      524288,   555464,   588493,   623487,   660561,   699841,
      741455,   785544,   832255,   881744,   934175,   989724,
     1048576
};

/* tan(pi * f / sr) as Q16 from the shared sine table. theta = pi*f/sr maps
 * into the table as the fraction theta/(2*pi) = f/(2*sr); the cosine comes
 * from a quarter-turn offset in phase. Used for the one-pole pre-filter
 * coefficients at parameter change only. */
int32_t h5_tanpi_q16(int32_t f_q16, uint32_t sample_rate)
{
    int64_t phase;
    int32_t s, c;

    if (sample_rate == 0u) {
        return H5_Q16_ONE;
    }
    phase = ((int64_t)f_q16 << 11) / (int64_t)sample_rate;
    if (phase > INT32_MAX) {
        phase = INT32_MAX;
    }
    s = h5_sine_interp((uint32_t)phase);
    c = h5_sine_interp((uint32_t)(phase + (1 << 26)));
    if (c == 0) {
        return INT32_MAX;
    }
    return h5_sat_i32(((int64_t)s * H5_Q16_ONE) / c);
}

/*
 * x^0.7 as Q14.14 for x = j/64, j = 0..64. The drum voices use this
 * exponent for the NoiseTone and Click controls. It is evaluated only on
 * parameter change and interpolated linearly; Q14 storage is about 0.003%
 * of full scale, far below the control resolution. Regenerate with:
 * python3 tools/gen_pow07_table.py
 */
const int16_t h5_pow07[H5_POW07_SIZE] = {
         0,    891,   1448,   1923,   2353,   2750,
      3125,   3481,   3822,   4150,   4468,   4776,
      5076,   5369,   5654,   5934,   6208,   6478,
      6742,   7002,   7258,   7510,   7759,   8004,
      8246,   8485,   8721,   8955,   9186,   9414,
      9640,   9864,  10086,  10305,  10523,  10738,
     10952,  11164,  11375,  11583,  11791,  11996,
     12200,  12403,  12604,  12804,  13002,  13200,
     13396,  13590,  13784,  13976,  14168,  14358,
     14547,  14735,  14922,  15108,  15293,  15477,
     15660,  15843,  16024,  16204,  16384,
};

int32_t h5_pow07_q16(int32_t x_q16)
{
    uint32_t idx, frac;
    int32_t a, b, q14;

    if (x_q16 <= 0) {
        return 0;
    }
    if (x_q16 >= H5_Q16_ONE) {
        return H5_Q16_ONE;
    }
    idx = (uint32_t)x_q16 >> 10;
    frac = (uint32_t)x_q16 & 0x3FFu;
    a = h5_pow07[idx];
    b = h5_pow07[idx + 1u];
    q14 = a + (int32_t)h5_shr_round(h5_mulwide(b - a, (int32_t)frac), 10u);
    return q14 << 2;
}

int32_t h5_semitone_ratio_interp(int32_t semitones_q16)
{
    int32_t whole, frac, a, b;

    if (semitones_q16 >= H5_SEMITONE_MAX * H5_Q16_ONE) {
        return h5_semitone[H5_SEMITONE_COUNT - 1];
    }
    if (semitones_q16 <= -H5_SEMITONE_MAX * H5_Q16_ONE) {
        return h5_semitone[0];
    }

    /* Arithmetic shift gives floor() for negative values, so the fraction
     * stays in [0, 1) and the interpolation runs between adjacent entries. */
    whole = semitones_q16 >> 16;
    frac = semitones_q16 & 0xFFFF;
    a = h5_semitone[whole + H5_SEMITONE_MAX];
    b = h5_semitone[whole + H5_SEMITONE_MAX + 1];
    return a + (int32_t)h5_shr_round(h5_mulwide(b - a, frac), 16u);
}

/*
 * Soft-clip shaper: the engine mix is saturated with tanh(1.5 * mix), with
 * the lookup table holding the saturation curve as Q15 over the argument
 * range [0, 1.5] in H5_SOFTCLIP_STEPS uniform steps, so entry j holds
 * tanh(1.5 * j / 24). The input is the Q31 mix, so the index is 24 * |mix|
 * and the entry's argument is 1.5 * |mix|; the interpolation is scaled to Q30
 * by the caller, and the result never exceeds tanh(1.5) ~= 0.905, so the
 * output ceiling is one. As |mix| approaches 1.0 the curve is held at the
 * last entry; the flat tail makes the hold inaudible. Negative arguments use
 * odd symmetry.
 * Regenerate with: python3 tools/gen_softclip_table.py
 */
const int16_t h5_softclip[H5_SOFTCLIP_SIZE] = {
         0,   2045,   4075,   6073,   8025,   9919,  11742,  13486,
     15142,  16706,  18173,  19541,  20812,  21986,  23065,  24053,
     24955,  25775,  26518,  27190,  27796,  28340,  28829,  29267,
     29659
};

/* The index scale of the shaper lookup: the table's argument is 1.5 * |mix|
 * and its steps are 1/16 of that argument, so the index is 24 * |mix|. */
#define H5_SOFTCLIP_INPUT_SCALE 24u

int32_t h5_softclip_q31(int32_t m)
{
    uint64_t w;
    uint32_t idx, frac;
    int32_t y0, y1, interp;
    int64_t v;
    int neg = m < 0;

    /* w = |m| * 24 in Q31 index units: the entry index is w >> 31 and the
     * fractional part is bits 30..15 of w. */
    w = (uint64_t)(neg ? -(int64_t)m : (int64_t)m)
        * (uint64_t)H5_SOFTCLIP_INPUT_SCALE;
    idx = (uint32_t)(w >> 31);
    frac = (uint32_t)((w >> 15) & 0xFFFFu);
    if (idx > (H5_SOFTCLIP_SIZE - 2u)) {
        idx = H5_SOFTCLIP_SIZE - 2u;
        frac = 0xFFFFu;
    }
    y0 = h5_softclip[idx];
    y1 = h5_softclip[idx + 1u];
    interp = y0 + (int32_t)h5_shr_round(h5_mulwide(y1 - y0, (int32_t)frac), 16u);
    /* Q15 tanh -> Q30 result (1.0 = 1 << 30). */
    v = (int64_t)interp * 32768;
    v = h5_sat_i32(v);
    return neg ? -v : v;
}


