/* artful — PNG (all color types, Adam7) and JPEG (baseline and progressive), for textures. The code is
 * cascade's (src/image/image.c), copied and not linked: Artful does not depend on it. Pixels are BGRA, with
 * alpha as it is, as artfSceneAddTexture takes them. */
#include "artful/gltf.h"

#include <stdlib.h>

typedef ARTF_VEC(uint8_t) Bytes;
typedef struct Picture { int width, height; uint32_t* pixels; } Picture;

static Picture* pictureNew(int width, int height) {
    Picture* p = ARTF_NEW(Picture);
    p->width = width;
    p->height = height;
    p->pixels = artfCalloc((size_t)width * height + 1, 4);
    return p;
}
static void pictureFree(Picture* p) {
    if (!p) return;
    artfFree(p->pixels);
    artfFree(p);
}
#define pictureRow(p, y) ((p)->pixels + (size_t)(y) * (p)->width)
#define vecResize(v, n) do { artfVecReserve((v), (n)); (v)->len = (n); } while (0)
#define vecAppend(v, p, n) do { size_t count_ = (n); artfVecReserve((v), (v)->len + count_); memcpy((v)->items + (v)->len, (p), count_ * sizeof *(v)->items); (v)->len += count_; } while (0)
#define vecClear(v) ((v)->len = 0)
#define ONCE(flag, fn) do { if (!(flag)) { fn(); (flag) = true; } } while (0)
static size_t minz(size_t a, size_t b) { return a < b ? a : b; }

/* ---- inflate --------------------------------------------------------------------------------- */

/* LSB-first bits. The buffer is kept filled ahead (zero bytes past the end, counted in pad); reading
 * any of those pad bits is an overflow. */
typedef struct BitReader {
    const uint8_t* p;
    size_t n, i;
    uint32_t buf;
    int cnt, pad;
    bool overflow;
} BitReader;

static inline void refill(BitReader* br) {
    while (br->cnt <= 24) {
        if (br->i < br->n) br->buf |= (uint32_t)br->p[br->i++] << br->cnt;
        else ++br->pad;
        br->cnt += 8;
    }
}

static inline void consume(BitReader* br, int k) {
    br->buf >>= k;
    br->cnt -= k;
    if (br->pad * 8 > br->cnt) br->overflow = true;
}

/* k bits, k <= 25 */
static uint32_t bits(BitReader* br, int k) {
    refill(br);
    const uint32_t v = br->buf & ((1u << k) - 1);
    consume(br, k);
    return v;
}

static void alignByte(BitReader* br) { consume(br, br->cnt % 8); }

/* Bytes of the input read so far (a byte partly read counts). */
static size_t bytesRead(const BitReader* br) {
    const int buffered = br->cnt - br->pad * 8;
    return br->i - (buffered > 0 ? (size_t)buffered / 8 : 0);
}

enum { FAST_BITS = 9 };

typedef struct Huffman {
    uint16_t counts[16];
    uint16_t symbols[320];
    uint16_t fast[1 << FAST_BITS]; /* the next FAST_BITS bits -> symbol << 4 | length; 0: decoded the long way */
} Huffman;

static void huffBuild(Huffman* h, const uint8_t* lengths, int n) {
    memset(h->counts, 0, sizeof h->counts);
    for (int i = 0; i < n; ++i) h->counts[lengths[i]]++;
    h->counts[0] = 0;
    uint16_t offs[16];
    offs[1] = 0;
    for (int i = 1; i < 15; ++i) offs[i + 1] = offs[i] + h->counts[i];
    for (int i = 0; i < n; ++i) if (lengths[i]) h->symbols[offs[lengths[i]]++] = (uint16_t)i;
    /* the short codes, bit-reversed (they are read from the low bit) */
    memset(h->fast, 0, sizeof h->fast);
    int code = 0, index = 0;
    for (int len = 1; len <= FAST_BITS; ++len) {
        for (int k = 0; k < h->counts[len]; ++k, ++code, ++index) {
            if (code >= (1 << len)) return; /* (over-subscribed: the long way decides) */
            int rev = 0;
            for (int b = 0; b < len; ++b) rev |= ((code >> b) & 1) << (len - 1 - b);
            for (int j = rev; j < (1 << FAST_BITS); j += 1 << len) h->fast[j] = (uint16_t)(h->symbols[index] << 4 | len);
        }
        code <<= 1;
    }
}

