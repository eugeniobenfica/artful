/* artful — BC7 and ASTC turned back into pixels, for the CPU and for GPUs that do not take them. All integers, so x86
 * and ARM decode the same bytes. ASTC is its low dynamic range profile: a block in high dynamic range comes out magenta,
 * as one that breaks the format's rules does. */
#include "scene/sceneInternal.h"

static inline uint32_t pack(int r, int g, int b, int a) { return (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b; }
static inline int clampByte(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* n bits from a position, the lowest first. */
static uint32_t bitsAt(const uint8_t* b, int at, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i) {
        const int p = at + i;
        if (p >= 0 && p < 128) v |= (uint32_t)(b[p >> 3] >> (p & 7) & 1) << i;
    }
    return v;
}

/* ---- BC7 ---- */

static const uint16_t partitions2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
    0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
    0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
    0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22};

/* Two bits a texel, the first in the lowest. */
static const uint32_t partitions3[64] = {
    0xAA685050, 0x6A5A5040, 0x5A5A4200, 0x5450A0A8, 0xA5A50000, 0xA0A05050, 0x5555A0A0, 0x5A5A5050,
    0xAA550000, 0xAA555500, 0xAAAA5500, 0x90909090, 0x94949494, 0xA4A4A4A4, 0xA9A59450, 0x2A0A4250,
    0xA5945040, 0x0A425054, 0xA5A5A500, 0x55A0A0A0, 0xA8A85454, 0x6A6A4040, 0xA4A45000, 0x1A1A0500,
    0x0050A4A4, 0xAAA59090, 0x14696914, 0x69691400, 0xA08585A0, 0xAA821414, 0x50A4A450, 0x6A5A0200,
    0xA9A58000, 0x5090A0A8, 0xA8A09050, 0x24242424, 0x00AA5500, 0x24924924, 0x24499224, 0x50A50A50,
    0x500AA550, 0xAAAA4444, 0x66660000, 0xA5A0A5A0, 0x50A050A0, 0x69286928, 0x44AAAA44, 0x66666600,
    0xAA444444, 0x54A854A8, 0x95809580, 0x96969600, 0xA85454A8, 0x80959580, 0xAA141414, 0x96960000,
    0xAAAA1414, 0xA05050A0, 0xA0A5A5A0, 0x96000000, 0x40804080, 0xA9A8A9A8, 0xAAAAAA44, 0x2A4A5254};

static const uint8_t anchors2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
                                     15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15};
static const uint8_t anchors3a[64] = {3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
                                      8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3};
static const uint8_t anchors3b[64] = {15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
                                      15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8};

static const uint8_t blend2[4] = {0, 21, 43, 64};
static const uint8_t blend3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
static const uint8_t blend4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

typedef struct Bc7Mode { uint8_t subsets, partitionBits, rotationBits, selectBits, colorBits, alphaBits, endBit, sharedBit, indexBits, index2Bits; } Bc7Mode;
static const Bc7Mode bc7Modes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0}, {2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
    {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2}, {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0}};

static int bc7Blend(int a, int b, int weight) { return ((64 - weight) * a + weight * b + 32) >> 6; }
static int bc7Weight(int index, int bits) { return bits == 2 ? blend2[index] : bits == 3 ? blend3[index] : blend4[index]; }

static int bc7Expand(int v, int bits) {
    v <<= 8 - bits;
    return v | v >> bits;
}

