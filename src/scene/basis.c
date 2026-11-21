/* artful — Basis Universal's textures, read: UASTC (LDR, 4 x 4) and ETC1S with BasisLZ, as KTX2 files hold them
 * (KHR_texture_basisu), into pixels. What each makes of its blocks is the reference transcoder's, byte for byte (its
 * RGBA32), so that a picture is what its encoder meant.
 *
 * The formats are Binomial's (the UASTC specification and basisu's transcoder, Apache License 2.0): the tables of UASTC's
 * modes, its partitions shared by BC7 and ASTC and their anchors, and ETC1S's codebooks' layout are taken from there; ASTC's
 * and BC7's own (ranges, unquantization, weights) are the Khronos and Microsoft specifications'. */
#include "scene/sceneInternal.h"

static inline uint32_t pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return a << 24 | r << 16 | g << 8 | b; }

/* ---- UASTC ---- */

enum { UASTC_MODES = 19, UASTC_SOLID = 8 };

/* of each mode (as basisu has them): bits a weight, weights' range, endpoints' range, subsets, planes, components */
static const uint8_t uastcWeightBits[UASTC_MODES] = {4, 2, 3, 2, 2, 3, 2, 2, 0, 2, 4, 2, 3, 1, 2, 4, 2, 2, 5};
static const uint8_t uastcEndpointRange[UASTC_MODES] = {19, 20, 8, 7, 12, 20, 18, 12, 0, 8, 13, 13, 19, 20, 20, 20, 20, 20, 11};
static const uint8_t uastcSubsets[UASTC_MODES] = {1, 1, 2, 3, 2, 1, 1, 2, 0, 2, 1, 1, 1, 1, 1, 1, 2, 1, 1};
static const uint8_t uastcPlanes[UASTC_MODES] = {1, 1, 1, 1, 1, 1, 2, 1, 0, 1, 1, 2, 1, 2, 1, 1, 1, 2, 1};
static const uint8_t uastcComponents[UASTC_MODES] = {3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 2, 2, 2, 3};
/* the hints for other formats that come before the endpoints (skipped here) */
static const uint8_t uastcHintBits[UASTC_MODES] = {15, 15, 15, 15, 15, 15, 15, 15, 0, 23, 17, 17, 17, 23, 23, 23, 23, 23, 15};
/* each mode's code (LSB first) and its length; the low 7 bits of a block, to its mode */
static const uint8_t uastcCode[UASTC_MODES + 1][2] = {{0x1, 4}, {0x35, 6}, {0x1D, 5}, {0x3, 5}, {0x13, 5}, {0xB, 5}, {0x1B, 5}, {0x7, 5}, {0x17, 5}, {0xF, 5},
                                                      {0x2, 3}, {0x0, 2}, {0x6, 3}, {0x1F, 5}, {0xD, 5}, {0x5, 7}, {0x15, 6}, {0x25, 6}, {0x9, 4}, {0x45, 7}};

/* partitions both BC7 and ASTC have: 30 of two subsets, 11 of three, and 19 of BC7's of three that are ASTC's of two
 * (mode 7); each texel's subset, and each subset's first texel (whose weight has a bit less) */
static const uint8_t uastcPatterns2[30][16] = {
    {0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1}, {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1}, {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1},
    {1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 0, 0}, {0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1}, {1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}, {1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1}, {1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1}, {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0},
    {1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0},
    {1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1}, {1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0, 1}, {0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0}, {0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0}, {1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 1}, {1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0}, {0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0},
    {1, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 0, 1, 1}, {0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0}, {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1}, {1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0},
    {1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0}, {1, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0}};
static const uint8_t uastcPatterns3[11][16] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2}, {1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 2, 2, 2, 2}, {1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2}, {1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0}, {0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2}, {0, 2, 1, 1, 0, 2, 1, 1, 0, 2, 1, 1, 0, 2, 1, 1}, {2, 0, 0, 0, 2, 0, 0, 0, 2, 1, 1, 1, 2, 1, 1, 1},
    {2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2}, {1, 1, 1, 1, 0, 0, 0, 0, 2, 2, 2, 2, 1, 1, 1, 1}, {0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2}};
