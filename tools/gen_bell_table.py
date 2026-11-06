#!/usr/bin/env python3
"""Generate the bell table for src/h5_tables.c.

The table is one period of a dense, inharmonic wash: white noise shaped by
a smooth magnitude envelope with broad bumps in the low mids and the upper
mids, random phases, then band-limited and peak-normalised to 0.98 full
scale. It is generated from scratch (no recorded samples) with a fixed
seed, so it is byte-identical on every run.

The envelope is defined by control points (rfft bin, level in dB where
0 dB is the bin-0 power) interpolated linearly. The levels are a rounded,
generic metallic-wash voicing; they are design parameters, not measured
data. The phase of every bin is drawn from a seeded PRNG.
"""

import cmath
import math
import random

SIZE = 4096
SEED = 20261106
PEAK = 0.98 * 32767

CONTROL = [(0, 12), (64, 12), (128, 17), (192, 19), (256, 22), (320, 23),
           (384, 27), (448, 29), (512, 22), (576, 19), (640, 17), (704, 15),
           (768, 14), (832, 15), (896, 19), (960, 14), (1024, 15),
           (1088, 17), (1152, 17), (1216, 17), (1280, 10), (1344, 11),
           (1408, 8), (1472, 12), (1536, 18), (1600, 9), (1664, 8),
           (1728, 6), (1792, 8), (1856, 9), (1920, 9), (2048, 9)]


def envelope(n):
    xs = [p[0] for p in CONTROL]
    ys = [p[1] for p in CONTROL]
    env = []
    for i in range(n):
        j = 0
        while j + 1 < len(xs) and xs[j + 1] < i:
            j += 1
        x0, x1 = xs[j], xs[min(j + 1, len(xs) - 1)]
        y0, y1 = ys[j], ys[min(j + 1, len(ys) - 1)]
        if x1 == x0:
            env.append(y0)
        else:
            env.append(y0 + (y1 - y0) * (i - x0) / (x1 - x0))
    return [10.0 ** (v / 10.0) for v in env]


def fft(x):
    n = len(x)
    bits = n.bit_length() - 1
    j = 0
    a = list(x)
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j ^= bit
        if i < j:
            a[i], a[j] = a[j], a[i]
    length = 2
    while length <= n:
        wlen = cmath.exp(2j * math.pi / length)
        for i in range(0, n, length):
            w = 1 + 0j
            half = length >> 1
            for k in range(i, i + half):
                u = a[k]
                v = a[k + half] * w
                a[k] = u + v
                a[k + half] = u - v
                w *= wlen
        length <<= 1
    return a


def synth():
    rng = random.Random(SEED)
    mags = [math.sqrt(v) for v in envelope(SIZE // 2 + 1)]
    spec = [0.0] * SIZE
    for k in range(SIZE // 2 + 1):
        if k == 0 or k == SIZE // 2:
            spec[k] = mags[k]
        else:
            spec[k] = mags[k] * cmath.exp(1j * rng.uniform(0.0, 2.0 * math.pi))
            spec[SIZE - k] = spec[k].conjugate()
    return [v.real for v in fft(spec)]


def main():
    x = synth()
    peak = max(abs(v) for v in x)
    entries = [int(math.floor(v * PEAK / peak + 0.5)) for v in x]

    print("const int16_t h5_bell[H5_BELL_SIZE] = {")
    for i in range(0, SIZE, 8):
        row = ", ".join("%6d" % v for v in entries[i:i + 8])
        if i + 8 < SIZE:
            print("    " + row + ",")
        else:
            print("    " + row)
    print("};")


main()