void artfDecodeBc7(const uint8_t* b, uint32_t* out) {
    int mode = 0;
    while (mode < 8 && !(b[0] >> mode & 1)) ++mode;
    if (mode == 8) {
        for (int i = 0; i < 16; ++i) out[i] = 0;
        return;
    }
    const Bc7Mode* m = &bc7Modes[mode];
    int at = mode + 1;
#define TAKE(n) (at += (n), (int)bitsAt(b, at - (n), (n)))
    const int partition = TAKE(m->partitionBits), rotation = TAKE(m->rotationBits), select = TAKE(m->selectBits);
    const int ends = m->subsets * 2;
    int e[6][4];
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < ends; ++i) e[i][c] = TAKE(m->colorBits);
    for (int i = 0; i < ends; ++i) e[i][3] = m->alphaBits ? TAKE(m->alphaBits) : 255;
    int extra[6] = {0, 0, 0, 0, 0, 0};
    if (m->endBit)
        for (int i = 0; i < ends; ++i) extra[i] = TAKE(1);
    if (m->sharedBit)
        for (int i = 0; i < m->subsets; ++i) extra[i * 2] = extra[i * 2 + 1] = TAKE(1);
    const int more = m->endBit | m->sharedBit;
    for (int i = 0; i < ends; ++i) {
        for (int c = 0; c < 3; ++c) e[i][c] = bc7Expand(e[i][c] << more | (more ? extra[i] : 0), m->colorBits + more);
        if (m->alphaBits) e[i][3] = bc7Expand(e[i][3] << more | (more ? extra[i] : 0), m->alphaBits + more);
    }

    int subset[16], first[16];
    for (int i = 0; i < 16; ++i) {
        subset[i] = m->subsets == 1 ? 0 : m->subsets == 2 ? (int)(partitions2[partition] >> i & 1) : (int)(partitions3[partition] >> (i * 2) & 3);
        const bool anchor = i == 0 || (m->subsets == 2 && i == anchors2[partition]) || (m->subsets == 3 && (i == anchors3a[partition] || i == anchors3b[partition]));
        first[i] = TAKE(anchor ? m->indexBits - 1 : m->indexBits);
    }
    int second[16];
    for (int i = 0; i < 16; ++i) second[i] = m->index2Bits ? TAKE(i == 0 ? m->index2Bits - 1 : m->index2Bits) : 0;
#undef TAKE
    for (int i = 0; i < 16; ++i) {
        const int* lo = e[subset[i] * 2];
        const int* hi = e[subset[i] * 2 + 1];
        int colorIndex = first[i], colorBits = m->indexBits, alphaIndex = first[i], alphaBits = m->indexBits;
        if (m->index2Bits) {
            if (select) { colorIndex = second[i]; colorBits = m->index2Bits; }
            else { alphaIndex = second[i]; alphaBits = m->index2Bits; }
        }
        const int cw = bc7Weight(colorIndex, colorBits), aw = bc7Weight(alphaIndex, alphaBits);
        int v[4] = {bc7Blend(lo[0], hi[0], cw), bc7Blend(lo[1], hi[1], cw), bc7Blend(lo[2], hi[2], cw), m->alphaBits ? bc7Blend(lo[3], hi[3], aw) : 255};
        if (rotation) {
            const int t = v[3];
            v[3] = v[rotation - 1];
            v[rotation - 1] = t;
        }
        out[i] = pack(v[0], v[1], v[2], v[3]);
    }
}

/* ---- ASTC ---- */

/* The sizes numbers can have, by how they are stored: bits, and maybe a trit or a quint. */
typedef struct Range { uint8_t bits, trits, quints; } Range;
static const Range ranges[21] = {{1, 0, 0}, {0, 1, 0}, {2, 0, 0}, {0, 0, 1}, {1, 1, 0}, {3, 0, 0}, {1, 0, 1}, {2, 1, 0}, {4, 0, 0}, {2, 0, 1}, {3, 1, 0},
                                 {5, 0, 0}, {3, 0, 1}, {4, 1, 0}, {6, 0, 0}, {4, 0, 1}, {5, 1, 0}, {7, 0, 0}, {5, 0, 1}, {6, 1, 0}, {8, 0, 0}};

static int storedBits(int count, const Range* r) { return count * r->bits + (count * 8 * r->trits + 4) / 5 * (r->trits ? 1 : 0) + (count * 7 * r->quints + 2) / 3 * (r->quints ? 1 : 0); }

