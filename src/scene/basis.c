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

/* ---- UASTC as ASTC ----
 * UASTC is ASTC 4 x 4's, written otherwise: the same endpoints in the same ranges, the same weights, partitions ASTC
 * has. So a block is turned into ASTC's by moving its numbers: the endpoints' trits and quints packed as ASTC packs
 * them, each subset at its partition's place, the weights from the far end; where ASTC would see a subset's endpoints
 * as going down (and contract blue), they change places and its weights turn over, which gives the same texels. */

/* ASTC packs five trits in 8 bits and three quints in 7: of each base-3 (base-5) number, its code */
static const uint8_t astcTritCode[243] = {0, 1, 2, 4, 5, 6, 8, 9, 10, 16, 17, 18, 20, 21, 22, 24, 25, 26, 3, 7, 11, 19, 23, 27, 12, 13, 14, 32, 33, 34, 36, 37, 38, 40, 41, 42, 48, 49, 50, 52, 53, 54, 56, 57, 58, 35, 39, 43, 51, 55, 59, 44, 45, 46, 64, 65, 66, 68, 69, 70, 72, 73, 74, 80, 81, 82, 84, 85, 86, 88, 89, 90, 67, 71, 75, 83, 87, 91, 76, 77, 78, 128, 129, 130, 132, 133, 134, 136, 137, 138, 144, 145, 146, 148, 149, 150, 152, 153, 154, 131, 135, 139, 147, 151, 155, 140, 141, 142, 160, 161, 162, 164, 165, 166, 168, 169, 170, 176, 177, 178, 180, 181, 182, 184, 185, 186, 163, 167, 171, 179, 183, 187, 172, 173, 174, 192, 193, 194, 196, 197, 198, 200, 201, 202, 208, 209, 210, 212, 213, 214, 216, 217, 218, 195, 199, 203, 211, 215, 219, 204, 205, 206, 96, 97, 98, 100, 101, 102, 104, 105, 106, 112, 113, 114, 116, 117, 118, 120, 121, 122, 99, 103, 107, 115, 119, 123, 108, 109, 110, 224, 225, 226, 228, 229, 230, 232, 233, 234, 240, 241, 242, 244, 245, 246, 248, 249, 250, 227, 231, 235, 243, 247, 251, 236, 237, 238, 28, 29, 30, 60, 61, 62, 92, 93, 94, 156, 157, 158, 188, 189, 190, 220, 221, 222, 31, 63, 95, 159, 191, 223, 124, 125, 126};
static const uint8_t astcQuintCode[125] = {0, 1, 2, 3, 4, 8, 9, 10, 11, 12, 16, 17, 18, 19, 20, 24, 25, 26, 27, 28, 5, 13, 21, 29, 6, 32, 33, 34, 35, 36, 40, 41, 42, 43, 44, 48, 49, 50, 51, 52, 56, 57, 58, 59, 60, 37, 45, 53, 61, 14, 64, 65, 66, 67, 68, 72, 73, 74, 75, 76, 80, 81, 82, 83, 84, 88, 89, 90, 91, 92, 69, 77, 85, 93, 22, 96, 97, 98, 99, 100, 104, 105, 106, 107, 108, 112, 113, 114, 115, 116, 120, 121, 122, 123, 124, 101, 109, 117, 125, 30, 102, 103, 70, 71, 38, 110, 111, 78, 79, 46, 118, 119, 86, 87, 54, 126, 127, 94, 95, 62, 39, 47, 55, 63, 7};
/* each pattern's ASTC seed, and where each of its subsets is among ASTC's partitions */
static const uint16_t astcSeeds2[30][3] = {{28, 0, 1}, {20, 0, 1}, {16, 0, 1}, {29, 0, 1}, {91, 0, 1}, {9, 0, 1}, {107, 0, 1}, {72, 0, 1}, {149, 0, 1}, {204, 0, 1}, {50, 0, 1}, {114, 0, 1}, {496, 0, 1}, {17, 0, 1}, {78, 0, 1}, {39, 0, 1}, {252, 0, 1}, {828, 0, 1}, {43, 0, 1}, {156, 0, 1}, {116, 0, 1}, {210, 0, 1}, {476, 0, 1}, {273, 0, 1}, {684, 0, 1}, {359, 0, 1}, {246, 0, 1}, {195, 0, 1}, {694, 0, 1}, {524, 0, 1}};
static const uint16_t astcSeeds3[11][4] = {{260, 0, 1, 2}, {74, 0, 1, 2}, {32, 0, 1, 2}, {156, 0, 1, 2}, {183, 0, 1, 2}, {15, 0, 1, 2}, {745, 0, 1, 2}, {0, 0, 1, 2}, {335, 0, 1, 2}, {902, 0, 1, 2}, {254, 0, 1, 2}};
static const uint16_t astcSeeds7[19][3] = {{36, 0, 1}, {48, 0, 1}, {61, 0, 1}, {137, 0, 1}, {161, 0, 1}, {183, 0, 1}, {226, 0, 1}, {281, 0, 1}, {302, 0, 1}, {307, 0, 1}, {479, 0, 1}, {495, 0, 1}, {593, 0, 1}, {594, 0, 1}, {605, 0, 1}, {799, 0, 1}, {812, 0, 1}, {988, 0, 1}, {993, 0, 1}};

