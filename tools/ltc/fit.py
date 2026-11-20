#!/usr/bin/env python3
# Writes src/shading/ltc.inc: linearly transformed cosines (Heitz, Dupuy, Hill and Neubelt, 2016) fitted to Artful's
# own highlight (shading.inc's Specular: GGX without 1 / pi, Smith's visibility with Schlick's k = (roughness + 1)^2 / 8),
# for light from a polygon.
#
# For a roughness and n . v, the lobe (its Fresnel left out) is taken as a cosine distribution put through a matrix: a
# direction w of the lobe is M^-1 w / |M^-1 w| of the cosine's. The fit finds M^-1 = [[a, 0, b], [0, c, 0], [d, 0, 1]]
# in the frame of n and v (v in the xz plane, x toward it), as the lobe is symmetric across that plane; and how much
# the lobe gives back in all (its integral over the hemisphere), which the cosine's 1 is scaled by.
#
#   python tools/ltc/fit.py          (writes the table; a minute or two)
#
# The error is the L1 distance of the two distributions, which bounds what their integrals over any polygon differ by.
# Each cell's fit starts from the one before it, the table is walked in one order, and the samples are fixed: the
# table is the same each time it is made. numpy and scipy are needed only to make it, not to build Artful.
import math, os, sys
import numpy as np
from scipy.optimize import minimize

SIZE = 32  # cells each way: roughness from 0 to 1, and sqrt(1 - n . v) from 0 to 1
MIN_ROUGH = .06


def halton(n, base):
    out = np.zeros(n)
    for i in range(n):
        f, r, k = 1.0, 0.0, i + 1
        while k:
            f /= base
            r += f * (k % base)
            k //= base
        out[i] = r
    return out


U1, U2 = halton(4096, 2), halton(4096, 3)


def lobe(w, v, rough):
    """Artful's highlight toward w (rows), from v; n = z. No Fresnel, a light of 1."""
    a = rough * rough
    a2 = a * a
    k = (rough + 1) ** 2 * .125
    ndl = w[:, 2]
    ndv = v[2]
    h = w + v
    h /= np.linalg.norm(h, axis=1, keepdims=True)
    ndh = np.clip(h[:, 2], 0, 1)
    d = ndh * ndh * (a2 - 1) + 1
    D = a2 / (d * d)
    seen = (ndv * (1 - k) + k) * (np.clip(ndl, 0, 1) * (1 - k) + k)
    return np.where(ndl > 0, D * ndl / (4 * seen), 0.0)


def samples(v, rough):
    """Directions from the lobe's normals (GGX, reflected about them) and from the cosine, and their pdf as both."""
    a = max(rough * rough, 1e-4)
    a2 = a * a
    phi = 2 * math.pi * U1
    ct = np.sqrt((1 - U2) / (1 + (a2 - 1) * U2))
    st = np.sqrt(1 - ct * ct)
    h = np.stack([st * np.cos(phi), st * np.sin(phi), ct], 1)
    w1 = 2 * (h @ v)[:, None] * h - v
    r = np.sqrt(U2)
    w2 = np.stack([r * np.cos(phi), r * np.sin(phi), np.sqrt(1 - U2)], 1)
    # (those below the horizon stay, as nothing: the mean is over all that were drawn)
    w = np.concatenate([w1, w2])

    def pdfs(w):
        hh = w + v
        hh /= np.linalg.norm(hh, axis=1, keepdims=True)
        ndh = np.clip(hh[:, 2], 0, 1)
        d = ndh * ndh * (a2 - 1) + 1
        Dn = a2 / (math.pi * d * d)
        p1 = Dn * ndh / (4 * np.clip(hh @ v, 1e-6, None))
        p2 = np.clip(w[:, 2], 0, None) / math.pi
        return .5 * p1 + .5 * p2

    return w, pdfs(w)


def ltcValue(w, m):
    """The distribution through M^-1 (a, b, c, d) toward w: cos(w_o) / pi times the Jacobian."""
    a, b, c, d = m
    x = a * w[:, 0] + b * w[:, 2]
    y = c * w[:, 1]
    z = d * w[:, 0] + w[:, 2]
    length = np.sqrt(x * x + y * y + z * z)
    det = abs(c * (a - b * d))
    return np.clip(z / length, 0, None) / math.pi * det / length ** 3


def fitCell(rough, ndv, start):
    v = np.array([math.sqrt(max(1 - ndv * ndv, 0)), 0, ndv])
    w, pdf = samples(v, rough)
    pdf = np.maximum(pdf, 1e-12)
    f = lobe(w, v, rough)
    total = float(np.mean(f / pdf))
    target = f / max(total, 1e-12)

    def error(m):
        if m[0] <= 1e-4 or m[2] <= 1e-4:
            return 1e30
        g = ltcValue(w, m)
        return float(np.mean(np.abs(g - target) / pdf))

    # (twice: Nelder and Mead's simplex shrinks where it started, and a second start from where it ended goes on)
    best = minimize(error, start, method='Nelder-Mead', options={'xatol': 1e-6, 'fatol': 1e-12, 'maxiter': 4000, 'maxfev': 8000})
    best = minimize(error, best.x, method='Nelder-Mead', options={'xatol': 1e-7, 'fatol': 1e-12, 'maxiter': 4000, 'maxfev': 8000})
    return best.x, total


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
    table = np.zeros((SIZE, SIZE, 5))
    for ti in range(SIZE):
        t = ti / (SIZE - 1)
        ndv = max(1 - t * t, 1e-3)  # (x of the cell: sqrt(1 - n . v))
        start = np.array([1.0, 0.0, 1.0, 0.0])
        for ri in range(SIZE - 1, -1, -1):  # (from rough, where the cosine is near, to smooth)
            rough = MIN_ROUGH + (1 - MIN_ROUGH) * ri / (SIZE - 1)
            m, total = fitCell(rough, ndv, start)
            start = m
            table[ri, ti, :4] = m
            table[ri, ti, 4] = total
        print(f'n . v {ndv:.3f}: smooth a {table[0, ti, 0]:.3g}, rough a {table[-1, ti, 0]:.3g}, all {table[-1, ti, 4]:.3f}', file=sys.stderr)
    out = ['/* Made by tools/ltc/fit.py. Not to be edited.\n',
           ' * Linearly transformed cosines for Artful\'s highlight: of each roughness (rows, from %g to 1) and sqrt(1 - n . v)\n' % MIN_ROUGH,
           ' * (columns, 0 to 1), M^-1 as a, b, c, d of [[a, 0, b], [0, c, 0], [d, 0, 1]], and the lobe\'s integral. */\n',
           '#if ARTF_LTC_SIZE != %d\n#error "sceneInternal.h\'s ARTF_LTC_SIZE is not this table\'s"\n#endif\n' % SIZE,
           '/* (and its ARTF_LTC_MIN_ROUGH is %g) */\n' % MIN_ROUGH,
           'const float artfLtcTable[ARTF_LTC_SIZE * ARTF_LTC_SIZE * 5] = {\n']
    for ri in range(SIZE):
        row = []
        for ti in range(SIZE):
            row.extend('%.9gf' % x for x in table[ri, ti])
        out.append('    ' + ', '.join(row) + ',\n')
    out.append('};\n')
    with open(os.path.join(root, 'src', 'shading', 'ltc.inc'), 'w', newline='\n') as f:
        f.write(''.join(out))


if __name__ == '__main__':
    main()