static int huffDecode(const Huffman* h, BitReader* br) {
    refill(br);
    const uint16_t e = h->fast[br->buf & ((1u << FAST_BITS) - 1)];
    if (e) {
        consume(br, e & 15);
        return br->overflow ? -1 : e >> 4;
    }
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= (int)((br->buf >> (len - 1)) & 1);
        int count = h->counts[len];
        if (code - count < first) {
            consume(br, len);
            return br->overflow ? -1 : h->symbols[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    consume(br, 15);
    return -1;
}

static const uint16_t kLenBase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t kLenExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t kDistBase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t kDistExtra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static bool inflateBlock(BitReader* br, Bytes* out, const Huffman* lit, const Huffman* dist, size_t maxSize) {
    for (;;) {
        int sym = huffDecode(lit, br);
        if (sym < 0) return false;
        if (sym < 256) {
            if (out->len >= maxSize) return false;
            artfVecPush(out, (uint8_t)sym);
            continue;
        }
        if (sym == 256) return true;
        sym -= 257;
        if (sym >= 29) return false;
        int len = kLenBase[sym] + (int)bits(br, kLenExtra[sym]);
        int ds = huffDecode(dist, br);
        if (ds < 0 || ds >= 30) return false;
        size_t d = kDistBase[ds] + bits(br, kDistExtra[ds]);
        if (d > out->len || (size_t)len > maxSize - out->len) return false;
        size_t start = out->len - d;
        artfVecReserve(out, out->len + (size_t)len);
        uint8_t* o = out->items;
        size_t at = out->len;
        if (d >= (size_t)len) memcpy(o + at, o + start, (size_t)len); /* (no overlap) */
        else for (int k = 0; k < len; ++k) o[at + k] = o[start + k];
        out->len += (size_t)len;
        if (br->overflow) return false;
    }
}

static Huffman fixedLit, fixedDist;
static bool fixedOnce;
static void initFixed(void) {
    uint8_t l[288];
    for (int i = 0; i < 144; ++i) l[i] = 8;
    for (int i = 144; i < 256; ++i) l[i] = 9;
    for (int i = 256; i < 280; ++i) l[i] = 7;
    for (int i = 280; i < 288; ++i) l[i] = 8;
    huffBuild(&fixedLit, l, 288);
    uint8_t d[30];
    for (int i = 0; i < 30; ++i) d[i] = 5;
    huffBuild(&fixedDist, d, 30);
}

static bool inflateRaw(const uint8_t* data, size_t size, Bytes* out, size_t* consumed, size_t maxSize) {
    if (out->len > maxSize) return false;
    BitReader br = {data, size, 0, 0, 0, 0, false};
    for (;;) {
        uint32_t final = bits(&br, 1);
        uint32_t type = bits(&br, 2);
        if (type == 0) {
            alignByte(&br);
            /* whatever is left in the bit buffer is whole bytes */
            uint8_t hdr[4];
            for (int k = 0; k < 4; ++k) hdr[k] = (uint8_t)bits(&br, 8);
            uint16_t len = (uint16_t)(hdr[0] | (hdr[1] << 8));
            if (br.overflow || len > maxSize - out->len) return false;
            artfVecReserve(out, out->len + len);
            uint32_t k = 0;
            for (; k < len && br.cnt - br.pad * 8 >= 8; ++k) out->items[out->len++] = (uint8_t)bits(&br, 8); /* (the bytes buffered) */
            const size_t rest = len - k;
            if (rest > br.n - br.i) return false;
            if (rest) memcpy(out->items + out->len, br.p + br.i, rest); /* (an empty block, with nothing out yet: no items) */
            out->len += rest;
            br.i += rest;
        } else if (type == 1) {
            ONCE(fixedOnce, initFixed);
            if (!inflateBlock(&br, out, &fixedLit, &fixedDist, maxSize)) return false;
        } else if (type == 2) {
            int hlit = (int)bits(&br, 5) + 257, hdist = (int)bits(&br, 5) + 1, hclen = (int)bits(&br, 4) + 4;
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t cl[19] = {0};
            for (int i = 0; i < hclen; ++i) cl[order[i]] = (uint8_t)bits(&br, 3);
            Huffman clh;
            huffBuild(&clh, cl, 19);
            uint8_t lens[320] = {0};
            int n = 0;
            while (n < hlit + hdist) {
                int sym = huffDecode(&clh, &br);
                if (sym < 0) return false;
                if (sym < 16) lens[n++] = (uint8_t)sym;
                else if (sym == 16) {
                    if (n == 0) return false;
                    int r = 3 + (int)bits(&br, 2);
                    while (r-- && n < 320) { lens[n] = lens[n - 1]; ++n; }
                } else if (sym == 17) { int r = 3 + (int)bits(&br, 3); while (r-- && n < 320) lens[n++] = 0; }
                else { int r = 11 + (int)bits(&br, 7); while (r-- && n < 320) lens[n++] = 0; }
            }
            Huffman lit, dist;
            huffBuild(&lit, lens, hlit);
            huffBuild(&dist, lens + hlit, hdist);
            if (!inflateBlock(&br, out, &lit, &dist, maxSize)) return false;
        } else return false;
        if (br.overflow) return false;
        if (final) break;
    }
    if (consumed) *consumed = bytesRead(&br);
    return true;
}

static bool inflateRawApi(const uint8_t* data, size_t size, Bytes* out, size_t* consumed) { return inflateRaw(data, size, out, consumed, SIZE_MAX); }

static bool inflateZlibLimit(const uint8_t* data, size_t size, Bytes* out, size_t maxSize) {
    if (size < 2) return false;
    if ((data[0] & 0x0F) != 8) return false;
    return inflateRaw(data + 2, size - 2, out, NULL, maxSize);
}

static bool inflateZlib(const uint8_t* data, size_t size, Bytes* out) { return inflateZlibLimit(data, size, out, SIZE_MAX); }

/* ---- PNG ------------------------------------------------------------------------------------- */

static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static uint32_t packPixel(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return b | (g << 8) | (r << 16) | (a << 24); }

typedef struct PngState {
    const Bytes* raw;
    int channels, depth, bpp, ctype;
    const Bytes* plte;
    const Bytes* trns;
} PngState;

static bool pngUnfilter(const PngState* st, size_t* pos, uint32_t pw, uint32_t ph, Bytes* outRows, size_t* strideOut) {
    const size_t stride = ((size_t)pw * st->channels * st->depth + 7) / 8;
    *strideOut = stride;
    vecClear(outRows);
    vecResize(outRows, stride * ph);
    uint8_t* zero = (uint8_t*)artfCalloc(stride ? stride : 1, 1); /* (the row above the first) */
    const size_t bpp = (size_t)st->bpp;
    for (uint32_t y = 0; y < ph; ++y) {
        if (*pos + 1 + stride > st->raw->len) { artfFree(zero); return false; }
        uint8_t f = st->raw->items[(*pos)++];
        uint8_t* cur = &outRows->items[y * stride];
        const uint8_t* prev = y ? cur - stride : zero; /* (the rows are one after the other) */
        memcpy(cur, &st->raw->items[*pos], stride);
        *pos += stride;
        /* one loop per filter; the first bpp bytes have nothing to their left */
        switch (f) {
        case 1:
            for (size_t x = bpp; x < stride; ++x) cur[x] = (uint8_t)(cur[x] + cur[x - bpp]);
            break;
        case 2:
            for (size_t x = 0; x < stride; ++x) cur[x] = (uint8_t)(cur[x] + prev[x]);
            break;
        case 3:
            for (size_t x = 0; x < stride; ++x) cur[x] = (uint8_t)(cur[x] + (((x >= bpp ? cur[x - bpp] : 0) + prev[x]) >> 1));
            break;
        case 4:
            for (size_t x = 0; x < stride; ++x) {
                const int a = x >= bpp ? cur[x - bpp] : 0, b = prev[x], c = x >= bpp ? prev[x - bpp] : 0;
                const int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                cur[x] = (uint8_t)(cur[x] + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c));
            }
            break;
        default: break;
        }
    }
    artfFree(zero);
    return true;
}

static uint32_t pngSample(const PngState* st, const uint8_t* row, uint32_t x, int ch) {
    if (st->depth == 8) return row[x * st->channels + ch];
    if (st->depth == 16) return row[(x * st->channels + ch) * 2];
    /* sub-byte depths (grayscale or palette, one channel) */
    int bitsPos = (int)(x * st->depth);
    int v = (row[bitsPos / 8] >> (8 - st->depth - bitsPos % 8)) & ((1 << st->depth) - 1);
    return (uint32_t)v;
}

static uint32_t pngPixel(const PngState* st, const uint8_t* row, uint32_t x) {
    uint32_t r, g, b, a = 255;
    const Bytes* trns = st->trns;
    const Bytes* plte = st->plte;
    switch (st->ctype) {
    case 0: {
        uint32_t v = pngSample(st, row, x, 0);
        uint32_t vv = st->depth < 8 ? v * 255 / ((1u << st->depth) - 1) : v;
        if (trns->len >= 2) {
            uint32_t t = ((uint32_t)trns->items[0] << 8) | trns->items[1];
            uint32_t raw16 = st->depth == 16 ? ((uint32_t)row[x * 2] << 8 | row[x * 2 + 1]) : v;
            if (raw16 == t) a = 0;
        }
        r = g = b = vv;
        break;
    }
    case 2:
        r = pngSample(st, row, x, 0); g = pngSample(st, row, x, 1); b = pngSample(st, row, x, 2);
        if (trns->len >= 6 && st->depth == 8 && r == trns->items[1] && g == trns->items[3] && b == trns->items[5]) a = 0;
        break;
    case 3: {
        uint32_t idx = pngSample(st, row, x, 0);
        if (idx * 3 + 2 < plte->len) { r = plte->items[idx * 3]; g = plte->items[idx * 3 + 1]; b = plte->items[idx * 3 + 2]; }
        else r = g = b = 0;
        if (idx < trns->len) a = trns->items[idx];
        break;
    }
    case 4: r = g = b = pngSample(st, row, x, 0); a = pngSample(st, row, x, 1); break;
    default: r = pngSample(st, row, x, 0); g = pngSample(st, row, x, 1); b = pngSample(st, row, x, 2); a = pngSample(st, row, x, 3); break;
    }
    return packPixel(r, g, b, a);
}

static Picture* decodePNG(const uint8_t* d, size_t n) {
    if (n < 8 || memcmp(d, "\x89PNG\r\n\x1a\n", 8) != 0) return NULL;
    size_t i = 8;
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = 0, interlace = 0;
    Bytes idat = {0}, plte = {0}, trns = {0}, raw = {0}, rows = {0};
    Picture* img = NULL;
    bool header = false;
    while (i + 8 <= n) {
        uint32_t len = be32(d + i);
        const uint8_t* type = d + i + 4;
        const uint8_t* body = d + i + 8;
        if (len > n - i - 8 || n - i - 8 - len < 4) break; /* (the body and its CRC) */
        if (!memcmp(type, "IHDR", 4)) {
            if (header || len < 13) goto done;
            header = true;
            w = be32(body); h = be32(body + 4); depth = body[8]; ctype = body[9]; interlace = body[12];
        } else if (!memcmp(type, "PLTE", 4)) { vecClear(&plte); vecAppend(&plte, body, len); }
        else if (!memcmp(type, "tRNS", 4)) { vecClear(&trns); vecAppend(&trns, body, len); }
        else if (!memcmp(type, "IDAT", 4)) vecAppend(&idat, body, len);
        else if (!memcmp(type, "IEND", 4)) break;
        i += 12 + (size_t)len;
    }
    if (!w || !h || w > 16384 || h > 16384) goto done;
    /* the bit depths each color type allows */
    {
        const int allowed = ctype == 0 ? 0x1F : ctype == 3 ? 0x0F : (ctype == 2 || ctype == 4 || ctype == 6) ? 0x18 : 0;
        const int bit = depth == 1 ? 1 : depth == 2 ? 2 : depth == 4 ? 4 : depth == 8 ? 8 : depth == 16 ? 16 : 0;
        if (!(allowed & bit) || interlace > 1) goto done;
    }
    PngState st;
    st.channels = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : 4;
    /* the filtered rows (each with its filter byte), all passes: what the data must inflate to */
    size_t expected = 0;
    if (!interlace) expected = (((size_t)w * st.channels * depth + 7) / 8 + 1) * h;
    else {
        static const int xs[7] = {0, 4, 0, 2, 0, 1, 0}, ys[7] = {0, 0, 4, 0, 2, 0, 1};
        static const int dx[7] = {8, 8, 4, 4, 2, 2, 1}, dy[7] = {8, 8, 8, 4, 4, 2, 2};
        for (int p = 0; p < 7; ++p) {
            if (w <= (uint32_t)xs[p] || h <= (uint32_t)ys[p]) continue;
            const size_t pw = (w - xs[p] + dx[p] - 1) / dx[p], ph = (h - ys[p] + dy[p] - 1) / dy[p];
            expected += ((pw * st.channels * depth + 7) / 8 + 1) * ph;
        }
    }
    artfVecReserve(&raw, minz(expected, idat.len * 1032 + 1024)); /* (DEFLATE makes at most ~1032 bytes of one) */
    if (!inflateZlibLimit(idat.items, idat.len, &raw, expected + 65536)) goto done; /* (some slack for sloppy encoders) */
    st.raw = &raw;
    st.depth = depth;
    st.bpp = artfMaxi(1, st.channels * depth / 8);
    st.ctype = ctype;
    st.plte = &plte;
    st.trns = &trns;
    img = pictureNew((int)w, (int)h);
    size_t pos = 0, stride = 0;
    if (!interlace) {
        if (!pngUnfilter(&st, &pos, w, h, &rows, &stride)) { pictureFree(img); img = NULL; goto done; }
        for (uint32_t y = 0; y < h; ++y) {
            uint32_t* out = pictureRow(img, (int)y);
            for (uint32_t x = 0; x < w; ++x) out[x] = pngPixel(&st, &rows.items[y * stride], x);
        }
    } else {
        static const int xs[7] = {0, 4, 0, 2, 0, 1, 0}, ys[7] = {0, 0, 4, 0, 2, 0, 1};
        static const int dx[7] = {8, 8, 4, 4, 2, 2, 1}, dy[7] = {8, 8, 8, 4, 4, 2, 2};
        for (int p = 0; p < 7; ++p) {
            uint32_t pw = (w - xs[p] + dx[p] - 1) / dx[p], ph = (h - ys[p] + dy[p] - 1) / dy[p];
            if (w <= (uint32_t)xs[p] || h <= (uint32_t)ys[p] || !pw || !ph) continue;
            if (!pngUnfilter(&st, &pos, pw, ph, &rows, &stride)) { pictureFree(img); img = NULL; goto done; }
            for (uint32_t y = 0; y < ph; ++y)
                for (uint32_t x = 0; x < pw; ++x)
                    pictureRow(img, (int)(ys[p] + y * dy[p]))[xs[p] + x * dx[p]] = pngPixel(&st, &rows.items[y * stride], x);
        }
    }
done:
    artfVecFree(&idat);
    artfVecFree(&plte);
    artfVecFree(&trns);
    artfVecFree(&raw);
    artfVecFree(&rows);
    return img;
}

/* ---- JPEG (baseline and progressive, huffman) ---------------------------------------------- */

/* the largest picture decoded (as many pixels as PNG's 16384 x 16384) */
#define JPEG_MAX_PIXELS ((size_t)1 << 28)
#define JPEG_FAST_BITS 9

typedef struct HT {
    uint8_t bits[17];
    uint8_t vals[256];
    int count;
    int maxcode[18];
    int valptr[17];
    int mincode[17];
    uint16_t fast[1 << JPEG_FAST_BITS]; /* the next JPEG_FAST_BITS bits -> length << 8 | value; 0: the long way */
} HT;

typedef struct Comp {
    int id, h, v, tq, td, ta;
    int pred;
    Bytes data;
    int bw, bh;
    ARTF_VEC(int16_t) coef; /* progressive: the coefficients of every block, natural order, not dequantized */
} Comp;

typedef struct Jpeg {
    const uint8_t* d;
    size_t n, i;
    uint16_t qt[4][64];
    HT ht[2][4];
    Comp comps[3];
    int ncomp, width, height, hmax, vmax, restart;
    uint32_t bitbuf; /* the next bits, from the top */
    int bitcnt;
    bool progressive;
    int eobrun;
} Jpeg;

static const uint8_t kZigzag[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
                                    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/* The value of the first code that the top bits of v (MSB first) make, its length in *len; -1 when
 * none of max bits or fewer does. */
static int htMatch(const HT* t, uint32_t v, int max, int* len) {
    int code = 0;
    for (int l = 1; l <= max; ++l) {
        code = (code << 1) | (int)((v >> (31 - (l - 1))) & 1);
        if (t->maxcode[l] >= 0 && code <= t->maxcode[l] && code >= t->mincode[l]) { *len = l; return t->vals[t->valptr[l] + code - t->mincode[l]]; }
    }
    return -1;
}

static void buildHT(HT* t) {
    int code = 0, k = 0;
    for (int len = 1; len <= 16; ++len) {
        t->valptr[len] = k;
        t->mincode[len] = code;
        code += t->bits[len];
        k += t->bits[len];
        t->maxcode[len] = t->bits[len] ? code - 1 : -1;
        code <<= 1;
    }
    t->count = k;
    /* each pattern of the first bits: what the long way decodes from it (so the same for any table) */
    for (uint32_t p = 0; p < (1u << JPEG_FAST_BITS); ++p) {
        int len = 0;
        const int v = htMatch(t, p << (32 - JPEG_FAST_BITS), JPEG_FAST_BITS, &len);
        t->fast[p] = v < 0 ? 0 : (uint16_t)(len << 8 | v);
    }
}

/* At least 25 bits in the buffer. Stuffed zeros are dropped; a marker ends the data: it reads as
 * zeros and stays to be found, as does the end. */
static void jfill(Jpeg* j) {
    while (j->bitcnt <= 24) {
        uint32_t b = 0;
        if (j->i < j->n) {
            b = j->d[j->i];
            if (b != 0xFF) ++j->i;
            else if ((j->i + 1 < j->n ? j->d[j->i + 1] : 0) == 0) j->i += 2;
            else b = 0;
        }
        j->bitbuf |= b << (24 - j->bitcnt);
        j->bitcnt += 8;
    }
}

static void jskip(Jpeg* j, int k) {
    j->bitbuf <<= k;
    j->bitcnt -= k;
}

/* The bits buffered dropped (at a restart, and between scans). */
static void jreset(Jpeg* j) {
    j->bitbuf = 0;
    j->bitcnt = 0;
}

static int jbit(Jpeg* j) {
    if (j->bitcnt < 1) jfill(j);
    const int v = (int)(j->bitbuf >> 31);
    jskip(j, 1);
    return v;
}

static int receive(Jpeg* j, int s) {
    if (s <= 0) return 0;
    if (j->bitcnt < s) jfill(j);
    const int v = (int)(j->bitbuf >> (32 - s));
    jskip(j, s);
    return v;
}
static int extend(int v, int s) { return s == 0 ? 0 : (v < (1 << (s - 1)) ? v - (1 << s) + 1 : v); }

static int decodeHuff(Jpeg* j, const HT* t) {
    if (j->bitcnt < 16) jfill(j);
    const uint16_t e = t->fast[j->bitbuf >> (32 - JPEG_FAST_BITS)];
    if (e) {
        jskip(j, e >> 8);
        return e & 255;
    }
    int len = 0;
    const int v = htMatch(t, j->bitbuf, 16, &len);
    jskip(j, v < 0 ? 16 : len);
    return v < 0 ? 0 : v;
}

static float idctC[8][8];
static bool idctOnce;
static void initIdct(void) {
    for (int u = 0; u < 8; ++u)
        for (int x = 0; x < 8; ++x) idctC[u][x] = (u == 0 ? sqrtf(0.125f) : 0.5f) * cosf((2 * x + 1) * u * 3.14159265f / 16);
}

/* v rounded half away from zero (what lroundf gives), clamped to 0..255; without a library call */
static inline int byteOf(float v) {
    const float c = v > 0 ? (v < 255 ? v : 255) : 0; /* (selects, not branches: the loops vectorize) */
    const int i = (int)c;
    return i + (c - (float)i >= 0.5f);
}

/* The rows of eight run side by side: each sum is still taken term by term in the same order, so the
 * values are those of one sum at a time. */
static void idct(const int* in, uint8_t* out, int stride) {
    ONCE(idctOnce, initIdct);
    float tmp[64];
    /* most rows of coefficients are all zero, and so are their sums: left out (the same values) */
    int rows[8], nrows = 0;
    for (int y = 0; y < 8; ++y) {
        const int* r = in + y * 8;
        if (!(r[0] | r[1] | r[2] | r[3] | r[4] | r[5] | r[6] | r[7])) continue;
        rows[nrows++] = y;
        /* and so are the zero coefficients of a row (s + 0 * c is s, the sum of the same terms) */
        float s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        for (int u = 0; u < 8; ++u) {
            if (!r[u]) continue;
            const float cu = (float)r[u];
            for (int x = 0; x < 8; ++x) s[x] += idctC[u][x] * cu;
        }
        memcpy(tmp + y * 8, s, sizeof s);
    }
    if (nrows == 0) { /* (a flat block) */
        for (int y = 0; y < 8; ++y) memset(out + y * stride, 128, 8);
        return;
    }
    for (int y = 0; y < 8; ++y) {
        float s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        for (int k = 0; k < nrows; ++k) {
            const float c = idctC[rows[k]][y];
            const float* t = tmp + rows[k] * 8;
            for (int x = 0; x < 8; ++x) s[x] += c * t[x];
        }
        uint8_t* o = out + (size_t)y * stride;
        for (int x = 0; x < 8; ++x) o[x] = (uint8_t)byteOf(s[x] + 128);
    }
}

static void decodeBlock(Jpeg* j, Comp* c, int* blk) {
    memset(blk, 0, 64 * sizeof(int));
    const HT* dc = &j->ht[0][c->td];
    const HT* ac = &j->ht[1][c->ta];
    int t = decodeHuff(j, dc) & 15; /* (a size past 11 is broken data; kept to what the arithmetic takes) */
    int diff = t ? extend(receive(j, t), t) : 0;
    c->pred += diff;
    blk[0] = c->pred * j->qt[c->tq][0];
    for (int k = 1; k < 64;) {
        int rs = decodeHuff(j, ac);
        int r = rs >> 4, s = rs & 15;
        if (s == 0) { if (r == 15) { k += 16; continue; } break; }
        k += r;
        if (k > 63) break;
        blk[kZigzag[k]] = extend(receive(j, s), s) * j->qt[c->tq][k];
        ++k;
    }
}

/* The components (upsampled) as RGB. */
/* The chroma terms of YCbCr to RGB for each sample: 1.402 (Cr - 128) and so on, the same floats. */
static struct { float crR[256], cbG[256], crG[256], cbB[256]; } kYcc;
static bool yccOnce;
static void initYcc(void) {
    for (int v = 0; v < 256; ++v) {
        const float c = (float)v - 128;
        kYcc.crR[v] = 1.402f * c;
        kYcc.cbG[v] = 0.344136f * c;
        kYcc.crG[v] = 0.714136f * c;
        kYcc.cbB[v] = 1.772f * c;
    }
}

static Picture* jpegToImage(Jpeg* j) {
    ONCE(yccOnce, initYcc);
    Picture* img = pictureNew(j->width, j->height);
    /* each component's sample for a column (upsampled), worked out once */
    int* cols[3] = {NULL, NULL, NULL};
    for (int ci = 0; ci < j->ncomp && ci < 3; ++ci) {
        cols[ci] = (int*)artfMalloc((size_t)artfMaxi(1, j->width) * sizeof(int));
        for (int x = 0; x < j->width; ++x) cols[ci][x] = x * j->comps[ci].h / j->hmax;
    }
    for (int y = 0; y < j->height; ++y) {
        uint32_t* out = pictureRow(img, y);
        const uint8_t* rows[3] = {NULL, NULL, NULL};
        for (int ci = 0; ci < j->ncomp && ci < 3; ++ci) rows[ci] = j->comps[ci].data.items + (size_t)(y * j->comps[ci].v / j->vmax) * j->comps[ci].bw;
        if (j->ncomp == 1) {
            for (int x = 0; x < j->width; ++x) {
                const uint32_t g = rows[0][cols[0][x]];
                out[x] = g | (g << 8) | (g << 16) | 0xFF000000u;
            }
            continue;
        }
        for (int x = 0; x < j->width; ++x) {
            const float Y = (float)rows[0][cols[0][x]];
            const int cb = rows[1][cols[1][x]], cr = rows[2][cols[2][x]];
            const int r = byteOf(Y + kYcc.crR[cr]);
            const int g = byteOf(Y - kYcc.cbG[cb] - kYcc.crG[cr]);
            const int b = byteOf(Y + kYcc.cbB[cb]);
            out[x] = (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16) | 0xFF000000u;
        }
    }
    for (int ci = 0; ci < 3; ++ci) artfFree(cols[ci]);
    return img;
}

static Picture* jpegScan(Jpeg* j) {
    if (!j->width || !j->height) return NULL;
    int mcuw = 8 * j->hmax, mcuh = 8 * j->vmax;
    int mx = (j->width + mcuw - 1) / mcuw, my = (j->height + mcuh - 1) / mcuh;
    for (int c = 0; c < j->ncomp; ++c) {
        Comp* cp = &j->comps[c];
        cp->bw = mx * cp->h * 8;
        cp->bh = my * cp->v * 8;
        vecClear(&cp->data);
        vecResize(&cp->data, (size_t)cp->bw * cp->bh);
        cp->pred = 0;
    }
    int blk[64];
    int mcus = 0;
    for (int y = 0; y < my; ++y)
        for (int x = 0; x < mx; ++x) {
            if (j->restart && mcus && mcus % j->restart == 0) {
                jreset(j);
                /* skip to the restart marker */
                while (j->i + 1 < j->n && !(j->d[j->i] == 0xFF && j->d[j->i + 1] >= 0xD0 && j->d[j->i + 1] <= 0xD7)) ++j->i;
                j->i += 2;
                for (int c = 0; c < j->ncomp; ++c) j->comps[c].pred = 0;
            }
            for (int c = 0; c < j->ncomp; ++c) {
                Comp* cp = &j->comps[c];
                for (int v = 0; v < cp->v; ++v)
                    for (int h = 0; h < cp->h; ++h) {
                        decodeBlock(j, cp, blk);
                        int bx = (x * cp->h + h) * 8, by = (y * cp->v + v) * 8;
                        idct(blk, &cp->data.items[(size_t)by * cp->bw + bx], cp->bw);
                    }
            }
            ++mcus;
        }
    return jpegToImage(j);
}

/* ---- progressive: the coefficients come in several scans ---- */

static void setupBlocks(Jpeg* j) {
    int mcuw = 8 * j->hmax, mcuh = 8 * j->vmax;
    int mx = (j->width + mcuw - 1) / mcuw, my = (j->height + mcuh - 1) / mcuh;
    for (int c = 0; c < j->ncomp; ++c) {
        Comp* cp = &j->comps[c];
        cp->bw = mx * cp->h * 8;
        cp->bh = my * cp->v * 8;
        vecClear(&cp->coef);
        vecResize(&cp->coef, (size_t)cp->bw * cp->bh);
    }
}

static int16_t* blockAt(Comp* c, int bx, int by) { return &c->coef.items[((size_t)by * (c->bw / 8) + bx) * 64]; }

static void decodeDC(Jpeg* j, Comp* c, int16_t* blk, int ah, int al) {
    if (ah == 0) {
        const int t = decodeHuff(j, &j->ht[0][c->td]) & 15;
        c->pred += t ? extend(receive(j, t), t) : 0;
        blk[0] = (int16_t)(c->pred * (1 << al));
    } else if (jbit(j)) blk[0] = (int16_t)(blk[0] | (1 << al));
}

static void decodeACFirst(Jpeg* j, Comp* c, int16_t* blk, int ss, int se, int al) {
    if (j->eobrun > 0) { --j->eobrun; return; }
    for (int k = ss; k <= se; ++k) {
        const int rs = decodeHuff(j, &j->ht[1][c->ta]);
        const int r = rs >> 4, sz = rs & 15;
        if (sz == 0) {
            if (r < 15) { j->eobrun = (1 << r) - 1; if (r) j->eobrun += receive(j, r); break; }
            k += 15; /* (sixteen zeros) */
            continue;
        }
        k += r;
        if (k > 63) break;
        blk[kZigzag[k]] = (int16_t)(extend(receive(j, sz), sz) * (1 << al));
    }
}

static void refine(Jpeg* j, int16_t* coef, int p1, int m1) {
    if (jbit(j) && (*coef & p1) == 0) *coef = (int16_t)(*coef >= 0 ? *coef + p1 : *coef + m1);
}

static void decodeACRefine(Jpeg* j, Comp* c, int16_t* blk, int ss, int se, int al) {
    const int p1 = 1 << al, m1 = -1 * (1 << al);
    int k = ss;
    if (j->eobrun <= 0) {
        for (; k <= se; ++k) {
            const int rs = decodeHuff(j, &j->ht[1][c->ta]);
            int r = rs >> 4, sz = rs & 15, value = 0;
            if (sz) value = jbit(j) ? p1 : m1;
            else if (r != 15) {
                j->eobrun = 1 << r;
                if (r) j->eobrun += receive(j, r);
                break;
            }
            /* skip r zero coefficients (refining the nonzero ones passed), then place the new one */
            while (k <= se) {
                int16_t* coef = &blk[kZigzag[k]];
                if (*coef != 0) refine(j, coef, p1, m1);
                else {
                    if (r == 0) break;
                    --r;
                }
                ++k;
            }
            if (value && k <= se) blk[kZigzag[k]] = (int16_t)value;
        }
    }
    if (j->eobrun > 0) {
        for (; k <= se; ++k) {
            int16_t* coef = &blk[kZigzag[k]];
            if (*coef != 0) refine(j, coef, p1, m1);
        }
        --j->eobrun;
    }
}

static void restartIfDue(Jpeg* j, int units, const int* in, int nin) {
    if (!j->restart || !units || units % j->restart) return;
    jreset(j);
    while (j->i + 1 < j->n && !(j->d[j->i] == 0xFF && j->d[j->i + 1] >= 0xD0 && j->d[j->i + 1] <= 0xD7)) ++j->i;
    j->i += 2;
    j->eobrun = 0;
    for (int k = 0; k < nin; ++k) j->comps[in[k]].pred = 0;
}

static bool progressiveScan(Jpeg* j, const int* in, int nin, int ss, int se, int ah, int al) {
    if (!j->width || !j->height || nin == 0 || ss > se || se > 63) return false;
    if (j->comps[0].coef.len == 0) setupBlocks(j);
    jreset(j);
    j->eobrun = 0;
    for (int k = 0; k < nin; ++k) j->comps[in[k]].pred = 0;
    int units = 0;
    if (nin == 1) {
        /* one component: its blocks in order, as many as its own size needs */
        Comp* c = &j->comps[in[0]];
        const int cw = (j->width * c->h + j->hmax - 1) / j->hmax, ch = (j->height * c->v + j->vmax - 1) / j->vmax;
        const int bxN = (cw + 7) / 8, byN = (ch + 7) / 8;
        for (int by = 0; by < byN; ++by)
            for (int bx = 0; bx < bxN; ++bx) {
                restartIfDue(j, units, in, nin);
                int16_t* blk = blockAt(c, bx, by);
                if (ss == 0) decodeDC(j, c, blk, ah, al);
                else if (ah == 0) decodeACFirst(j, c, blk, ss, se, al);
                else decodeACRefine(j, c, blk, ss, se, al);
                ++units;
            }
        return true;
    }
    if (ss != 0) return false; /* (interleaved scans carry DC only) */
    const int mx = j->comps[0].bw / (8 * j->comps[0].h), my = j->comps[0].bh / (8 * j->comps[0].v);
    for (int y = 0; y < my; ++y)
        for (int x = 0; x < mx; ++x) {
            restartIfDue(j, units, in, nin);
            for (int k = 0; k < nin; ++k) {
                Comp* c = &j->comps[in[k]];
                for (int v = 0; v < c->v; ++v)
                    for (int h = 0; h < c->h; ++h) decodeDC(j, c, blockAt(c, x * c->h + h, y * c->v + v), ah, al);
            }
            ++units;
        }
    return true;
}

static Picture* finishProgressive(Jpeg* j) {
    if (!j->width || !j->height || j->comps[0].coef.len == 0) return NULL;
    int blk[64];
    for (int ci = 0; ci < j->ncomp; ++ci) {
        Comp* c = &j->comps[ci];
        vecClear(&c->data);
        vecResize(&c->data, (size_t)c->bw * c->bh);
        for (int by = 0; by < c->bh / 8; ++by)
            for (int bx = 0; bx < c->bw / 8; ++bx) {
                const int16_t* q = blockAt(c, bx, by);
                for (int k = 0; k < 64; ++k) blk[kZigzag[k]] = q[kZigzag[k]] * j->qt[c->tq][k];
                idct(blk, &c->data.items[(size_t)by * 8 * c->bw + bx * 8], c->bw);
            }
    }
    return jpegToImage(j);
}

static Picture* jpegRun(Jpeg* j) {
    const uint8_t* d = j->d;
    const size_t n = j->n;
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return NULL;
    j->i = 2;
    while (j->i + 4 <= n) {
        if (d[j->i] != 0xFF) { ++j->i; continue; }
        uint8_t m = d[j->i + 1];
        j->i += 2;
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01 || m == 0xFF) continue;
        if (m == 0xD9) break;
        uint16_t len = (uint16_t)((d[j->i] << 8) | d[j->i + 1]);
        const uint8_t* s = d + j->i + 2;
        size_t segEnd = j->i + len;
        if (len < 2 || segEnd > n) return NULL;
        const size_t slen = (size_t)len - 2; /* (the bytes of s) */
        if (m == 0xDB) {
            size_t k = 0;
            while (k < slen) {
                int pq = s[k] >> 4, tq = s[k] & 3;
                if (k + 1 + (pq ? 128 : 64) > slen) break;
                ++k;
                for (int q = 0; q < 64; ++q) { j->qt[tq][q] = pq ? (uint16_t)((s[k] << 8) | s[k + 1]) : s[k]; k += pq ? 2 : 1; }
            }
        } else if (m == 0xC4) {
            size_t k = 0;
            while (k + 17 <= slen) {
                int tc = s[k] >> 4, th = s[k] & 3;
                HT* t = &j->ht[tc & 1][th];
                int total = 0;
                for (int q = 1; q <= 16; ++q) total += s[k + q];
                if (total > 256 || k + 17 + (size_t)total > slen) return NULL;
                t->bits[0] = 0;
                for (int q = 1; q <= 16; ++q) t->bits[q] = s[k + q];
                k += 17;
                for (int q = 0; q < total; ++q) t->vals[q] = s[k + q];
                k += total;
                buildHT(t);
            }
        } else if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            if (j->width || slen < 6) return NULL; /* (one frame) */
            j->progressive = m == 0xC2;
            j->height = (s[1] << 8) | s[2];
            j->width = (s[3] << 8) | s[4];
            j->ncomp = s[5];
            if (j->ncomp != 1 && j->ncomp != 3) return NULL;
            if (slen < 6 + 3 * (size_t)j->ncomp) return NULL;
            if ((size_t)j->width * j->height > JPEG_MAX_PIXELS) return NULL;
            for (int c = 0; c < j->ncomp; ++c) {
                j->comps[c].id = s[6 + 3 * c];
                j->comps[c].h = s[7 + 3 * c] >> 4;
                j->comps[c].v = s[7 + 3 * c] & 15;
                j->comps[c].tq = s[8 + 3 * c] & 3;
                if (j->comps[c].h < 1 || j->comps[c].h > 4 || j->comps[c].v < 1 || j->comps[c].v > 4) return NULL;
                j->hmax = artfMaxi(j->hmax, j->comps[c].h);
                j->vmax = artfMaxi(j->vmax, j->comps[c].v);
            }
        } else if (m == 0xDD) {
            if (slen >= 2) j->restart = (s[0] << 8) | s[1];
        } else if (m == 0xDA) {
            if (slen < 1) return NULL;
            int ns = s[0];
            if (slen < 4 + 2 * (size_t)ns) return NULL;
            int in[4];
            int nin = 0;
            for (int k = 0; k < ns; ++k) {
                int id = s[1 + 2 * k];
                for (int c = 0; c < j->ncomp; ++c)
                    if (j->comps[c].id == id) {
                        j->comps[c].td = (s[2 + 2 * k] >> 4) & 3;
                        j->comps[c].ta = s[2 + 2 * k] & 3;
                        if (nin < 4) in[nin++] = c;
                    }
            }
            j->i = segEnd;
            if (!j->progressive) return jpegScan(j);
            const int ss = s[1 + 2 * ns], se = s[2 + 2 * ns], ah = s[3 + 2 * ns] >> 4, al = s[3 + 2 * ns] & 15;
            if (!progressiveScan(j, in, nin, ss, se, ah, al)) return NULL;
            /* on to the next marker */
            while (j->i + 1 < n && !(d[j->i] == 0xFF && d[j->i + 1] != 0 && !(d[j->i + 1] >= 0xD0 && d[j->i + 1] <= 0xD7))) ++j->i;
            continue;
        }
        j->i = segEnd;
    }
    return j->progressive ? finishProgressive(j) : NULL; /* (a progressive file may end without EOI) */
}

static Picture* decodeJPEG(const uint8_t* d, size_t n) {
    Jpeg* j = ARTF_NEW(Jpeg);
    j->d = d;
    j->n = n;
    j->hmax = j->vmax = 1;
    Picture* img = jpegRun(j);
    for (int c = 0; c < 3; ++c) {
        artfVecFree(&j->comps[c].data);
        artfVecFree(&j->comps[c].coef);
    }
    artfFree(j);
    return img;
}

uint32_t* artfImageDecode(const void* data, size_t size, int* width, int* height) {
    Picture* p = decodePNG(data, size);
    if (!p) p = decodeJPEG(data, size);
    if (!p) return NULL;
    uint32_t* pixels = p->pixels;
    *width = p->width;
    *height = p->height;
    artfFree(p);
    return pixels;
}