static const uint8_t uastcPatterns7[19][16] = {
    {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0}, {1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1},
    {1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1}, {0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0}, {0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, {0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1},
    {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0}, {0, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 0}, {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0},
    {0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1}, {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 0}, {1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0},
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0}, {0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0}, {1, 1, 1, 1, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0}};
static const uint8_t uastcAnchors2[30][2] = {{0, 2}, {0, 3}, {1, 0}, {0, 3}, {7, 0}, {0, 2}, {3, 0}, {7, 0}, {0, 11}, {2, 0}, {0, 7}, {11, 0}, {3, 0}, {8, 0}, {0, 4},
                                             {12, 0}, {1, 0}, {8, 0}, {0, 1}, {0, 2}, {0, 4}, {8, 0}, {1, 0}, {0, 2}, {4, 0}, {0, 1}, {4, 0}, {1, 0}, {4, 0}, {1, 0}};
static const uint8_t uastcAnchors3[11][3] = {{0, 8, 10}, {8, 0, 12}, {4, 0, 12}, {8, 0, 4}, {3, 0, 2}, {0, 1, 3}, {0, 2, 1}, {1, 9, 0}, {1, 2, 0}, {4, 0, 8}, {0, 6, 2}};
static const uint8_t uastcAnchors7[19][2] = {{0, 4}, {0, 2}, {2, 0}, {0, 7}, {8, 0}, {0, 1}, {0, 3}, {0, 1}, {2, 0}, {0, 1}, {0, 8}, {2, 0}, {0, 1}, {0, 7}, {12, 0}, {2, 0}, {9, 0}, {0, 2}, {4, 0}};

/* ASTC's ranges: bits, trits, quints */
static const uint8_t astcRange[21][3] = {{1, 0, 0}, {0, 1, 0}, {2, 0, 0}, {0, 0, 1}, {1, 1, 0}, {3, 0, 0}, {1, 0, 1}, {2, 1, 0}, {4, 0, 0}, {2, 0, 1}, {3, 1, 0},
                                         {5, 0, 0}, {3, 0, 1}, {4, 1, 0}, {6, 0, 0}, {4, 0, 1}, {5, 1, 0}, {7, 0, 0}, {5, 0, 1}, {6, 1, 0}, {8, 0, 0}};
/* ASTC's unquantization of endpoints with trits or quints: which of the value's bits go where in B, and C */
static const char* const astcUnquantB[21] = {"", "", "", "", "000000000", "", "000000000", "b000b0bb0", "", "b0000bb00", "cb000cbcb",
                                             "", "cb0000cbc", "dcb000dcb", "", "dcb0000dc", "edcb000ed", "", "edcb0000e", "fedcb000f", ""};
static const uint16_t astcUnquantC[21] = {0, 0, 0, 0, 204, 0, 113, 93, 0, 54, 44, 0, 26, 22, 0, 13, 11, 0, 6, 5, 0};

/* An endpoint's value (its bits low, its trit or quint above them), as 0 to 255. */
static uint32_t astcEndpoint(uint32_t value, int range) {
    const uint32_t bits = astcRange[range][0];
    const uint32_t low = value & ((1u << bits) - 1), digit = value >> bits;
    if (!astcRange[range][1] && !astcRange[range][2]) {
        /* (the bits repeated down to 8) */
        uint32_t out = 0;
        int left = 8;
        while (left > 0) {
            const int n = left < (int)bits ? left : (int)bits;
            out |= (n < (int)bits ? low >> (bits - (uint32_t)n) : low) << (left - n);
            left -= n;
        }
        return out;
    }
    const uint32_t A = low & 1 ? 511u : 0u;
    uint32_t B = 0;
    for (int i = 0; i < 9; ++i) {
        B <<= 1;
        const char c = astcUnquantB[range][i];
        if (c != '0') B |= low >> (c - 'a') & 1;
    }
    const uint32_t v = (digit * astcUnquantC[range] + B) ^ A;
    return (A & 0x80) | (v >> 2);
}

static const uint8_t weights1[2] = {0, 64}, weights2[4] = {0, 21, 43, 64}, weights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
static const uint8_t weights4[16] = {0, 4, 8, 12, 17, 21, 25, 29, 35, 39, 43, 47, 52, 56, 60, 64};
static const uint8_t weights5[32] = {0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 34, 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62, 64};