static void unpackTrits(uint32_t t, int* v) {
    int c, t4, t3, t2, t1, t0;
    if ((t >> 2 & 7) == 7) {
        c = (int)((t >> 5 & 7) << 2 | (t & 3));
        t4 = t3 = 2;
    } else {
        c = (int)(t & 31);
        if ((t >> 5 & 3) == 3) { t4 = 2; t3 = (int)(t >> 7 & 1); }
        else { t4 = (int)(t >> 7 & 1); t3 = (int)(t >> 5 & 3); }
    }
    if ((c & 3) == 3) {
        t2 = 2;
        t1 = c >> 4 & 1;
        t0 = (c >> 3 & 1) << 1 | ((c >> 2 & 1) & ~(c >> 3 & 1) & 1);
    } else if ((c >> 2 & 3) == 3) {
        t2 = t1 = 2;
        t0 = c & 3;
    } else {
        t2 = c >> 4 & 1;
        t1 = c >> 2 & 3;
        t0 = (c >> 1 & 1) << 1 | ((c & 1) & ~(c >> 1 & 1) & 1);
    }
    v[0] = t0; v[1] = t1; v[2] = t2; v[3] = t3; v[4] = t4;
}

static void unpackQuints(uint32_t q, int* v) {
    int q2, q1, q0;
    const int low = (int)(q & 1), three = (int)(q >> 3 & 1), four = (int)(q >> 4 & 1);
    if ((q >> 1 & 3) == 3 && (q >> 5 & 3) == 0) {
        q2 = low << 2 | ((four & ~low & 1) << 1) | (three & ~low & 1);
        q1 = q0 = 4;
    } else {
        int c;
        if ((q >> 1 & 3) == 3) {
            q2 = 4;
            c = (int)(q >> 3 & 3) << 3 | ((int)(~q >> 5) & 3) << 1 | low;
        } else {
            q2 = (int)(q >> 5 & 3);
            c = (int)(q & 31);
        }
        if ((c & 7) == 5) { q1 = 4; q0 = c >> 3 & 3; }
        else { q1 = c >> 3 & 3; q0 = c & 7; }
    }
    v[0] = q0; v[1] = q1; v[2] = q2;
}

/* What a last group does not fill counts as zeros, whatever is stored after it. */
static uint32_t bitsUpTo(const uint8_t* b, int at, int n, int end) { return at >= end ? 0 : bitsAt(b, at, n < end - at ? n : end - at); }

/* count numbers from a bit, in groups of five with trits and of three with quints. */
static void readNumbers(const uint8_t* b, int at, int count, const Range* r, int* bitsOut, int* levelOut) {
    const int n = r->bits, end = at + storedBits(count, r);
    int i = 0;
    if (r->trits) {
        static const uint8_t widths[5] = {2, 2, 1, 2, 1};
        while (i < count) {
            uint32_t t = 0;
            int shift = 0, part[5];
            for (int k = 0; k < 5; ++k) {
                part[k] = (int)bitsUpTo(b, at, n, end);
                at += n;
                t |= bitsUpTo(b, at, widths[k], end) << shift;
                at += widths[k];
                shift += widths[k];
            }
            int levels[5];
            unpackTrits(t, levels);
            for (int k = 0; k < 5 && i < count; ++k, ++i) { bitsOut[i] = part[k]; levelOut[i] = levels[k]; }
        }
    } else if (r->quints) {
        static const uint8_t widths[3] = {3, 2, 2};
        while (i < count) {
            uint32_t q = 0;
            int shift = 0, part[3];
            for (int k = 0; k < 3; ++k) {
                part[k] = (int)bitsUpTo(b, at, n, end);
                at += n;
                q |= bitsUpTo(b, at, widths[k], end) << shift;
                at += widths[k];
                shift += widths[k];
            }
            int levels[3];
            unpackQuints(q, levels);
            for (int k = 0; k < 3 && i < count; ++k, ++i) { bitsOut[i] = part[k]; levelOut[i] = levels[k]; }
        }
    } else {
        for (; i < count; ++i, at += n) {
            bitsOut[i] = (int)bitsAt(b, at, n);
            levelOut[i] = 0;
        }
    }
}