/* (n bits of v, LSB first, at bit `at`; the block's 128 at most) */
static inline void bitsPut(uint8_t* b, uint32_t* at, uint32_t v, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i, ++*at)
        if (*at < 128 && (v >> i & 1)) b[*at >> 3] |= (uint8_t)(1u << (*at & 7));
}

/* count numbers in a range, ASTC's integer sequence: with trits, groups of five (each one's bits, then the code's
 * parts between them); with quints, of three; what a last group does not fill is left out. */
static void numbersPut(uint8_t* b, uint32_t at, const uint8_t* bitsOf, const uint8_t* digit, uint32_t count, int range) {
    const uint32_t n = astcRange[range][0], trits = astcRange[range][1], quints = astcRange[range][2];
    if (!trits && !quints) {
        for (uint32_t i = 0; i < count; ++i) bitsPut(b, &at, bitsOf[i], n);
        return;
    }
    const uint32_t size = trits ? 5 : 3, end = at + count * n + (trits ? (count * 8 + 4) / 5 : (count * 7 + 2) / 3);
    static const uint8_t tritWidths[5] = {2, 2, 1, 2, 1}, quintWidths[3] = {3, 2, 2};
    for (uint32_t g = 0; g * size < count; ++g) {
        uint32_t value = 0, scale = 1;
        for (uint32_t k = 0; k < size; ++k, scale *= trits ? 3 : 5)
            if (g * size + k < count) value += digit[g * size + k] * scale;
        uint32_t packed = trits ? astcTritCode[value] : astcQuintCode[value];
        for (uint32_t k = 0; k < size; ++k) {
            const uint32_t i = g * size + k, width = trits ? tritWidths[k] : quintWidths[k];
            const uint32_t v = i < count ? bitsOf[i] : 0;
            for (uint32_t j = 0; j < n && at < end; ++j, ++at)
                if (v >> j & 1) b[at >> 3] |= (uint8_t)(1u << (at & 7));
            for (uint32_t j = 0; j < width && at < end; ++j, ++at)
                if (packed >> j & 1) b[at >> 3] |= (uint8_t)(1u << (at & 7));
            packed >>= width;
        }
    }
}

/* (ASTC's integer sequence's length) */
static uint32_t numbersLength(uint32_t count, int range) {
    return count * astcRange[range][0] + (astcRange[range][1] ? (count * 8 + 4) / 5 : 0) + (astcRange[range][2] ? (count * 7 + 2) / 3 : 0);
}