/* (ASTC's interpolation, the endpoints as 16 bits each: what is not sRGB) */
static inline uint32_t astcMix(uint32_t l, uint32_t h, uint32_t w) { return ((l << 8 | l) * (64 - w) + (h << 8 | h) * w + 32) >> 6 >> 8; }

/* (n bits, LSB first, from bit `at`: past the block's 128, 0, and the block is found out after) */
static inline uint32_t bitsAt(const uint8_t* b, uint32_t* at, uint32_t n) {
    uint32_t value = 0;
    for (uint32_t i = 0; i < n; ++i, ++*at)
        if (*at < 128) value |= (uint32_t)(b[*at >> 3] >> (*at & 7) & 1) << i;
    return value;
}

/* A block's 16 texels, as basisu's unpack_uastc makes them; false when it is not one. */
static bool uastcBlock(const uint8_t* b, uint32_t* out) {
    int mode = -1;
    for (int m = 0; m < UASTC_MODES && mode < 0; ++m)
        if ((b[0] & ((1u << uastcCode[m][1]) - 1)) == uastcCode[m][0]) mode = m;
    if (mode < 0) return false;
    uint32_t at = uastcCode[mode][1];
    if (mode == UASTC_SOLID) {
        const uint32_t r = bitsAt(b, &at, 8), g = bitsAt(b, &at, 8), bl = bitsAt(b, &at, 8), a = bitsAt(b, &at, 8);
        for (int i = 0; i < 16; ++i) out[i] = pack(r, g, bl, a);
        return true;
    }
    at += uastcHintBits[mode];
    const uint32_t subsets = uastcSubsets[mode], planes = uastcPlanes[mode], comps = uastcComponents[mode];
    const uint8_t *pattern = NULL, *anchors = NULL;
    static const uint8_t none[16], noAnchors[3];
    if (mode == 2 || mode == 4 || mode == 7 || mode == 9 || mode == 16) {
        const uint32_t p = bitsAt(b, &at, 5);
        if (mode == 7) {
            if (p >= 19) return false;
            pattern = uastcPatterns7[p];
            anchors = uastcAnchors7[p];
        } else {
            if (p >= 30) return false;
            pattern = uastcPatterns2[p];
            anchors = uastcAnchors2[p];
        }
    } else if (mode == 3) {
        const uint32_t p = bitsAt(b, &at, 4);
        if (p >= 11) return false;
        pattern = uastcPatterns3[p];
        anchors = uastcAnchors3[p];
    } else {
        pattern = none;
        anchors = noAnchors;
    }
    int ccs = -1;
    if (mode == 6 || mode == 11 || mode == 13) ccs = (int)bitsAt(b, &at, 2);
    else if (mode == 17) ccs = 3;
    /* endpoints: their trits or quints first, five or three to a number, then each one's bits */
    const int range = uastcEndpointRange[mode];
    const uint32_t values = comps * 2 * subsets, bits = astcRange[range][0], trits = astcRange[range][1], quints = astcRange[range][2];
    uint32_t bundles[8], count = 0, size = 0, base = 0;
    if (trits || quints) {
        size = trits ? 5 : 3;
        base = trits ? 3 : 5;
        count = (values + size - 1) / size;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t n = trits ? 8 : 7;
            if (i == count - 1) {
                const uint32_t rest = values - (count - 1) * size;
                if (trits) n = rest == 1 ? 2 : rest == 2 ? 4 : rest == 3 ? 5 : rest == 4 ? 7 : n;
                else n = rest == 1 ? 3 : rest == 2 ? 5 : n;
            }
            bundles[i] = bitsAt(b, &at, n);
        }
    }
    uint8_t endpoint[24];
    uint32_t accumulated = 0, left = 0, next = 0;
    for (uint32_t i = 0; i < values; ++i) {
        uint32_t v = bitsAt(b, &at, bits);
        if (count) {
            if (!left) {
                accumulated = bundles[next++];
                left = size;
            }
            v |= (accumulated % base) << bits;
            accumulated /= base;
            --left;
        }
        endpoint[i] = (uint8_t)astcEndpoint(v, range);
    }
    /* weights, to the end: each subset's anchor (and with two planes, both of the first texel's) a bit less */
    const uint32_t weightBits = uastcWeightBits[mode];
    uint8_t weight[32];
    for (uint32_t i = 0; i < 16 * planes; ++i) {
        const uint32_t texel = i / planes;
        bool anchor = false;
        for (uint32_t s = 0; s < subsets; ++s) anchor = anchor || anchors[s] == texel;
        weight[i] = (uint8_t)bitsAt(b, &at, anchor ? weightBits - 1 : weightBits);
    }
    if (at > 128) return false;
    const uint8_t* table = weightBits == 1 ? weights1 : weightBits == 2 ? weights2 : weightBits == 3 ? weights3 : weightBits == 4 ? weights4 : weights5;
    /* each subset's endpoints, RGBA (LA: luminance to RGB) */
    uint32_t low[3][4], high[3][4];
    for (uint32_t s = 0; s < subsets; ++s) {
        const uint8_t* e = endpoint + s * comps * 2;
        if (comps == 2) {
            for (int c = 0; c < 3; ++c) {
                low[s][c] = e[0];
                high[s][c] = e[1];
            }
            low[s][3] = e[2];
            high[s][3] = e[3];
        } else
            for (uint32_t c = 0; c < 4; ++c) {
                low[s][c] = c < comps ? e[c * 2] : 255u;
                high[s][c] = c < comps ? e[c * 2 + 1] : 255u;
            }
    }
    for (int i = 0; i < 16; ++i) {
        const uint32_t s = pattern[i];
        uint32_t c[4];
        for (int k = 0; k < 4; ++k) {
            const uint32_t w = table[weight[planes == 2 && k == ccs ? i * 2 + 1 : i * planes]];
            c[k] = (k < (int)comps || comps == 2) ? astcMix(low[s][k], high[s][k], w) : 255u;
        }
        out[i] = pack(c[0], c[1], c[2], c[3]);
    }
    return true;
}