static int replicate(int v, int from, int to) {
    int out = 0, have = 0;
    v &= (1 << from) - 1;
    if (!from) return 0;
    while (have < to) {
        const int shift = to - have - from;
        out |= shift >= 0 ? v << shift : v >> -shift;
        have += from;
    }
    return out;
}

/* A number of a color, to 0..255. */
static int colorValue(int bits, int level, const Range* r) {
    const int n = r->bits;
    if (!r->trits && !r->quints) return replicate(bits, n, 8);
    const int a = bits & 1 ? 0x1FF : 0;
    const int bb = bits >> 1 & 1, c = bits >> 2 & 1, d = bits >> 3 & 1, e = bits >> 4 & 1, f = bits >> 5 & 1;
    int B = 0, C = 0;
    if (r->trits) {
        switch (n) {
        case 1: C = 204; break;
        case 2: C = 93; B = (bb << 8) | (bb << 4) | (bb << 2) | (bb << 1); break;
        case 3: C = 44; B = (c << 8) | (bb << 7) | (c << 3) | (bb << 2) | (c << 1) | bb; break;
        case 4: C = 22; B = (d << 8) | (c << 7) | (bb << 6) | (d << 2) | (c << 1) | bb; break;
        case 5: C = 11; B = (e << 8) | (d << 7) | (c << 6) | (bb << 5) | (e << 1) | d; break;
        default: C = 5; B = (f << 8) | (e << 7) | (d << 6) | (c << 5) | (bb << 4) | e; break;
        }
    } else {
        switch (n) {
        case 1: C = 113; break;
        case 2: C = 54; B = (bb << 8) | (bb << 3) | (bb << 2); break;
        case 3: C = 26; B = (c << 8) | (bb << 7) | (c << 2) | (bb << 1) | c; break;
        case 4: C = 13; B = (d << 8) | (c << 7) | (bb << 6) | (d << 1) | c; break;
        default: C = 6; B = (e << 8) | (d << 7) | (c << 6) | (bb << 5) | e; break;
        }
    }
    const int t = (level * C + B) ^ a;
    return (a & 0x80) | t >> 2;
}

/* A weight, to 0..64. */
static int weightValue(int bits, int level, const Range* r) {
    const int n = r->bits;
    int v;
    if (!r->trits && !r->quints) {
        v = replicate(bits, n, 6);
    } else if (!n) {
        static const uint8_t three[3] = {0, 32, 63}, five[5] = {0, 16, 32, 47, 63};
        v = r->trits ? three[level] : five[level];
    } else {
        const int a = bits & 1 ? 0x7F : 0, bb = bits >> 1 & 1, c = bits >> 2 & 1;
        int B = 0, C;
        if (r->trits) {
            C = n == 1 ? 50 : n == 2 ? 23 : 11;
            if (n == 2) B = (bb << 6) | (bb << 2) | bb;
            if (n == 3) B = (c << 6) | (bb << 5) | (c << 1) | bb;
        } else {
            C = n == 1 ? 28 : 13;
            if (n == 2) B = (bb << 6) | (bb << 1);
        }
        const int t = (level * C + B) ^ a;
        v = (a & 0x20) | t >> 2;
    }
    return v > 32 ? v + 1 : v;
}

static uint32_t partitionHash(uint32_t p) {
    p ^= p >> 15;
    p -= p << 17;
    p += p << 7;
    p += p << 4;
    p ^= p >> 5;
    p += p << 16;
    p ^= p >> 7;
    p ^= p >> 3;
    p ^= p << 6;
    p ^= p >> 17;
    return p;
}

