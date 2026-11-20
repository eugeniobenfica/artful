#!/usr/bin/env python3
"""artful-flip against NVIDIA's own FLIP (the flip-evaluator package, github.com/NVlabs/flip): the plan's 0.4.

    python tools/flipCompare.py <artful-flip> <out-dir> [png pairs: reference test ...]

Makes pairs of pictures, LDR (PNG) and HDR (Radiance .hdr, as artful-flip reads them; NVIDIA's is given the same
floats), each pair a difference a test meets: noise, a shift of a pixel, blur, thin lines, a tint; light scaled, bright
points, steps. Adds the pairs given. Prints, for each, the mean error by both and how far apart they are, and exits 1
when any is more than 1e-4 apart (the plan's mark). Needs flip-evaluator and numpy; Artful needs neither.
"""
import os
import struct
import subprocess
import sys
import zlib

import flip_evaluator
import numpy as np

TOLERANCE = 1e-4


def writePng(path, rgb):
    """8-bit RGB, h x w x 3 in 0..1."""
    data = np.clip(np.round(rgb * 255.0), 0, 255).astype(np.uint8)
    h, w, _ = data.shape
    raw = b"".join(b"\x00" + data[y].tobytes() for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def rgbe(rgb):
    """Radiance's RGBE, flat; and the floats it holds, as artful's reader makes them: m 2^(e - 136)."""
    top = rgb.max(axis=2)
    mantissa, exponent = np.frexp(top)
    exponent = np.where(top > 1e-32, exponent + 128, 0).astype(np.int32)
    scale = np.where(exponent > 0, np.ldexp(1.0, 136 - exponent), 0.0)
    m = np.clip(np.floor(rgb * scale[..., None]), 0, 255).astype(np.uint8)
    e = exponent.astype(np.uint8)
    floats = (m.astype(np.float32) * np.where(exponent > 0, np.ldexp(1.0, exponent - 136), 0.0).astype(np.float32)[..., None]).astype(np.float32)
    return np.concatenate([m, e[..., None]], axis=2), floats


def writeHdr(path, rgb):
    pixels, floats = rgbe(rgb)
    h, w, _ = rgb.shape
    with open(path, "wb") as f:
        f.write(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n" + f"-Y {h} +X {w}\n".encode())
        f.write(pixels.tobytes())
    return floats


def pairs(rng):
    """(name, reference, test, hdr)"""
    h = w = 256
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    gradient = np.stack([x / w, y / h, (x + y) / (w + h)], axis=2)
    out = [("gradient and noise", gradient, np.clip(gradient + rng.normal(0, .02, gradient.shape), 0, 1), False)]
    checker = ((np.floor(x / 8) + np.floor(y / 8)) % 2)[..., None].repeat(3, axis=2) * .8 + .1
    out.append(("checker, a pixel along", checker, np.roll(checker, 1, axis=1), False))
    blobs = np.zeros((h, w, 3))
    for _ in range(40):
        cx, cy, r = rng.uniform(0, w), rng.uniform(0, h), rng.uniform(4, 30)
        color = rng.uniform(0, 1, 3)
        mask = ((x - cx) ** 2 + (y - cy) ** 2) < r * r
        blobs[mask] = color
    blurred = sum(np.roll(np.roll(blobs, dy, axis=0), dx, axis=1) for dy in (-1, 0, 1) for dx in (-1, 0, 1)) / 9
    out.append(("blobs, blurred", blobs, blurred, False))
    lines = np.full((h, w, 3), .9)
    lines[(x.astype(int) % 16 == 0) | (y.astype(int) % 16 == 0)] = .1
    thinner = np.full((h, w, 3), .9)
    thinner[(x.astype(int) % 16 == 0)] = .1
    out.append(("lines, half of them", lines, thinner, False))
    out.append(("a tint", blobs, np.clip(blobs + np.array([.05, 0, -.03]), 0, 1), False))
    light = gradient * 20 + .01
    out.append(("light, 5% more", light, light * 1.05, True))
    points = np.full((h, w, 3), .05)
    for _ in range(60):
        points[rng.integers(0, h), rng.integers(0, w)] = rng.uniform(5, 50)
    out.append(("bright points and noise", points, np.abs(points + rng.normal(0, .01, points.shape)), True))
    sky = np.stack([.3 + 2 * y / h, .5 + 2 * y / h, 1.5 + 4 * y / h], axis=2)
    out.append(("sky, in steps", sky, np.floor(sky * 8) / 8, True))
    return out


def ours(tool, a, b):
    got = subprocess.run([tool, a, b], capture_output=True, text=True)
    if got.returncode not in (0, 1):
        raise RuntimeError(got.stderr)
    return float(got.stdout.split()[1])


def main():
    if len(sys.argv) < 3 or len(sys.argv) % 2 == 0:
        print(__doc__)
        return 2
    tool, out = os.path.abspath(sys.argv[1]), sys.argv[2]
    os.makedirs(out, exist_ok=True)
    rng = np.random.default_rng(7)
    rows = []
    for k, (name, ref, test, hdr) in enumerate(pairs(rng)):
        if hdr:
            a, b = os.path.join(out, f"{k}a.hdr"), os.path.join(out, f"{k}b.hdr")
            fa, fb = writeHdr(a, ref), writeHdr(b, test)
            _, theirs, _ = flip_evaluator.evaluate(fa, fb, "HDR", inputsRGB=False, applyMagma=False)
        else:
            a, b = os.path.join(out, f"{k}a.png"), os.path.join(out, f"{k}b.png")
            writePng(a, ref)
            writePng(b, test)
            _, theirs, _ = flip_evaluator.evaluate(a, b, "LDR", applyMagma=False)
        rows.append((name, "HDR" if hdr else "LDR", ours(tool, a, b), float(theirs)))
    given = sys.argv[3:]
    for k in range(0, len(given), 2):
        a, b = given[k], given[k + 1]
        _, theirs, _ = flip_evaluator.evaluate(a, b, "LDR", applyMagma=False)
        rows.append((os.path.basename(a) + " / " + os.path.basename(b), "LDR", ours(tool, a, b), float(theirs)))
    worst = 0.0
    for name, kind, a, b in rows:
        worst = max(worst, abs(a - b))
        print(f"{kind} {name:42} artful {a:.6f}  NVIDIA {b:.6f}  apart {abs(a - b):.2e}{'  PAST 1e-4' if abs(a - b) > TOLERANCE else ''}")
    print(f"{len(rows)} pairs: at most {worst:.2e} apart (the mark: {TOLERANCE:.0e})")
    return 0 if worst <= TOLERANCE else 1


if __name__ == "__main__":
    sys.exit(main())