bool artfBasisUastc(const uint8_t* blocks, size_t size, uint32_t width, uint32_t height, uint32_t* out) {
    const uint32_t across = (width + 3) / 4, down = (height + 3) / 4;
    if ((size_t)across * down * 16 != size) return false;
    uint32_t texels[16];
    for (uint32_t by = 0; by < down; ++by)
        for (uint32_t bx = 0; bx < across; ++bx) {
            if (!uastcBlock(blocks + ((size_t)by * across + bx) * 16, texels)) return false;
            for (uint32_t y = 0; y < 4 && by * 4 + y < height; ++y)
                for (uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x) out[(size_t)(by * 4 + y) * width + bx * 4 + x] = texels[y * 4 + x];
        }
    return true;
}

/* ---- ETC1S with BasisLZ ---- */


static uint32_t read32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t read64(const uint8_t* p) { return read32(p) | (uint64_t)read32(p + 4) << 32; }

/* bits, LSB first; past the end, 0 */
typedef struct Bits {
    const uint8_t *p, *end;
    uint64_t buffer;
    int count;
} Bits;

static inline uint32_t bitsGet(Bits* b, int n) {
    if (!n) return 0;
    while (b->count < n) {
        b->buffer |= (uint64_t)(b->p < b->end ? *b->p++ : 0u) << b->count;
        b->count += 8;
    }
    const uint32_t value = (uint32_t)(b->buffer & ((1ull << n) - 1));
    b->buffer >>= n;
    b->count -= n;
    return value;
}

/* A canonical Huffman code of up to 16 bits (as deflate's): how many codes of each length, and the symbols in order. */
typedef struct Code {
    uint16_t count[17];
    uint16_t* symbols;
    uint32_t total; /* 0: none */
} Code;

static void codeFree(Code* c) {
    artfFree(c->symbols);
    memset(c, 0, sizeof *c);
}

static bool codeBuild(Code* c, const uint8_t* lengths, uint32_t n) {
    memset(c->count, 0, sizeof c->count);
    uint32_t used = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (lengths[i] > 16) return false;
        ++c->count[lengths[i]];
        used += lengths[i] != 0;
    }
    /* whole, unless there is one symbol */
    uint64_t sum = 0;
    for (int l = 1; l <= 16; ++l) sum += (uint64_t)c->count[l] << (16 - l);
    if (sum != 65536 && used != 1) return false;
    c->count[0] = 0;
    uint16_t first[17];
    first[1] = 0;
    for (int l = 1; l < 16; ++l) first[l + 1] = (uint16_t)(first[l] + c->count[l]);
    c->symbols = artfMalloc((used ? used : 1) * 2);
    for (uint32_t i = 0; i < n; ++i)
        if (lengths[i]) c->symbols[first[lengths[i]]++] = (uint16_t)i;
    c->total = n;
    return true;
}