static int partitionOf(int seed, int x, int y, int count, bool small) {
    if (small) { x <<= 1; y <<= 1; }
    seed += (count - 1) * 1024;
    const uint32_t r = partitionHash((uint32_t)seed);
    uint32_t s[9];
    for (int i = 0; i < 8; ++i) s[i + 1] = r >> (i * 4) & 15;
    for (int i = 1; i <= 8; ++i) s[i] *= s[i];
    int sh1, sh2;
    if (seed & 1) { sh1 = seed & 2 ? 4 : 5; sh2 = count == 3 ? 6 : 5; }
    else { sh1 = count == 3 ? 6 : 5; sh2 = seed & 2 ? 4 : 5; }
    for (int i = 1; i <= 8; ++i) s[i] >>= i & 1 ? sh1 : sh2;
    int a = (int)(s[1] * (uint32_t)x + s[2] * (uint32_t)y + (r >> 14)) & 63;
    int b = (int)(s[3] * (uint32_t)x + s[4] * (uint32_t)y + (r >> 10)) & 63;
    int c = (int)(s[5] * (uint32_t)x + s[6] * (uint32_t)y + (r >> 6)) & 63;
    int d = (int)(s[7] * (uint32_t)x + s[8] * (uint32_t)y + (r >> 2)) & 63;
    if (count < 4) d = 0;
    if (count < 3) c = 0;
    return a >= b && a >= c && a >= d ? 0 : b >= c && b >= d ? 1 : c >= d ? 2 : 3;
}

static void transferBits(int* a, int* b) {
    *b >>= 1;
    *b |= *a & 0x80;
    *a >>= 1;
    *a &= 0x3F;
    if (*a & 0x20) *a -= 0x40;
}

static void put(int* e, int r, int g, int b, int a) {
    e[0] = clampByte(r); e[1] = clampByte(g); e[2] = clampByte(b); e[3] = clampByte(a);
}

static void putBlue(int* e, int r, int g, int b, int a) { put(e, (r + b) >> 1, (g + b) >> 1, b, a); }

/* The two ends of a color, from its numbers. false: high dynamic range. */
static bool endsOf(int mode, int* v, int* e0, int* e1) {
    switch (mode) {
    case 0: put(e0, v[0], v[0], v[0], 255); put(e1, v[1], v[1], v[1], 255); return true;
    case 1: {
        const int l0 = v[0] >> 2 | (v[1] & 0xC0), l1 = artfMini(l0 + (v[1] & 0x3F), 255);
        put(e0, l0, l0, l0, 255);
        put(e1, l1, l1, l1, 255);
        return true;
    }
    case 4: put(e0, v[0], v[0], v[0], v[2]); put(e1, v[1], v[1], v[1], v[3]); return true;
    case 5:
        transferBits(&v[1], &v[0]);
        transferBits(&v[3], &v[2]);
        put(e0, v[0], v[0], v[0], v[2]);
        put(e1, v[0] + v[1], v[0] + v[1], v[0] + v[1], v[2] + v[3]);
        return true;
    case 6:
        put(e0, v[0] * v[3] >> 8, v[1] * v[3] >> 8, v[2] * v[3] >> 8, 255);
        put(e1, v[0], v[1], v[2], 255);
        return true;
    case 8:
    case 12: {
        const int a0 = mode == 12 ? v[6] : 255, a1 = mode == 12 ? v[7] : 255;
        if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) {
            put(e0, v[0], v[2], v[4], a0);
            put(e1, v[1], v[3], v[5], a1);
        } else {
            putBlue(e0, v[1], v[3], v[5], a1);
            putBlue(e1, v[0], v[2], v[4], a0);
        }
        return true;
    }
    case 9:
    case 13: {
        transferBits(&v[1], &v[0]);
        transferBits(&v[3], &v[2]);
        transferBits(&v[5], &v[4]);
        int a0 = 255, a1 = 255;
        if (mode == 13) {
            transferBits(&v[7], &v[6]);
            a0 = v[6];
            a1 = v[6] + v[7];
        }
        if (v[1] + v[3] + v[5] >= 0) {
            put(e0, v[0], v[2], v[4], a0);
            put(e1, v[0] + v[1], v[2] + v[3], v[4] + v[5], a1);
        } else {
            putBlue(e0, v[0] + v[1], v[2] + v[3], v[4] + v[5], a1);
            putBlue(e1, v[0], v[2], v[4], a0);
        }
        return true;
    }
    case 10:
        put(e0, v[0] * v[3] >> 8, v[1] * v[3] >> 8, v[2] * v[3] >> 8, v[4]);
        put(e1, v[0], v[1], v[2], v[5]);
        return true;
    default: return false;
    }
}