/* A UASTC block as ASTC 4 x 4's. false: not a block, or not one ASTC holds the same (none such is made by basisu). */
static bool uastcAstc(const uint8_t* b, uint8_t* out) {
    memset(out, 0, 16);
    int mode = -1;
    for (int m = 0; m < UASTC_MODES && mode < 0; ++m)
        if ((b[0] & ((1u << uastcCode[m][1]) - 1)) == uastcCode[m][0]) mode = m;
    if (mode < 0) return false;
    uint32_t at = uastcCode[mode][1];
    if (mode == UASTC_SOLID) {
        /* ASTC's one color all over: no extent, its colors as 16 bits */
        static const uint8_t solid[8] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        memcpy(out, solid, 8);
        for (int c = 0; c < 4; ++c) {
            const uint32_t v = bitsAt(b, &at, 8);
            out[8 + c * 2] = (uint8_t)v;
            out[9 + c * 2] = (uint8_t)v;
        }
        return true;
    }
    at += uastcHintBits[mode];
    const uint32_t subsets = uastcSubsets[mode], planes = uastcPlanes[mode], comps = uastcComponents[mode];
    const uint8_t* anchors;
    static const uint8_t noAnchors[3];
    const uint8_t* pattern = NULL;
    const uint16_t* seed = NULL;
    if (mode == 2 || mode == 4 || mode == 7 || mode == 9 || mode == 16) {
        const uint32_t p = bitsAt(b, &at, 5);
        if (mode == 7) {
            if (p >= 19) return false;
            anchors = uastcAnchors7[p], pattern = uastcPatterns7[p], seed = astcSeeds7[p];
        } else {
            if (p >= 30) return false;
            anchors = uastcAnchors2[p], pattern = uastcPatterns2[p], seed = astcSeeds2[p];
        }
    } else if (mode == 3) {
        const uint32_t p = bitsAt(b, &at, 4);
        if (p >= 11) return false;
        anchors = uastcAnchors3[p], pattern = uastcPatterns3[p], seed = astcSeeds3[p];
    } else anchors = noAnchors;
    uint32_t ccs = 0;
    if (mode == 6 || mode == 11 || mode == 13) ccs = bitsAt(b, &at, 2);
    else if (mode == 17) ccs = 3;
    /* the endpoints: each one's bits and its trit or quint, and what it means */
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
    uint8_t low[24], digit[24], meant[24];
    uint32_t accumulated = 0, left = 0, next = 0;
    for (uint32_t i = 0; i < values; ++i) {
        low[i] = (uint8_t)bitsAt(b, &at, bits);
        digit[i] = 0;
        if (count) {
            if (!left) {
                accumulated = bundles[next++];
                left = size;
            }
            digit[i] = (uint8_t)(accumulated % base);
            accumulated /= base;
            --left;
        }
        if (digit[i] >= (trits ? 3 : quints ? 5 : 1)) return false;
        meant[i] = (uint8_t)astcEndpoint((uint32_t)digit[i] << bits | low[i], range);
    }
    const uint32_t weightBits = uastcWeightBits[mode];
    uint8_t weight[32];
    for (uint32_t i = 0; i < 16 * planes; ++i) {
        const uint32_t texel = i / planes;
        bool anchor = false;
        for (uint32_t s = 0; s < subsets; ++s) anchor = anchor || anchors[s] == texel;
        weight[i] = (uint8_t)bitsAt(b, &at, anchor ? weightBits - 1 : weightBits);
    }
    if (at > 128) return false;
    /* where ASTC would read a subset's colors as going down (RGB and RGBA: by their sums), they change places, and
     * the subset's weights turn over (ASTC's weights of bits alone are even about the middle) */
    const uint32_t cem = comps == 2 ? 4 : comps == 3 ? 8 : 12, top = (1u << weightBits) - 1;
    for (uint32_t s = 0; s < subsets && comps >= 3; ++s) {
        const uint32_t e = s * comps * 2;
        if ((uint32_t)meant[e + 1] + meant[e + 3] + meant[e + 5] >= (uint32_t)meant[e] + meant[e + 2] + meant[e + 4]) continue;
        for (uint32_t c = 0; c < comps; ++c) {
            uint8_t t = low[e + c * 2]; low[e + c * 2] = low[e + c * 2 + 1]; low[e + c * 2 + 1] = t;
            t = digit[e + c * 2]; digit[e + c * 2] = digit[e + c * 2 + 1]; digit[e + c * 2 + 1] = t;
        }
        for (uint32_t i = 0; i < 16 * planes; ++i)
            if ((pattern ? pattern[i / planes] : 0) == s) weight[i] = (uint8_t)(top - weight[i]);
    }
    /* the block: its mode (a 4 x 4 grid, the weights' range, one or two planes), partitions, endpoints' mode */
    static const uint8_t rOf[6] = {0, 2, 4, 7, 4, 7}, hOf[6] = {0, 0, 0, 0, 1, 1};
    const uint32_t r = rOf[weightBits], blockMode = (r >> 1 & 3) | (r & 1) << 4 | 2u << 5 | (uint32_t)hOf[weightBits] << 9 | (planes == 2 ? 1u : 0u) << 10;
    uint32_t put = 0;
    bitsPut(out, &put, blockMode, 11);
    bitsPut(out, &put, subsets - 1, 2);
    if (subsets == 1) bitsPut(out, &put, cem, 4);
    else {
        bitsPut(out, &put, seed[0], 10);
        bitsPut(out, &put, cem << 2, 6);
    }
    const uint32_t weightCount = 16 * planes, weightLength = weightCount * weightBits;
    const uint32_t colorBits = 128 - put - weightLength - (planes == 2 ? 2 : 0);
    /* (the endpoints' range ASTC finds from what room is left: UASTC's, or the block is not one ASTC reads the same) */
    int level = -1;
    for (int i = 20; i >= 0 && level < 0; --i)
        if (numbersLength(values, i) <= colorBits) level = i;
    if (level != range) return false;
    /* each subset's endpoints at its partition's place */
    uint8_t lowAt[24], digitAt[24];
    for (uint32_t s = 0; s < subsets; ++s) {
        const uint32_t to = (subsets > 1 ? seed[1 + s] : 0) * comps * 2;
        memcpy(lowAt + to, low + s * comps * 2, comps * 2);
        memcpy(digitAt + to, digit + s * comps * 2, comps * 2);
    }
    numbersPut(out, put, lowAt, digitAt, values, range);
    if (planes == 2) {
        uint32_t c = 128 - weightLength - 2;
        bitsPut(out, &c, ccs, 2);
    }
    /* the weights, from the far end: weight bit j at the block's bit 127 - j */
    for (uint32_t i = 0, j = 0; i < weightCount; ++i)
        for (uint32_t k = 0; k < weightBits; ++k, ++j)
            if (weight[i] >> k & 1) out[(127 - j) >> 3] |= (uint8_t)(1u << ((127 - j) & 7));
    return true;
}