static uint32_t codeDecode(Bits* b, const Code* c) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l <= 16; ++l) {
        code |= (int)bitsGet(b, 1);
        const int count = c->count[l];
        if (code - first < count) return c->symbols[index + code - first];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return 0; /* (not a code: as the reference, the first symbol) */
}

/* A code as basisu writes one: how many symbols; lengths' own code, by 3 bits each in its order; the lengths, with runs. */
static bool codeRead(Bits* b, Code* c) {
    memset(c, 0, sizeof *c);
    const uint32_t total = bitsGet(b, 14);
    if (!total) return true;
    static const uint8_t order[21] = {17, 18, 19, 20, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15, 16};
    uint8_t sizes[21] = {0};
    const uint32_t n = bitsGet(b, 5);
    if (n < 1 || n > 21) return false;
    for (uint32_t i = 0; i < n; ++i) sizes[order[i]] = (uint8_t)bitsGet(b, 3);
    Code lengthCode;
    if (!codeBuild(&lengthCode, sizes, 21)) return false;
    uint8_t* lengths = artfCalloc(total, 1);
    uint32_t at = 0;
    bool ok = true;
    while (ok && at < total) {
        const uint32_t s = codeDecode(b, &lengthCode);
        if (s <= 16) lengths[at++] = (uint8_t)s;
        else if (s == 17) at += bitsGet(b, 3) + 3;
        else if (s == 18) at += bitsGet(b, 7) + 11;
        else {
            uint32_t run = s == 19 ? bitsGet(b, 2) + 3 : bitsGet(b, 7) + 7;
            ok = at && lengths[at - 1];
            const uint8_t previous = ok ? lengths[at - 1] : 0;
            while (ok && run--) {
                ok = at < total;
                if (ok) lengths[at++] = previous;
            }
        }
    }
    ok = ok && at == total && codeBuild(c, lengths, total);
    codeFree(&lengthCode);
    artfFree(lengths);
    return ok;
}

/* VLC: chunks of n bits, each with a bit after it that says another follows */
static uint32_t vlcRead(Bits* b, int n) {
    uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += n) {
        const uint32_t s = bitsGet(b, n + 1);
        value |= (s & ((1u << n) - 1)) << shift;
        if (!(s >> n & 1)) break;
    }
    return value;
}

/* ETC1's intensity tables */
static const int etc1Intensity[8][4] = {{-8, -2, 2, 8}, {-17, -5, 5, 17}, {-29, -9, 9, 29}, {-42, -13, 13, 42}, {-60, -18, 18, 60}, {-80, -24, 24, 80}, {-106, -33, 33, 106}, {-183, -47, 47, 183}};

typedef struct Etc1s {
    uint8_t (*endpoints)[4]; /* red, green, blue (5 bits), intensity */
    uint8_t (*selectors)[4]; /* each row's four selectors, 2 bits each */
    uint32_t endpointCount, selectorCount, history;
    Code predictions, deltaEndpoints, selectorCode, historyRuns;
} Etc1s;

static void etc1sFree(Etc1s* e) {
    artfFree(e->endpoints);
    artfFree(e->selectors);
    codeFree(&e->predictions);
    codeFree(&e->deltaEndpoints);
    codeFree(&e->selectorCode);
    codeFree(&e->historyRuns);
}