typedef struct AstcMode { int width, height, range; bool two; } AstcMode;

static bool astcMode(uint32_t m, AstcMode* out) {
    int r, w, h;
    const int a = (int)(m >> 5 & 3), b = (int)(m >> 7 & 3);
    bool high = m >> 9 & 1, two = m >> 10 & 1;
    if (m & 3) {
        r = (int)(m >> 4 & 1) | (int)(m & 3) << 1;
        switch (m >> 2 & 3) {
        case 0: w = b + 4; h = a + 2; break;
        case 1: w = b + 8; h = a + 2; break;
        case 2: w = a + 2; h = b + 8; break;
        default:
            if (m >> 8 & 1) { w = (int)(m >> 7 & 1) + 2; h = a + 2; }
            else { w = a + 2; h = (int)(m >> 7 & 1) + 6; }
            break;
        }
    } else {
        r = (int)(m >> 4 & 1) | (int)(m >> 2 & 3) << 1;
        if (!(m >> 2 & 3)) return false;
        switch (m >> 7 & 3) {
        case 0: w = 12; h = a + 2; break;
        case 1: w = a + 2; h = 12; break;
        case 3:
            if (m >> 6 & 1) return false;
            if (m >> 5 & 1) { w = 10; h = 6; }
            else { w = 6; h = 10; }
            break;
        default:
            w = a + 6;
            h = (int)(m >> 9 & 3) + 6;
            high = two = false;
            break;
        }
    }
    if (r < 2) return false;
    /* into `ranges`: 2 3 4 5 6 8 then 10 12 16 20 24 32 */
    out->range = r - 2 + (high ? 6 : 0);
    out->width = w;
    out->height = h;
    out->two = two;
    return true;
}