bool artfBasisAstc(const uint8_t* uastc, size_t size, uint8_t* astc) {
    for (size_t i = 0; i + 16 <= size; i += 16)
        if (!uastcAstc(uastc + i, astc + i)) return false;
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
/* An ETC1S block as ETC1's: differential, both halves of the one color (no delta) and the one table; its selectors run
 * from -large to +large, ETC's codes are +small, +large, -small, -large, and its texels go by columns. */
static void etc1Block(const uint8_t* ep, const uint8_t* rows, uint8_t* out) {
    static const uint32_t code[4] = {3, 2, 0, 1};
    const uint32_t high = (uint32_t)ep[0] << 27 | (uint32_t)ep[1] << 19 | (uint32_t)ep[2] << 11 | (uint32_t)ep[3] << 5 | (uint32_t)ep[3] << 2 | 2u;
    uint32_t low = 0;
    for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x) {
            const uint32_t c = code[rows[y] >> (x * 2) & 3], i = x * 4 + y;
            low |= (c >> 1) << (i + 16) | (c & 1) << i;
        }
    for (int k = 0; k < 4; ++k) {
        out[k] = (uint8_t)(high >> (24 - 8 * k));
        out[4 + k] = (uint8_t)(low >> (24 - 8 * k));
    }
}

/* A slice into pixels (alpha: its green into their alpha); or, with blocks, into ETC1's blocks (8 bytes each). */
static bool etc1sSlice(const Etc1s* e, const uint8_t* data, size_t size, uint32_t width, uint32_t height, uint32_t* out, bool alpha, uint8_t* blocks) {
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
            if (blocks) {
                etc1Block(e->endpoints[endpoint], e->selectors[selector], blocks + ((size_t)by * across + bx) * 8);
                continue;
            }
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
static bool etc1sLevels(const uint8_t* d, size_t size, uint32_t width, uint32_t height, uint32_t levels, uint32_t* pixels, const uint32_t* offsets, uint8_t* blocks) {
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
        ok = ok && (!blocks || !alphaLength); /* (blocks: ETC1's, which have no alpha) */
        ok = ok && etc1sSlice(&e, d + levelOffset + rgbOffset, rgbLength, w, h, blocks ? NULL : pixels + offsets[i], false, blocks ? blocks + offsets[i] : NULL);
        if (ok && alphaLength) ok = etc1sSlice(&e, d + levelOffset + alphaOffset, alphaLength, w, h, pixels + offsets[i], true, NULL);
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
    if (model == 163) ok = etc1sLevels(d, size, width, height, levels, pixels, offsets, NULL);
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

/* Its blocks, as a GPU takes them: ETC1S without alpha as ETC1's (ETC2's RGB format, 147), UASTC as ASTC 4 x 4 (157);
 * the same texels as artfBasisLevels gives. false: not so (ETC1S with alpha), or not a file this reads. */
bool artfBasisBlocks(const uint8_t* d, size_t size, uint32_t* format, uint8_t** blocksOut, size_t* bytes, uint32_t* offsets) {
    *blocksOut = NULL;
    if (size < 80) return false;
    const uint32_t vkFormat = read32(d + 12), width = read32(d + 20), height = read32(d + 24), depth = read32(d + 28), layers = read32(d + 32), faces = read32(d + 36);
    const uint32_t levels = read32(d + 40) ? read32(d + 40) : 1, scheme = read32(d + 44), dfdOffset = read32(d + 48), dfdLength = read32(d + 52);
    if (vkFormat || depth > 1 || layers > 1 || faces != 1 || !width || !height || width > 16384 || height > 16384 || levels > 16 || 80 + (size_t)levels * 24 > size) return false;
    if (dfdLength < 12 || dfdOffset > size || dfdLength > size - dfdOffset) return false;
    const uint32_t model = d[dfdOffset + 12];
    if (!(model == 163 && scheme == 1) && !(model == 166 && (scheme == 0 || scheme == 2))) return false;
    const size_t blockBytes = model == 163 ? 8 : 16;
    size_t total = 0;
    uint32_t w = width, h = height;
    for (uint32_t i = 0; i < levels; ++i) {
        offsets[i] = (uint32_t)total;
        total += (size_t)((w + 3) / 4) * ((h + 3) / 4) * blockBytes;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    uint8_t* blocks = artfMalloc(total);
    bool ok;
    if (model == 163) ok = etc1sLevels(d, size, width, height, levels, NULL, offsets, blocks);
    else {
        ok = true;
        w = width;
        h = height;
        for (uint32_t i = 0; i < levels && ok; ++i) {
            const uint64_t offset = read64(d + 80 + (size_t)i * 24), length = read64(d + 88 + (size_t)i * 24), full = read64(d + 96 + (size_t)i * 24);
            const size_t n = (size_t)((w + 3) / 4) * ((h + 3) / 4) * 16;
            ok = offset <= size && length <= size - offset && full == n;
            if (ok && scheme == 2) {
                uint8_t* raw = artfMalloc(n);
                ok = artfZstdDecode(d + offset, (size_t)length, raw, n) && artfBasisAstc(raw, n, blocks + offsets[i]);
                artfFree(raw);
            } else if (ok) ok = length == n && artfBasisAstc(d + offset, n, blocks + offsets[i]);
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
    }
    if (!ok) {
        artfFree(blocks);
        return false;
    }
    *format = model == 163 ? 147u : 157u;
    *blocksOut = blocks;
    *bytes = total;
    return true;
}