/* The codebooks: endpoints by deltas, selectors raw or by deltas (as basisu's decode_palettes); and the tables (decode_tables). */
static bool etc1sCodebooks(Etc1s* e, const uint8_t* endpoints, size_t endpointBytes, const uint8_t* selectors, size_t selectorBytes, const uint8_t* tables, size_t tableBytes) {
    Bits b = {tables, tables + tableBytes, 0, 0};
    if (!codeRead(&b, &e->predictions) || !e->predictions.total || !codeRead(&b, &e->deltaEndpoints) || !e->deltaEndpoints.total || !codeRead(&b, &e->selectorCode) ||
        !e->selectorCode.total || !codeRead(&b, &e->historyRuns) || !e->historyRuns.total)
        return false;
    e->history = bitsGet(&b, 13);
    if (!e->history) return false;
    Bits p = {endpoints, endpoints + endpointBytes, 0, 0};
    Code delta[3], intensity;
    memset(delta, 0, sizeof delta);
    memset(&intensity, 0, sizeof intensity);
    bool ok = codeRead(&p, &delta[0]) && codeRead(&p, &delta[1]) && codeRead(&p, &delta[2]) && codeRead(&p, &intensity) && delta[0].total && delta[1].total && delta[2].total && intensity.total;
    if (ok) {
        const bool gray = bitsGet(&p, 1) != 0;
        int previous[3] = {16, 16, 16}, previousIntensity = 0;
        e->endpoints = artfMalloc((size_t)e->endpointCount * 4);
        for (uint32_t i = 0; i < e->endpointCount; ++i) {
            previousIntensity = (int)(codeDecode(&p, &intensity) + (uint32_t)previousIntensity) & 7;
            e->endpoints[i][3] = (uint8_t)previousIntensity;
            for (int c = 0; c < (gray ? 1 : 3); ++c) {
                const Code* model = previous[c] <= 9 ? &delta[0] : previous[c] <= 21 ? &delta[1] : &delta[2];
                previous[c] = (previous[c] + (int)codeDecode(&p, model)) & 31;
                e->endpoints[i][c] = (uint8_t)previous[c];
            }
            if (gray) e->endpoints[i][1] = e->endpoints[i][2] = e->endpoints[i][0];
        }
    }
    for (int k = 0; k < 3; ++k) codeFree(&delta[k]);
    codeFree(&intensity);
    if (!ok) return false;
    Bits s = {selectors, selectors + selectorBytes, 0, 0};
    if (bitsGet(&s, 1) || bitsGet(&s, 1)) return false; /* (global or hybrid codebooks: none in KTX2) */
    e->selectors = artfMalloc((size_t)e->selectorCount * 4);
    if (bitsGet(&s, 1)) {
        for (uint32_t i = 0; i < e->selectorCount; ++i)
            for (int j = 0; j < 4; ++j) e->selectors[i][j] = (uint8_t)bitsGet(&s, 8);
        return true;
    }
    Code model;
    if (!codeRead(&s, &model) || (e->selectorCount > 1 && !model.total)) {
        codeFree(&model);
        return false;
    }
    uint8_t previousRow[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < e->selectorCount; ++i)
        for (int j = 0; j < 4; ++j) {
            previousRow[j] = (uint8_t)(i ? codeDecode(&s, &model) ^ previousRow[j] : bitsGet(&s, 8));
            e->selectors[i][j] = previousRow[j];
        }
    codeFree(&model);
    return true;
}

/* A slice: its blocks' endpoints and selectors, and their colors into out (width x height pixels); alpha: only into the
 * pixels' alpha, from green (as basisu's transcode_slice, to RGBA32 and A32). */