void artfDecodeAstc(const uint8_t* b, int bw, int bh, uint32_t* out) {
    const int texels = bw * bh;
    const uint32_t wrong = pack(255, 0, 255, 255);
    const uint32_t mode = bitsAt(b, 0, 11);
#define FAIL() do { for (int i = 0; i < texels; ++i) out[i] = wrong; return; } while (0)
    if ((mode & 0x1FF) == 0x1FC) {
        /* one color all over */
        if (mode >> 9 & 1) FAIL();
        const uint32_t c = pack((int)(bitsAt(b, 64, 16) >> 8), (int)(bitsAt(b, 80, 16) >> 8), (int)(bitsAt(b, 96, 16) >> 8), (int)(bitsAt(b, 112, 16) >> 8));
        for (int i = 0; i < texels; ++i) out[i] = c;
        return;
    }
    AstcMode m;
    if (!astcMode(mode, &m) || m.width > bw || m.height > bh) FAIL();
    const int weights = m.width * m.height * (m.two ? 2 : 1);
    const Range* wr = &ranges[m.range];
    const int weightBits = storedBits(weights, wr);
    if (weights > 64 || weightBits < 24 || weightBits > 96) FAIL();
    const int parts = (int)bitsAt(b, 11, 2) + 1;
    if (parts == 4 && m.two) FAIL();

    int modes[4], extra = 0, configBits;
    const int below = 128 - weightBits;
    if (parts == 1) {
        modes[0] = (int)bitsAt(b, 13, 4);
        configBits = 17;
    } else {
        uint32_t code = bitsAt(b, 23, 6);
        if (!(code & 3)) {
            for (int i = 0; i < parts; ++i) modes[i] = (int)(code >> 2);
            configBits = 29;
        } else {
            extra = 3 * parts - 4;
            code |= bitsAt(b, below - extra, extra) << 6;
            const int base = (int)(code & 3) - 1;
            for (int i = 0; i < parts; ++i) {
                const int more = (int)(code >> (2 + i) & 1), low = (int)(code >> (2 + parts + i * 2) & 3);
                modes[i] = (base + more) << 2 | low;
            }
            configBits = 25 + 3 * parts;
        }
    }
    int numbers = 0;
    for (int i = 0; i < parts; ++i) numbers += ((modes[i] >> 2) + 1) * 2;
    const int colorBits = 128 - configBits - weightBits - (m.two ? 2 : 0);
    if (numbers > 18) FAIL();
    int level = -1;
    for (int i = 20; i >= 0 && level < 0; --i)
        if (storedBits(numbers, &ranges[i]) <= colorBits) level = i;
    if (level < 4) FAIL();

    int stored[18], extraLevels[18], values[18];
    readNumbers(b, parts == 1 ? 17 : 29, numbers, &ranges[level], stored, extraLevels);
    for (int i = 0; i < numbers; ++i) values[i] = colorValue(stored[i], extraLevels[i], &ranges[level]);
    int ends[4][2][4];
    for (int i = 0, at = 0; i < parts; ++i) {
        if (!endsOf(modes[i], values + at, ends[i][0], ends[i][1])) FAIL();
        at += ((modes[i] >> 2) + 1) * 2;
    }

    /* the weights go from the block's far end, with each byte's bits turned around */
    uint8_t turned[16];
    for (int i = 0; i < 16; ++i) {
        uint8_t v = b[15 - i], t = 0;
        for (int k = 0; k < 8; ++k) t |= (uint8_t)((v >> k & 1) << (7 - k));
        turned[i] = t;
    }
    int weightStored[64], weightLevels[64], grid[64];
    readNumbers(turned, 0, weights, wr, weightStored, weightLevels);
    for (int i = 0; i < weights; ++i) grid[i] = weightValue(weightStored[i], weightLevels[i], wr);

    const int plane2 = m.two ? (int)bitsAt(b, below - extra - 2, 2) : -1;
    const int seed = parts > 1 ? (int)bitsAt(b, 13, 10) : 0;
    const bool small = texels < 31;
    const int ds = (1024 + bw / 2) / (bw - 1), dt = (1024 + bh / 2) / (bh - 1);
    const int stride = m.two ? 2 : 1;
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            const int gs = (ds * x * (m.width - 1) + 32) >> 6, gt = (dt * y * (m.height - 1) + 32) >> 6;
            const int js = gs >> 4, fs = gs & 15, jt = gt >> 4, ft = gt & 15;
            const int w11 = (fs * ft + 8) >> 4, w10 = ft - w11, w01 = fs - w11, w00 = 16 - fs - ft + w11;
            const int at = js + jt * m.width;
            int plane[2];
            for (int p = 0; p < stride; ++p) {
                const int p00 = grid[at * stride + p];
                const int p01 = js + 1 < m.width ? grid[(at + 1) * stride + p] : 0;
                const int p10 = jt + 1 < m.height ? grid[(at + m.width) * stride + p] : 0;
                const int p11 = js + 1 < m.width && jt + 1 < m.height ? grid[(at + m.width + 1) * stride + p] : 0;
                plane[p] = (p00 * w00 + p01 * w01 + p10 * w10 + p11 * w11 + 8) >> 4;
            }
            const int part = parts > 1 ? partitionOf(seed, x, y, parts, small) : 0;
            int c[4];
            for (int k = 0; k < 4; ++k) {
                const int w = k == plane2 ? plane[1] : plane[0];
                const int lo = ends[part][0][k] * 257, hi = ends[part][1][k] * 257;
                c[k] = ((lo * (64 - w) + hi * w + 32) / 64) >> 8;
            }
            out[y * bw + x] = pack(c[0], c[1], c[2], c[3]);
        }
#undef FAIL
}
