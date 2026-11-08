/* artful — compressed textures. GPUs read blocks of 4 x 4 texels as they are, at a quarter of the memory or less,
 * and each family has its own: BC on PCs, ETC2 and ASTC on phones. This reads KTX2 files, which hold them,
 * makes BC and ETC2 from plain pixels, and turns blocks back into pixels for the CPU and for GPUs that do not
 * take a format (BC7 and ASTC in blocks.c). */
#include "scene/sceneInternal.h"

#include <stdlib.h>

enum { BC1_RGB = 131, BC1_RGBA = 133, BC3 = 137, BC7 = 145, ETC2_RGB = 147, ETC2_RGBA = 151, ASTC_FIRST = 157, ASTC_LAST = 183 };

uint32_t artfTextureBlockBytes(uint32_t format) { return format == BC1_RGB || format == BC1_RGBA || format == ETC2_RGB ? 8 : 16; }

void artfTextureBlockSize(uint32_t format, int* width, int* height) {
    static const uint8_t sizes[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    const bool astc = format >= ASTC_FIRST && format <= ASTC_LAST;
    *width = astc ? sizes[(format - ASTC_FIRST) / 2][0] : 4;
    *height = astc ? sizes[(format - ASTC_FIRST) / 2][1] : 4;
}

static inline uint32_t pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return a << 24 | r << 16 | g << 8 | b; }
static inline int clampByte(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* ---- BC ---- */

static void colorOf565(uint32_t c, int* rgb) {
    rgb[0] = (int)((c >> 11 & 31) * 255 + 15) / 31;
    rgb[1] = (int)((c >> 5 & 63) * 255 + 31) / 63;
    rgb[2] = (int)((c & 31) * 255 + 15) / 31;
}

static void decodeBc1(const uint8_t* b, uint32_t* out, bool opaqueCodes) {
    const uint32_t c0 = (uint32_t)b[0] | (uint32_t)b[1] << 8, c1 = (uint32_t)b[2] | (uint32_t)b[3] << 8;
    int a[3], z[3];
    colorOf565(c0, a);
    colorOf565(c1, z);
    uint32_t palette[4];
    palette[0] = pack((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], 255);
    palette[1] = pack((uint32_t)z[0], (uint32_t)z[1], (uint32_t)z[2], 255);
    if (c0 > c1 || opaqueCodes) {
        palette[2] = pack((uint32_t)((2 * a[0] + z[0] + 1) / 3), (uint32_t)((2 * a[1] + z[1] + 1) / 3), (uint32_t)((2 * a[2] + z[2] + 1) / 3), 255);
        palette[3] = pack((uint32_t)((a[0] + 2 * z[0] + 1) / 3), (uint32_t)((a[1] + 2 * z[1] + 1) / 3), (uint32_t)((a[2] + 2 * z[2] + 1) / 3), 255);
    } else {
        palette[2] = pack((uint32_t)((a[0] + z[0]) / 2), (uint32_t)((a[1] + z[1]) / 2), (uint32_t)((a[2] + z[2]) / 2), 255);
        palette[3] = 0;
    }
    const uint32_t bits = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
    for (int i = 0; i < 16; ++i) out[i] = palette[bits >> (i * 2) & 3];
}

static void decodeAlpha(const uint8_t* b, uint8_t* out) {
    int a[8];
    a[0] = b[0];
    a[1] = b[1];
    if (a[0] > a[1]) {
        for (int i = 1; i < 7; ++i) a[i + 1] = ((7 - i) * a[0] + i * a[1] + 3) / 7;
    } else {
        for (int i = 1; i < 5; ++i) a[i + 1] = ((5 - i) * a[0] + i * a[1] + 2) / 5;
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= (uint64_t)b[2 + i] << (i * 8);
    for (int i = 0; i < 16; ++i) out[i] = (uint8_t)a[bits >> (i * 3) & 7];
}

static uint32_t to565(const int* c) { return (uint32_t)(c[0] * 31 + 127) / 255 << 11 | (uint32_t)(c[1] * 63 + 127) / 255 << 5 | (uint32_t)(c[2] * 31 + 127) / 255; }

/* Two ends along the box of the colors, then the nearest of four for each texel. */
static void encodeBc1(const uint32_t* px, uint8_t* out) {
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    int mean[3] = {0, 0, 0};
    for (int i = 0; i < 16; ++i)
        for (int c = 0; c < 3; ++c) {
            const int v = (int)(px[i] >> (16 - c * 8) & 255);
            lo[c] = artfMini(lo[c], v);
            hi[c] = artfMaxi(hi[c], v);
            mean[c] += v;
        }
    /* the box has two diagonals that matter here: which way green and blue go along red */
    int rg = 0, rb = 0;
    for (int i = 0; i < 16; ++i) {
        const int r = (int)(px[i] >> 16 & 255) * 16 - mean[0], g = (int)(px[i] >> 8 & 255) * 16 - mean[1], b = (int)(px[i] & 255) * 16 - mean[2];
        rg += (r >> 4) * (g >> 4);
        rb += (r >> 4) * (b >> 4);
    }
    int e0[3] = {hi[0], hi[1], hi[2]}, e1[3] = {lo[0], lo[1], lo[2]};
    if (rg < 0) { e0[1] = lo[1]; e1[1] = hi[1]; }
    if (rb < 0) { e0[2] = lo[2]; e1[2] = hi[2]; }
    uint32_t c0 = to565(e0), c1 = to565(e1);
    if (c0 < c1) { const uint32_t t = c0; c0 = c1; c1 = t; }
    uint32_t bits = 0;
    if (c0 != c1) {
        int a[3], z[3], p[4][3];
        colorOf565(c0, a);
        colorOf565(c1, z);
        for (int c = 0; c < 3; ++c) {
            p[0][c] = a[c];
            p[1][c] = z[c];
            p[2][c] = (2 * a[c] + z[c] + 1) / 3;
            p[3][c] = (a[c] + 2 * z[c] + 1) / 3;
        }
        for (int i = 0; i < 16; ++i) {
            int best = 1 << 30, code = 0;
            for (int k = 0; k < 4; ++k) {
                int d = 0;
                for (int c = 0; c < 3; ++c) {
                    const int e = (int)(px[i] >> (16 - c * 8) & 255) - p[k][c];
                    d += e * e;
                }
                if (d < best) { best = d; code = k; }
            }
            bits |= (uint32_t)code << (i * 2);
        }
    }
    out[0] = (uint8_t)c0; out[1] = (uint8_t)(c0 >> 8); out[2] = (uint8_t)c1; out[3] = (uint8_t)(c1 >> 8);
    out[4] = (uint8_t)bits; out[5] = (uint8_t)(bits >> 8); out[6] = (uint8_t)(bits >> 16); out[7] = (uint8_t)(bits >> 24);
}

static void encodeAlpha(const uint32_t* px, uint8_t* out) {
    int lo = 255, hi = 0;
    for (int i = 0; i < 16; ++i) {
        lo = artfMini(lo, (int)(px[i] >> 24));
        hi = artfMaxi(hi, (int)(px[i] >> 24));
    }
    out[0] = (uint8_t)hi;
    out[1] = (uint8_t)lo;
    int a[8];
    a[0] = hi;
    a[1] = lo;
    for (int i = 1; i < 7; ++i) a[i + 1] = ((7 - i) * hi + i * lo + 3) / 7;
    uint64_t bits = 0;
    if (hi > lo)
        for (int i = 0; i < 16; ++i) {
            int best = 1 << 30, code = 0;
            for (int k = 0; k < 8; ++k) {
                const int e = abs((int)(px[i] >> 24) - a[k]);
                if (e < best) { best = e; code = k; }
            }
            bits |= (uint64_t)code << (i * 3);
        }
    for (int i = 0; i < 6; ++i) out[2 + i] = (uint8_t)(bits >> (i * 8));
}

/* ---- ETC2 ---- */

static const int etcTable[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
static const int etcDistance[8] = {3, 6, 11, 16, 23, 32, 41, 64};
static const int8_t eacTable[16][8] = {{-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12}, {-2, -4, -6, -13, 1, 3, 5, 12},
    {-3, -6, -8, -12, 2, 5, 7, 11}, {-3, -7, -9, -11, 2, 6, 8, 10}, {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10},
    {-2, -6, -8, -10, 1, 5, 7, 9}, {-2, -5, -8, -10, 1, 4, 7, 9}, {-2, -4, -8, -10, 1, 3, 7, 9}, {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9}, {-1, -2, -3, -10, 0, 1, 2, 9}, {-4, -6, -8, -9, 3, 5, 7, 8}, {-3, -5, -7, -9, 2, 4, 6, 8}};

static inline int bits4(int v) { return v << 4 | v; }
static inline int bits5(int v) { return v << 3 | v >> 2; }
static inline int bits6(int v) { return v << 2 | v >> 4; }
static inline int bits7(int v) { return v << 1 | v >> 6; }
static inline int signed3(int v) { return v >= 4 ? v - 8 : v; }

/* Texels are stored by columns in ETC: in out, by rows, as the rest of this file. */
static void decodeEtc(const uint8_t* b, uint32_t* out) {
    const uint32_t high = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    const uint32_t low = (uint32_t)b[4] << 24 | (uint32_t)b[5] << 16 | (uint32_t)b[6] << 8 | b[7];
    const bool differential = high >> 1 & 1;
    const bool flip = high & 1;
    int base[2][3];
    if (!differential) {
        for (int c = 0; c < 3; ++c) {
            base[0][c] = bits4((int)(high >> (28 - c * 8) & 15));
            base[1][c] = bits4((int)(high >> (24 - c * 8) & 15));
        }
    } else {
        const int r = (int)(high >> 27 & 31), g = (int)(high >> 19 & 31), bl = (int)(high >> 11 & 31);
        const int dr = signed3((int)(high >> 24 & 7)), dg = signed3((int)(high >> 16 & 7)), db = signed3((int)(high >> 8 & 7));
        const int r2 = r + dr, g2 = g + dg, b2 = bl + db;
        if (r2 < 0 || r2 > 31) {
            /* T: two colors, and two more a distance from the second */
            const int r1 = (int)((high >> 27 & 3) << 2 | (high >> 24 & 3)), g1 = (int)(high >> 20 & 15), b1 = (int)(high >> 16 & 15);
            const int rr = (int)(high >> 12 & 15), gg = (int)(high >> 8 & 15), bb = (int)(high >> 4 & 15);
            const int d = etcDistance[(high >> 2 & 3) << 1 | (high & 1)];
            const int c0[3] = {bits4(r1), bits4(g1), bits4(b1)}, c1[3] = {bits4(rr), bits4(gg), bits4(bb)};
            uint32_t p[4];
            p[0] = pack((uint32_t)c0[0], (uint32_t)c0[1], (uint32_t)c0[2], 255);
            p[1] = pack((uint32_t)clampByte(c1[0] + d), (uint32_t)clampByte(c1[1] + d), (uint32_t)clampByte(c1[2] + d), 255);
            p[2] = pack((uint32_t)c1[0], (uint32_t)c1[1], (uint32_t)c1[2], 255);
            p[3] = pack((uint32_t)clampByte(c1[0] - d), (uint32_t)clampByte(c1[1] - d), (uint32_t)clampByte(c1[2] - d), 255);
            for (int i = 0; i < 16; ++i) {
                const int x = i >> 2, y = i & 3;
                out[y * 4 + x] = p[(low >> (i + 16) & 1) << 1 | (low >> i & 1)];
            }
            return;
        }
        if (g2 < 0 || g2 > 31) {
            /* H: two colors, each a distance both ways */
            const int r1 = (int)(high >> 27 & 15), g1 = (int)((high >> 24 & 7) << 1 | (high >> 20 & 1));
            const int b1 = (int)((high >> 19 & 1) << 3 | (high >> 15 & 7));
            const int rr = (int)(high >> 11 & 15), gg = (int)(high >> 7 & 15), bb = (int)(high >> 3 & 15);
            const int c0[3] = {bits4(r1), bits4(g1), bits4(b1)}, c1[3] = {bits4(rr), bits4(gg), bits4(bb)};
            const uint32_t v0 = (uint32_t)c0[0] << 16 | (uint32_t)c0[1] << 8 | (uint32_t)c0[2], v1 = (uint32_t)c1[0] << 16 | (uint32_t)c1[1] << 8 | (uint32_t)c1[2];
            const int d = etcDistance[(high >> 2 & 1) << 2 | (high & 1) << 1 | (v0 >= v1 ? 1 : 0)];
            uint32_t p[4];
            p[0] = pack((uint32_t)clampByte(c0[0] + d), (uint32_t)clampByte(c0[1] + d), (uint32_t)clampByte(c0[2] + d), 255);
            p[1] = pack((uint32_t)clampByte(c0[0] - d), (uint32_t)clampByte(c0[1] - d), (uint32_t)clampByte(c0[2] - d), 255);
            p[2] = pack((uint32_t)clampByte(c1[0] + d), (uint32_t)clampByte(c1[1] + d), (uint32_t)clampByte(c1[2] + d), 255);
            p[3] = pack((uint32_t)clampByte(c1[0] - d), (uint32_t)clampByte(c1[1] - d), (uint32_t)clampByte(c1[2] - d), 255);
            for (int i = 0; i < 16; ++i) {
                const int x = i >> 2, y = i & 3;
                out[y * 4 + x] = p[(low >> (i + 16) & 1) << 1 | (low >> i & 1)];
            }
            return;
        }
        if (b2 < 0 || b2 > 31) {
            /* planar: three colors, and a slope between them */
            const uint64_t all = (uint64_t)high << 32 | low;
            const int ro = bits6((int)(all >> 57 & 63)), go = bits7((int)((all >> 56 & 1) << 6 | (all >> 49 & 63)));
            const int bo = bits6((int)((all >> 48 & 1) << 5 | (all >> 43 & 3) << 3 | (all >> 39 & 7)));
            const int rh = bits6((int)((all >> 34 & 31) << 1 | (all >> 32 & 1))), gh = bits7((int)(all >> 25 & 127)), bh = bits6((int)(all >> 19 & 63));
            const int rv = bits6((int)(all >> 13 & 63)), gv = bits7((int)(all >> 6 & 127)), bv = bits6((int)(all & 63));
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    out[y * 4 + x] = pack((uint32_t)clampByte((x * (rh - ro) + y * (rv - ro) + 4 * ro + 2) >> 2), (uint32_t)clampByte((x * (gh - go) + y * (gv - go) + 4 * go + 2) >> 2),
                                          (uint32_t)clampByte((x * (bh - bo) + y * (bv - bo) + 4 * bo + 2) >> 2), 255);
            return;
        }
        base[0][0] = bits5(r); base[0][1] = bits5(g); base[0][2] = bits5(bl);
        base[1][0] = bits5(r2); base[1][1] = bits5(g2); base[1][2] = bits5(b2);
    }
    const int tables[2] = {(int)(high >> 5 & 7), (int)(high >> 2 & 7)};
    for (int i = 0; i < 16; ++i) {
        const int x = i >> 2, y = i & 3, half = flip ? y >> 1 : x >> 1;
        const int code = (int)(low >> (i + 16) & 1) << 1 | (int)(low >> i & 1);
        /* codes: 0 +small, 1 +large, 2 -small, 3 -large */
        const int amount = etcTable[tables[half]][code & 1];
        const int shift = code & 2 ? -amount : amount;
        out[y * 4 + x] = pack((uint32_t)clampByte(base[half][0] + shift), (uint32_t)clampByte(base[half][1] + shift), (uint32_t)clampByte(base[half][2] + shift), 255);
    }
}

static void decodeEac(const uint8_t* b, uint8_t* out) {
    const int base = b[0], multiplier = b[1] >> 4;
    const int8_t* table = eacTable[b[1] & 15];
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits = bits << 8 | b[2 + i];
    for (int i = 0; i < 16; ++i) {
        const int x = i >> 2, y = i & 3;
        const int code = (int)(bits >> (45 - i * 3) & 7);
        out[y * 4 + x] = (uint8_t)clampByte(base + table[code] * (multiplier ? multiplier : 1));
    }
}

/* Each half of the block its mean, in four bits a channel, and the table that fits it best. */
static void encodeEtc(const uint32_t* px, uint8_t* out) {
    int c[2][3];
    int table[2];
    uint32_t low = 0;
    for (int half = 0; half < 2; ++half) {
        int sum[3] = {0, 0, 0};
        for (int y = 0; y < 4; ++y)
            for (int x = half * 2; x < half * 2 + 2; ++x)
                for (int k = 0; k < 3; ++k) sum[k] += (int)(px[y * 4 + x] >> (16 - k * 8) & 255);
        for (int k = 0; k < 3; ++k) c[half][k] = artfClampi((sum[k] / 8 * 15 + 127) / 255, 0, 15);
        int bestError = 1 << 30;
        table[half] = 0;
        for (int t = 0; t < 8; ++t) {
            int error = 0;
            for (int y = 0; y < 4; ++y)
                for (int x = half * 2; x < half * 2 + 2; ++x) {
                    int best = 1 << 30;
                    for (int code = 0; code < 4; ++code) {
                        const int shift = code & 2 ? -etcTable[t][code & 1] : etcTable[t][code & 1];
                        int d = 0;
                        for (int k = 0; k < 3; ++k) {
                            const int e = (int)(px[y * 4 + x] >> (16 - k * 8) & 255) - clampByte(bits4(c[half][k]) + shift);
                            d += e * e;
                        }
                        best = artfMini(best, d);
                    }
                    error += best;
                }
            if (error < bestError) { bestError = error; table[half] = t; }
        }
        for (int y = 0; y < 4; ++y)
            for (int x = half * 2; x < half * 2 + 2; ++x) {
                int best = 1 << 30, chosen = 0;
                for (int code = 0; code < 4; ++code) {
                    const int shift = code & 2 ? -etcTable[table[half]][code & 1] : etcTable[table[half]][code & 1];
                    int d = 0;
                    for (int k = 0; k < 3; ++k) {
                        const int e = (int)(px[y * 4 + x] >> (16 - k * 8) & 255) - clampByte(bits4(c[half][k]) + shift);
                        d += e * e;
                    }
                    if (d < best) { best = d; chosen = code; }
                }
                const int i = x * 4 + y;
                low |= (uint32_t)(chosen & 1) << i | (uint32_t)(chosen >> 1) << (i + 16);
            }
    }
    const uint32_t high = (uint32_t)c[0][0] << 28 | (uint32_t)c[1][0] << 24 | (uint32_t)c[0][1] << 20 | (uint32_t)c[1][1] << 16 | (uint32_t)c[0][2] << 12 | (uint32_t)c[1][2] << 8 | (uint32_t)table[0] << 5 | (uint32_t)table[1] << 2;
    for (int i = 0; i < 4; ++i) {
        out[i] = (uint8_t)(high >> (24 - i * 8));
        out[4 + i] = (uint8_t)(low >> (24 - i * 8));
    }
}

static void encodeEac(const uint32_t* px, uint8_t* out) {
    int lo = 255, hi = 0;
    for (int i = 0; i < 16; ++i) {
        lo = artfMini(lo, (int)(px[i] >> 24));
        hi = artfMaxi(hi, (int)(px[i] >> 24));
    }
    const int base = (lo + hi + 1) / 2;
    int bestError = 1 << 30, bestTable = 0, bestMultiplier = 1;
    for (int t = 0; t < 16; ++t)
        for (int m = 1; m < 16; ++m) {
            int error = 0;
            for (int i = 0; i < 16; ++i) {
                int best = 1 << 30;
                for (int k = 0; k < 8; ++k) best = artfMini(best, abs((int)(px[i] >> 24) - clampByte(base + eacTable[t][k] * m)));
                error += best * best;
            }
            if (error < bestError) { bestError = error; bestTable = t; bestMultiplier = m; }
        }
    uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        const int x = i >> 2, y = i & 3;
        int best = 1 << 30, code = 0;
        for (int k = 0; k < 8; ++k) {
            const int e = abs((int)(px[y * 4 + x] >> 24) - clampByte(base + eacTable[bestTable][k] * bestMultiplier));
            if (e < best) { best = e; code = k; }
        }
        bits |= (uint64_t)code << (45 - i * 3);
    }
    out[0] = (uint8_t)base;
    out[1] = (uint8_t)(bestMultiplier << 4 | bestTable);
    for (int i = 0; i < 6; ++i) out[2 + i] = (uint8_t)(bits >> (40 - i * 8));
}

/* ---- whole textures ---- */

static void decodeBlock(uint32_t format, const uint8_t* b, uint32_t* out) {
    uint8_t alpha[16];
    switch (format) {
    case BC1_RGB:
        decodeBc1(b, out, false);
        for (int i = 0; i < 16; ++i) out[i] |= 0xFF000000u;
        break;
    case BC1_RGBA: decodeBc1(b, out, false); break;
    case BC3:
        decodeAlpha(b, alpha);
        decodeBc1(b + 8, out, true);
        for (int i = 0; i < 16; ++i) out[i] = (out[i] & 0xFFFFFFu) | (uint32_t)alpha[i] << 24;
        break;
    case ETC2_RGB: decodeEtc(b, out); break;
    case BC7: artfDecodeBc7(b, out); break;
    default: /* ETC2_RGBA */
        decodeEac(b, alpha);
        decodeEtc(b + 8, out);
        for (int i = 0; i < 16; ++i) out[i] = (out[i] & 0xFFFFFFu) | (uint32_t)alpha[i] << 24;
        break;
    }
}

void artfTextureDecode(ArtfTextureData* t) {
    if (t->pixels) return;
    size_t total = 0;
    int w = t->width, h = t->height;
    for (int level = 0; level < t->mipCount; ++level) {
        t->offsets[level] = (uint32_t)total;
        total += (size_t)w * h;
        w = artfMaxi(w / 2, 1);
        h = artfMaxi(h / 2, 1);
    }
    t->pixels = artfMalloc(total * 4);
    const uint32_t bytes = artfTextureBlockBytes(t->format);
    int bw, bh;
    artfTextureBlockSize(t->format, &bw, &bh);
    w = t->width;
    h = t->height;
    for (int level = 0; level < t->mipCount; ++level) {
        const uint8_t* block = t->blocks + t->blockOffsets[level];
        uint32_t* to = t->pixels + t->offsets[level];
        for (int by = 0; by < h; by += bh)
            for (int bx = 0; bx < w; bx += bw, block += bytes) {
                uint32_t texels[144];
                if (t->format >= ASTC_FIRST) artfDecodeAstc(block, bw, bh, texels);
                else decodeBlock(t->format, block, texels);
                for (int y = 0; y < bh && by + y < h; ++y)
                    for (int x = 0; x < bw && bx + x < w; ++x) to[(size_t)(by + y) * w + bx + x] = texels[y * bw + x];
            }
        w = artfMaxi(w / 2, 1);
        h = artfMaxi(h / 2, 1);
    }
}

void artfSceneDecodeTextures(ArtfScene* s) {
    if (s->decodedTextures == s->textures.len) return;
    for (size_t i = s->decodedTextures; i < s->textures.len; ++i) artfTextureDecode(&s->textures.items[i]);
    s->decodedTextures = s->textures.len;
}

uint8_t* artfTextureEncode(const ArtfTextureData* t, bool etc, uint32_t* format, size_t* size, uint32_t* offsets) {
    bool opaque = true;
    int w = t->width, h = t->height;
    for (size_t i = 0; i < (size_t)w * h && opaque; ++i) opaque = t->pixels[i] >> 24 == 255;
    *format = etc ? (opaque ? ETC2_RGB : ETC2_RGBA) : (opaque ? BC1_RGB : BC3);
    const uint32_t bytes = artfTextureBlockBytes(*format);
    size_t total = 0;
    for (int level = 0; level < t->mipCount; ++level) {
        offsets[level] = (uint32_t)total;
        total += (size_t)((w + 3) / 4) * ((h + 3) / 4) * bytes;
        w = artfMaxi(w / 2, 1);
        h = artfMaxi(h / 2, 1);
    }
    uint8_t* out = artfMalloc(total);
    *size = total;
    w = t->width;
    h = t->height;
    for (int level = 0; level < t->mipCount; ++level) {
        const uint32_t* from = t->pixels + t->offsets[level];
        uint8_t* block = out + offsets[level];
        for (int by = 0; by < h; by += 4)
            for (int bx = 0; bx < w; bx += 4, block += bytes) {
                uint32_t px[16];
                /* (what is past the edge repeats it) */
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x) px[y * 4 + x] = from[(size_t)artfMini(by + y, h - 1) * w + artfMini(bx + x, w - 1)];
                if (etc) {
                    if (opaque) encodeEtc(px, block);
                    else { encodeEac(px, block); encodeEtc(px, block + 8); }
                } else if (opaque) {
                    encodeBc1(px, block);
                } else {
                    encodeAlpha(px, block);
                    encodeBc1(px, block + 8);
                }
            }
        w = artfMaxi(w / 2, 1);
        h = artfMaxi(h / 2, 1);
    }
    return out;
}

/* ---- KTX2 ---- */

static uint32_t read32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t read64(const uint8_t* p) { return read32(p) | (uint64_t)read32(p + 4) << 32; }

/* sRGB formats are the same blocks: Artful squares colors itself, as it does for all bytes. */
static uint32_t knownFormat(uint32_t f) {
    switch (f) {
    case 131: case 132: return BC1_RGB;
    case 133: case 134: return BC1_RGBA;
    case 137: case 138: return BC3;
    case 145: case 146: return BC7;
    case 147: case 148: return ETC2_RGB;
    case 151: case 152: return ETC2_RGBA;
    default: return f >= ASTC_FIRST && f <= ASTC_LAST + 1 ? f - ((f - ASTC_FIRST) & 1) : 0;
    }
}

ArtfTexture artfSceneAddTextureKtx2(ArtfScene* s, const void* data, size_t size) {
    static const uint8_t mark[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 13, 10, 26, 10};
    const uint8_t* d = data;
    if (size < 80 || memcmp(d, mark, 12)) {
        artfLog("artful: not a KTX2 file");
        return 0;
    }
    const uint32_t vkFormat = read32(d + 12), width = read32(d + 20), height = read32(d + 24), depth = read32(d + 28), layers = read32(d + 32), faces = read32(d + 36);
    uint32_t levels = read32(d + 40);
    const uint32_t scheme = read32(d + 44);
    if (!levels) levels = 1;
    const uint32_t format = knownFormat(vkFormat);
    if (!format || scheme || depth > 1 || layers > 1 || faces != 1 || !width || !height || width > 65536 || height > 65536 || levels > 16 || 80 + (size_t)levels * 24 > size) {
        artfLog("artful: a KTX2 file of format %u, or with what cannot be read here (scheme %u)", vkFormat, scheme);
        return 0;
    }
    const uint32_t bytes = artfTextureBlockBytes(format);
    int blockWidth, blockHeight;
    artfTextureBlockSize(format, &blockWidth, &blockHeight);
    const uint32_t bw = (uint32_t)blockWidth, bh = (uint32_t)blockHeight;
    ArtfTextureData t;
    memset(&t, 0, sizeof t);
    t.width = (int)width;
    t.height = (int)height;
    t.mipCount = (int)levels;
    t.format = format;
    size_t total = 0;
    uint32_t w = width, h = height;
    for (uint32_t i = 0; i < levels; ++i) {
        t.blockOffsets[i] = (uint32_t)total;
        total += (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bytes;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    t.blocks = artfMalloc(total);
    t.blockBytes = total;
    w = width;
    h = height;
    for (uint32_t i = 0; i < levels; ++i) {
        const uint64_t offset = read64(d + 80 + (size_t)i * 24), length = read64(d + 88 + (size_t)i * 24);
        const size_t need = (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bytes;
        if (length != need || offset > size || length > size - offset) {
            artfLog("artful: a KTX2 file that does not hold what it says");
            artfFree(t.blocks);
            return 0;
        }
        memcpy(t.blocks + t.blockOffsets[i], d + offset, need);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return artfSceneStoreTexture(s, &t);
}