static bool etc1sSlice(const Etc1s* e, const uint8_t* data, size_t size, uint32_t width, uint32_t height, uint32_t* out, bool alpha) {
    const uint32_t across = (width + 3) / 4, down = (height + 3) / 4, total = across * down;
    Bits b = {data, data + size, 0, 0};
    /* (of each column, the row before's and this one's: prediction bits for the 2 x 2 group, the endpoint) */
    uint8_t* predictionBits[2] = {artfCalloc(across, 1), artfCalloc(across, 1)};
    uint16_t* endpointOf[2] = {artfCalloc(across, 2), artfCalloc(across, 2)};
    int32_t* history = artfCalloc(e->history, 4);
    uint32_t rover = e->history / 2, runLeft = 0, bitsNow = 0, previousEndpoint = 0;
    int previousSymbol = 0, repeat = 0;
    bool ok = true;
    for (uint32_t by = 0; by < down && ok; ++by) {
        const uint32_t row = by & 1;
        for (uint32_t bx = 0; bx < across && ok; ++bx) {
            if (!(bx & 1)) {
                if (!(by & 1)) {
                    if (repeat) {
                        --repeat;
                        bitsNow = (uint32_t)previousSymbol;
                    } else {
                        bitsNow = codeDecode(&b, &e->predictions);
                        if (bitsNow == 256) {
                            repeat = (int)vlcRead(&b, 4) + 3 - 1;
                            bitsNow = (uint32_t)previousSymbol;
                        } else previousSymbol = (int)bitsNow;
                    }
                    predictionBits[row ^ 1][bx] = (uint8_t)(bitsNow >> 4);
                } else bitsNow = predictionBits[row][bx];
            }
            uint32_t endpoint;
            const uint32_t prediction = bitsNow & 3;
            bitsNow >>= 2;
            if (prediction == 0) {
                ok = bx > 0;
                endpoint = previousEndpoint;
            } else if (prediction == 1) {
                ok = by > 0;
                endpoint = endpointOf[row ^ 1][bx];
            } else if (prediction == 2) {
                ok = bx > 0 && by > 0;
                endpoint = ok ? endpointOf[row ^ 1][bx - 1] : 0;
            } else {
                endpoint = codeDecode(&b, &e->deltaEndpoints) + previousEndpoint;
                if (endpoint >= e->endpointCount) endpoint -= e->endpointCount;
            }
            endpointOf[row][bx] = (uint16_t)endpoint;
            previousEndpoint = endpoint;
            /* the selector: by its code, from the history of those used, or the history's first again for a run */
            uint32_t symbol;
            if (runLeft) {
                --runLeft;
                symbol = e->selectorCount;
            } else {
                symbol = codeDecode(&b, &e->selectorCode);
                if (symbol == e->history + e->selectorCount) {
                    const uint32_t run = codeDecode(&b, &e->historyRuns);
                    runLeft = (run == 63 ? vlcRead(&b, 7) : run) + 3;
                    ok = ok && runLeft <= total;
                    symbol = e->selectorCount;
                    --runLeft;
                }
            }
            uint32_t selector;
            if (symbol >= e->selectorCount) {
                const uint32_t at = symbol - e->selectorCount;
                ok = ok && at < e->history;
                selector = ok ? (uint32_t)history[at] : 0;
                if (ok && at) {
                    const int32_t x = history[at / 2];
                    history[at / 2] = history[at];
                    history[at] = x;
                }
            } else {
                selector = symbol;
                history[rover++] = (int32_t)selector;
                if (rover == e->history) rover = e->history / 2;
            }
            ok = ok && endpoint < e->endpointCount && selector < e->selectorCount;
            if (!ok) break;
            /* its colors */
            const uint8_t* ep = e->endpoints[endpoint];
            const int* table = etc1Intensity[ep[3]];
            int colors[4][3];
            for (int k = 0; k < 4; ++k)
                for (int c = 0; c < 3; ++c) {
                    const int v = ((ep[c] << 3) | (ep[c] >> 2)) + table[k];
                    colors[k][c] = v < 0 ? 0 : v > 255 ? 255 : v;
                }
            for (uint32_t y = 0; y < 4 && by * 4 + y < height; ++y) {
                const uint32_t s = e->selectors[selector][y];
                uint32_t* line = out + (size_t)(by * 4 + y) * width + bx * 4;
                for (uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x) {
                    const int* c = colors[s >> (x * 2) & 3];
                    line[x] = alpha ? (line[x] & 0x00FFFFFFu) | (uint32_t)c[1] << 24 : pack((uint32_t)c[0], (uint32_t)c[1], (uint32_t)c[2], 255);
                }
            }
        }
    }
    artfFree(predictionBits[0]);
    artfFree(predictionBits[1]);
    artfFree(endpointOf[0]);
    artfFree(endpointOf[1]);
    artfFree(history);
    return ok;
}

/* The levels of an ETC1S file (scheme 1): its global data (each image's slices, the codebooks, the tables), then each
 * level's slices, color and alpha. */
