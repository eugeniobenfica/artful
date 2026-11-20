#!/usr/bin/env python3
# Writes src/scene/blueNoise.inc: a 64 x 64 tile of blue noise, as the rank of each pixel (0 to 4095), by Ulichney's
# void-and-cluster (1993) on the torus, with a Gaussian of sigma 1.5. A pixel's samples are turned by it (sampling.h's
# artfBlueTurn), so that with a few samples a pixel's error is unlike its neighbours' (Georgiev and Fajardo, 2016).
#
#   python tools/blueNoise.py      (a minute or so; numpy needed to make it, not to build Artful)
#
# From a fixed seed, in one order: the same tile each time it is made.
import os

import numpy as np

SIDE = 64
SIGMA = 1.5


def gaussian():
    d = np.minimum(np.arange(SIDE), SIDE - np.arange(SIDE)).astype(np.float64)
    g = np.exp(-(d[:, None] ** 2 + d[None, :] ** 2) / (2 * SIGMA * SIGMA))
    return np.fft.rfft2(g)


KERNEL = gaussian()


def energy(points):
    return np.fft.irfft2(np.fft.rfft2(points.astype(np.float64)) * KERNEL, s=(SIDE, SIDE))


def tightest(points, e):
    return np.unravel_index(np.argmax(np.where(points, e, -np.inf)), points.shape)


def largest_void(points, e):
    return np.unravel_index(np.argmin(np.where(points, np.inf, e)), points.shape)


def main():
    rng = np.random.default_rng(1993)
    n = SIDE * SIDE
    ones = n // 10
    points = np.zeros((SIDE, SIDE), bool)
    points.flat[rng.choice(n, ones, replace=False)] = True
    # the initial pattern: move the tightest cluster to the largest void until they are one
    while True:
        e = energy(points)
        c = tightest(points, e)
        points[c] = False
        e = energy(points)
        v = largest_void(points, e)
        if v == c:
            points[c] = True
            break
        points[v] = True
    rank = np.zeros((SIDE, SIDE), np.int32)
    proto = points.copy()
    # phase 1: take away the tightest clusters, ranks below
    p = proto.copy()
    for r in range(ones - 1, -1, -1):
        e = energy(p)
        c = tightest(p, e)
        p[c] = False
        rank[c] = r
    # phase 2 and 3: fill the largest voids, ranks above
    p = proto.copy()
    for r in range(ones, n):
        e = energy(p)
        v = largest_void(p, e)
        p[v] = True
        rank[v] = r
    assert sorted(rank.flat) == list(range(n))
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    out = ['/* Made by tools/blueNoise.py. Not to be edited.\n',
           ' * A 64 x 64 tile of blue noise (void-and-cluster, sigma 1.5): each pixel\'s rank, 0 to 4095, by rows. */\n',
           'static const uint16_t artfBlueNoise[64 * 64] = {\n']
    for y in range(SIDE):
        out.append('    ' + ', '.join(str(int(v)) for v in rank[y]) + ',\n')
    out.append('};\n')
    with open(os.path.join(root, 'src', 'scene', 'blueNoise.inc'), 'w', newline='\n') as f:
        f.write(''.join(out))


if __name__ == '__main__':
    main()