static bool etc1sLevels(const uint8_t* d, size_t size, uint32_t width, uint32_t height, uint32_t levels, uint32_t* pixels, const uint32_t* offsets) {
    const uint64_t sgdOffset = read64(d + 64), sgdLength = read64(d + 72);
    if (sgdOffset > size || sgdLength > size - sgdOffset || sgdLength < 20 + (uint64_t)levels * 20) return false;
    const uint8_t* g = d + sgdOffset;
    Etc1s e;
    memset(&e, 0, sizeof e);
    e.endpointCount = (uint32_t)g[0] | (uint32_t)g[1] << 8;
    e.selectorCount = (uint32_t)g[2] | (uint32_t)g[3] << 8;
    const uint32_t endpointBytes = read32(g + 4), selectorBytes = read32(g + 8), tableBytes = read32(g + 12), extendedBytes = read32(g + 16);
    const uint64_t need = 20 + (uint64_t)levels * 20 + endpointBytes + selectorBytes + tableBytes + extendedBytes;
    if (!e.endpointCount || !e.selectorCount || !endpointBytes || !selectorBytes || !tableBytes || need > sgdLength) return false;
    const uint8_t* books = g + 20 + (size_t)levels * 20;
    bool ok = etc1sCodebooks(&e, books, endpointBytes, books + endpointBytes, selectorBytes, books + endpointBytes + selectorBytes, tableBytes);
    uint32_t w = width, h = height;
    for (uint32_t i = 0; i < levels && ok; ++i) {
        const uint8_t* desc = g + 20 + (size_t)i * 20;
        const uint64_t levelOffset = read64(d + 80 + (size_t)i * 24), levelLength = read64(d + 88 + (size_t)i * 24);
        const uint32_t rgbOffset = read32(desc + 4), rgbLength = read32(desc + 8), alphaOffset = read32(desc + 12), alphaLength = read32(desc + 16);
        ok = levelOffset <= size && levelLength <= size - levelOffset && rgbLength && (uint64_t)rgbOffset + rgbLength <= levelLength && (uint64_t)alphaOffset + alphaLength <= levelLength;
        ok = ok && etc1sSlice(&e, d + levelOffset + rgbOffset, rgbLength, w, h, pixels + offsets[i], false);
        if (ok && alphaLength) ok = etc1sSlice(&e, d + levelOffset + alphaOffset, alphaLength, w, h, pixels + offsets[i], true);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    etc1sFree(&e);
    return ok;
}

/* ---- KTX2 ---- */

bool artfBasisLevels(const uint8_t* d, size_t size, uint32_t** pixelsOut, uint32_t* offsets) {
    *pixelsOut = NULL;
    if (size < 80) return false;
    const uint32_t vkFormat = read32(d + 12), width = read32(d + 20), height = read32(d + 24), depth = read32(d + 28), layers = read32(d + 32), faces = read32(d + 36);
    const uint32_t levels = read32(d + 40) ? read32(d + 40) : 1, scheme = read32(d + 44), dfdOffset = read32(d + 48), dfdLength = read32(d + 52);
    if (vkFormat || depth > 1 || layers > 1 || faces != 1 || !width || !height || width > 16384 || height > 16384 || levels > 16 || 80 + (size_t)levels * 24 > size) return false;
    if (dfdLength < 12 || dfdOffset > size || dfdLength > size - dfdOffset) return false;
    /* the data format descriptor's color model: ETC1S 163, UASTC 166 */
    const uint32_t model = d[dfdOffset + 12];
    if (!(model == 163 && scheme == 1) && !(model == 166 && (scheme == 0 || scheme == 2))) return false;
    size_t total = 0;
    uint32_t w = width, h = height;
    for (uint32_t i = 0; i < levels; ++i) {
        offsets[i] = (uint32_t)total;
        total += (size_t)w * h;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    uint32_t* pixels = artfMalloc(total * 4);
    bool ok = true;
    if (model == 163) ok = etc1sLevels(d, size, width, height, levels, pixels, offsets);
    else {
        w = width;
        h = height;
        for (uint32_t i = 0; i < levels && ok; ++i) {
            const uint64_t offset = read64(d + 80 + (size_t)i * 24), length = read64(d + 88 + (size_t)i * 24), full = read64(d + 96 + (size_t)i * 24);
            const size_t blocks = (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16;
            ok = offset <= size && length <= size - offset && full == blocks;
            if (ok && scheme == 2) {
                uint8_t* raw = artfMalloc(blocks);
                ok = artfZstdDecode(d + offset, (size_t)length, raw, blocks) && artfBasisUastc(raw, blocks, w, h, pixels + offsets[i]);
                artfFree(raw);
            } else if (ok) ok = length == blocks && artfBasisUastc(d + offset, blocks, w, h, pixels + offsets[i]);
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
    }
    if (!ok) {
        artfFree(pixels);
        return false;
    }
    *pixelsOut = pixels;
    return true;
}
