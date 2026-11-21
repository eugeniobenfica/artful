/* artful — Zstandard, read (RFC 8878): what KTX2 files hold UASTC's blocks in. Frames of raw, repeated and compressed
 * blocks: literals by Huffman codes, sequences by finite state entropy (FSE). Every read is checked against what the
 * file holds, and what is written against what the caller made room for: a file that says more than it holds, or
 * makes more than it should, is refused. No dictionaries; checksums are not checked. */
#include "scene/sceneInternal.h"

/* ---- bits, read backward: from the last byte's highest set bit (the end's mark) toward the first ---- */

typedef struct Backward {
    const uint8_t* start;
    int64_t left; /* bits not yet read: below 0, more were read than there are (they read as 0) */
} Backward;

static bool backwardOpen(Backward* b, const uint8_t* p, size_t size) {
    if (!size || !p[size - 1]) return false;
    int high = 7;
    while (!(p[size - 1] >> high & 1)) --high;
    b->start = p;
    b->left = (int64_t)(size - 1) * 8 + high;
    return true;
}

/* The next n bits (n at most 32), the first read the highest. */
static inline uint32_t backwardRead(Backward* b, int n) {
    if (!n) return 0;
    b->left -= n;
    const int64_t from = b->left; /* the lowest bit of what is read */
    uint64_t value = 0;
    const int64_t first = from >> 3, last = (from + n - 1) >> 3;
    for (int64_t i = last; i >= first; --i) value = value << 8 | (i >= 0 ? b->start[i] : 0u);
    const int shift = (int)(from - first * 8);
    return (uint32_t)(value >> shift) & (uint32_t)((1ull << n) - 1);
}

/* ---- FSE ---- */

enum { FSE_MOST_LOG = 9, FSE_MOST_SYMBOLS = 256 };

typedef struct FseEntry {
    uint16_t symbol, baseline;
    uint8_t bits;
} FseEntry;

typedef struct Fse {
    FseEntry table[1 << FSE_MOST_LOG];
    int log;
} Fse;

/* The table of these probabilities (-1: less than one), of 2^log states. */
static bool fseBuild(Fse* f, const int16_t* probability, int symbols, int log) {
    const uint32_t size = 1u << log, mask = size - 1;
    uint16_t next[FSE_MOST_SYMBOLS];
    uint32_t high = size - 1;
    for (int s = 0; s < symbols; ++s)
        if (probability[s] == -1) {
            f->table[high--].symbol = (uint16_t)s;
            next[s] = 1;
        } else next[s] = (uint16_t)probability[s];
    const uint32_t step = (size >> 1) + (size >> 3) + 3;
    uint32_t position = 0;
    for (int s = 0; s < symbols; ++s)
        for (int i = 0; i < probability[s]; ++i) {
            f->table[position].symbol = (uint16_t)s;
            do position = (position + step) & mask;
            while (position > high);
        }
    if (position) return false; /* (the probabilities did not fill the table) */
    for (uint32_t u = 0; u < size; ++u) {
        const uint16_t s = f->table[u].symbol, state = next[s]++;
        int highBit = 31;
        while (!(state >> highBit & 1u)) --highBit;
        f->table[u].bits = (uint8_t)(log - highBit);
        f->table[u].baseline = (uint16_t)(((uint32_t)state << f->table[u].bits) - size);
    }
    f->log = log;
    return true;
}

static uint32_t peekForward(const uint8_t* p, size_t size, uint64_t at, int n);

/* A table's description, read forward: its size, or 0 when it is not one. */
static size_t fseRead(Fse* f, const uint8_t* p, size_t size, int mostLog, int mostSymbols) {
    if (!size) return 0;
    uint64_t at = 0;
    const uint64_t bits = (uint64_t)size * 8;
#define PEEK(n) peekForward(p, size, at, n)
    int log = (int)(PEEK(4)) + 5;
    at += 4;
    if (log > mostLog) return 0;
    int16_t probability[FSE_MOST_SYMBOLS];
    int remaining = (1 << log) + 1, threshold = 1 << log, width = log + 1, symbol = 0;
    while (remaining > 1 && symbol < mostSymbols) {
        const int most = 2 * threshold - 1 - remaining;
        int value;
        const uint32_t low = PEEK(width - 1) & (uint32_t)(threshold - 1);
        if ((int)low < most) {
            value = (int)low;
            at += (uint64_t)(width - 1);
        } else {
            value = (int)(PEEK(width) & (uint32_t)(2 * threshold - 1));
            if (value >= threshold) value -= most;
            at += (uint64_t)width;
        }
        const int count = value - 1;
        remaining -= count < 0 ? -count : count;
        probability[symbol++] = (int16_t)count;
        if (!count) {
            for (;;) {
                const int repeat = (int)PEEK(2);
                at += 2;
                for (int i = 0; i < repeat && symbol < mostSymbols; ++i) probability[symbol++] = 0;
                if (repeat != 3) break;
            }
        }
        while (remaining < threshold) {
            --width;
            threshold >>= 1;
        }
        if (at > bits) return 0;
    }
#undef PEEK
    if (remaining != 1 || at > bits) return 0;
    if (!fseBuild(f, probability, symbol, log)) return 0;
    return (size_t)((at + 7) / 8);
}

/* (n bits forward from bit `at`, little-endian: past the end, 0) */
static uint32_t peekForward(const uint8_t* p, size_t size, uint64_t at, int n) {
    uint64_t value = 0;
    for (int i = 0; i < 5; ++i) {
        const uint64_t byte = (at >> 3) + (uint64_t)i;
        value |= (uint64_t)(byte < size ? p[byte] : 0u) << (8 * i);
    }
    return (uint32_t)(value >> (at & 7)) & (uint32_t)((1ull << n) - 1);
}

/* ---- Huffman ---- */

enum { HUF_MOST_BITS = 11 };

typedef struct Huffman {
    uint8_t symbol[1 << HUF_MOST_BITS], bits[1 << HUF_MOST_BITS];
    int most; /* 0: none yet */
} Huffman;

/* A tree's description: its size, or 0. */
static size_t huffmanRead(Huffman* h, const uint8_t* p, size_t size) {
    if (!size) return 0;
    uint8_t weight[256];
    int count = 0;
    size_t used;
    const int head = p[0];
    if (head >= 128) {
        count = head - 127;
        used = 1 + (size_t)(count + 1) / 2;
        if (used > size) return 0;
        for (int i = 0; i < count; ++i) weight[i] = (uint8_t)(i & 1 ? p[1 + i / 2] & 15 : p[1 + i / 2] >> 4);
    } else {
        used = 1 + (size_t)head;
        if (used > size || !head) return 0;
        Fse f;
        const size_t table = fseRead(&f, p + 1, (size_t)head, 6, 256);
        if (!table) return 0;
        Backward b;
        if (!backwardOpen(&b, p + 1 + table, (size_t)head - table)) return 0;
        uint32_t one = backwardRead(&b, f.log), two = backwardRead(&b, f.log);
        /* two states in turn, until the bits are gone: then the other's symbol is the last */
        for (;;) {
            if (count > 253) return 0;
            const FseEntry* e = &f.table[one];
            weight[count++] = (uint8_t)e->symbol;
            one = e->baseline + backwardRead(&b, e->bits);
            if (b.left < 0) {
                weight[count++] = (uint8_t)f.table[two].symbol;
                break;
            }
            e = &f.table[two];
            weight[count++] = (uint8_t)e->symbol;
            two = e->baseline + backwardRead(&b, e->bits);
            if (b.left < 0) {
                weight[count++] = (uint8_t)f.table[one].symbol;
                break;
            }
        }
    }
    /* the last weight is what makes the sum a power of 2 */
    uint32_t sum = 0;
    for (int i = 0; i < count; ++i) {
        if (weight[i] > HUF_MOST_BITS) return 0;
        if (weight[i]) sum += 1u << (weight[i] - 1);
    }
    if (!sum || count >= 256) return 0;
    int most = 0;
    while ((1u << most) <= sum) ++most;
    const uint32_t rest = (1u << most) - sum;
    if (rest & (rest - 1)) return 0;
    int lastWeight = 0;
    while ((1u << lastWeight) < rest) ++lastWeight;
    weight[count++] = (uint8_t)(lastWeight + 1);
    if (most > HUF_MOST_BITS) return 0;
    /* codes in order of weight, the least first, each symbol's range 2^(weight - 1) long */
    uint32_t position = 0;
    for (int w = 1; w <= most; ++w)
        for (int s = 0; s < count; ++s)
            if (weight[s] == w) {
                const uint32_t length = 1u << (w - 1);
                if (position + length > (1u << most)) return 0;
                for (uint32_t i = 0; i < length; ++i) {
                    h->symbol[position + i] = (uint8_t)s;
                    h->bits[position + i] = (uint8_t)(most + 1 - w);
                }
                position += length;
            }
    h->most = most;
    return used;
}

/* n literals of one stream. */
static bool huffmanStream(const Huffman* h, const uint8_t* p, size_t size, uint8_t* out, size_t n) {
    Backward b;
    if (!backwardOpen(&b, p, size)) return false;
    for (size_t i = 0; i < n; ++i) {
        /* (peek the longest code; read only its own bits) */
        const int64_t before = b.left;
        const uint32_t index = backwardRead(&b, h->most);
        const int bits = h->bits[index];
        b.left = before - bits;
        out[i] = h->symbol[index];
    }
    return b.left == 0;
}

/* ---- a frame ---- */

typedef struct Frame {
    Huffman huffman;
    Fse tables[3]; /* literal lengths, offsets, match lengths */
    bool haveTable[3];
    uint32_t repeat[3];
} Frame;

static const uint32_t literalBase[36] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
static const uint8_t literalExtra[36] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const uint32_t matchBase[53] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34,
                                       35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
static const uint8_t matchExtra[53] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const int16_t literalDefault[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
static const int16_t matchDefault[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
static const int16_t offsetDefault[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

/* A table for the sequences, by its mode: its size in the block, or -1. */
static long sequenceTable(Frame* fr, int which, int mode, const uint8_t* p, size_t size) {
    static const int symbols[3] = {36, 32, 53}, mostLog[3] = {9, 8, 9};
    Fse* f = &fr->tables[which];
    switch (mode) {
    case 0: {
        const int16_t* d = which == 0 ? literalDefault : which == 1 ? offsetDefault : matchDefault;
        if (!fseBuild(f, d, which == 0 ? 36 : which == 1 ? 29 : 53, which == 1 ? 5 : 6)) return -1;
        fr->haveTable[which] = true;
        return 0;
    }
    case 1:
        if (!size || p[0] >= symbols[which]) return -1;
        f->table[0].symbol = p[0];
        f->table[0].bits = 0;
        f->table[0].baseline = 0;
        f->log = 0;
        fr->haveTable[which] = true;
        return 1;
    case 2: {
        const size_t used = fseRead(f, p, size, mostLog[which], symbols[which]);
        if (!used) return -1;
        fr->haveTable[which] = true;
        return (long)used;
    }
    default:
        return fr->haveTable[which] ? 0 : -1;
    }
}

/* A compressed block, onto out (where `done` bytes of the frame are already). */
static bool compressedBlock(Frame* fr, const uint8_t* p, size_t size, uint8_t* out, size_t* done, size_t room, uint8_t* literals) {
    if (!size) return false;
    /* literals */
    const int kind = p[0] & 3, format = p[0] >> 2 & 3;
    size_t regenerated, compressed = 0, header;
    int streams = 1;
    if (kind < 2) {
        if (format == 0 || format == 2) { header = 1; regenerated = p[0] >> 3; }
        else if (format == 1) { header = 2; if (size < 2) return false; regenerated = (p[0] >> 4) + ((size_t)p[1] << 4); }
        else { header = 3; if (size < 3) return false; regenerated = (p[0] >> 4) + ((size_t)p[1] << 4) + ((size_t)p[2] << 12); }
    } else {
        header = format < 2 ? 3 : format == 2 ? 4 : 5;
        streams = format == 0 ? 1 : 4;
        if (size < header) return false;
        uint64_t bitsAll = 0;
        for (size_t i = 0; i < header; ++i) bitsAll |= (uint64_t)p[i] << (8 * i);
        const int width = format < 2 ? 10 : format == 2 ? 14 : 18;
        regenerated = (size_t)(bitsAll >> 4 & ((1u << width) - 1));
        compressed = (size_t)(bitsAll >> (4 + width) & ((1u << width) - 1));
    }
    if (regenerated > 131072) return false;
    const uint8_t* q = p + header;
    size_t left = size - header;
    if (kind == 0) {
        if (regenerated > left) return false;
        memcpy(literals, q, regenerated);
        q += regenerated;
        left -= regenerated;
    } else if (kind == 1) {
        if (!left) return false;
        memset(literals, q[0], regenerated);
        q += 1;
        left -= 1;
    } else {
        if (compressed > left) return false;
        const uint8_t* stream = q;
        size_t streamBytes = compressed;
        if (kind == 2) {
            const size_t tree = huffmanRead(&fr->huffman, q, compressed);
            if (!tree) return false;
            stream += tree;
            streamBytes -= tree;
        } else if (!fr->huffman.most) return false;
        if (streams == 1) {
            if (!huffmanStream(&fr->huffman, stream, streamBytes, literals, regenerated)) return false;
        } else {
            if (streamBytes < 6) return false;
            const size_t s1 = (size_t)stream[0] | (size_t)stream[1] << 8, s2 = (size_t)stream[2] | (size_t)stream[3] << 8, s3 = (size_t)stream[4] | (size_t)stream[5] << 8;
            if (6 + s1 + s2 + s3 > streamBytes) return false;
            const size_t s4 = streamBytes - 6 - s1 - s2 - s3, each = (regenerated + 3) / 4;
            if (each * 3 > regenerated) return false;
            const uint8_t* at = stream + 6;
            if (!huffmanStream(&fr->huffman, at, s1, literals, each) || !huffmanStream(&fr->huffman, at + s1, s2, literals + each, each) ||
                !huffmanStream(&fr->huffman, at + s1 + s2, s3, literals + each * 2, each) || !huffmanStream(&fr->huffman, at + s1 + s2 + s3, s4, literals + each * 3, regenerated - each * 3))
                return false;
        }
        q += compressed;
        left -= compressed;
    }
    /* sequences */
    if (!left) return false;
    size_t count;
    if (q[0] < 128) { count = q[0]; q += 1; left -= 1; }
    else if (q[0] < 255) { if (left < 2) return false; count = ((size_t)(q[0] - 128) << 8) + q[1]; q += 2; left -= 2; }
    else { if (left < 3) return false; count = q[1] + ((size_t)q[2] << 8) + 0x7F00; q += 3; left -= 3; }
    size_t literal = 0;
    if (count) {
        if (!left) return false;
        const int modes = q[0];
        q += 1;
        left -= 1;
        static const int order[3] = {0, 1, 2}; /* literal lengths, offsets, match lengths: as the modes byte has them */
        const int shifts[3] = {6, 4, 2};
        for (int k = 0; k < 3; ++k) {
            const long used = sequenceTable(fr, order[k], modes >> shifts[k] & 3, q, left);
            if (used < 0) return false;
            q += used;
            left -= (size_t)used;
        }
        Backward b;
        if (!backwardOpen(&b, q, left)) return false;
        const Fse *ll = &fr->tables[0], *of = &fr->tables[1], *ml = &fr->tables[2];
        uint32_t sLiteral = backwardRead(&b, ll->log), sOffset = backwardRead(&b, of->log), sMatch = backwardRead(&b, ml->log);
        for (size_t n = 0; n < count; ++n) {
            const int codeLiteral = ll->table[sLiteral].symbol, codeOffset = of->table[sOffset].symbol, codeMatch = ml->table[sMatch].symbol;
            if (codeLiteral > 35 || codeMatch > 52 || codeOffset > 31) return false;
            const uint32_t offsetValue = (1u << codeOffset) + backwardRead(&b, codeOffset);
            const uint32_t matchLength = matchBase[codeMatch] + backwardRead(&b, matchExtra[codeMatch]);
            const uint32_t literalLength = literalBase[codeLiteral] + backwardRead(&b, literalExtra[codeLiteral]);
            uint32_t offset;
            if (offsetValue > 3) {
                offset = offsetValue - 3;
                fr->repeat[2] = fr->repeat[1];
                fr->repeat[1] = fr->repeat[0];
                fr->repeat[0] = offset;
            } else {
                const uint32_t index = offsetValue - 1 + (literalLength == 0 ? 1u : 0u);
                if (index == 0) offset = fr->repeat[0];
                else {
                    offset = index == 3 ? fr->repeat[0] - 1 : fr->repeat[index];
                    if (index != 1) fr->repeat[2] = fr->repeat[1];
                    fr->repeat[1] = fr->repeat[0];
                    fr->repeat[0] = offset;
                }
            }
            if (n + 1 < count) {
                const FseEntry *a = &ll->table[sLiteral], *m = &ml->table[sMatch], *o = &of->table[sOffset];
                sLiteral = a->baseline + backwardRead(&b, a->bits);
                sMatch = m->baseline + backwardRead(&b, m->bits);
                sOffset = o->baseline + backwardRead(&b, o->bits);
            }
            /* what it says: so many literals, then so many bytes from so far back */
            if (literalLength > regenerated - literal || literalLength > room - *done) return false;
            memcpy(out + *done, literals + literal, literalLength);
            literal += literalLength;
            *done += literalLength;
            if (!offset || offset > *done || matchLength > room - *done) return false;
            const uint8_t* from = out + *done - offset;
            uint8_t* to = out + *done;
            for (uint32_t i = 0; i < matchLength; ++i) to[i] = from[i];
            *done += matchLength;
        }
        if (b.left != 0) return false;
    }
    if (regenerated - literal > room - *done) return false;
    memcpy(out + *done, literals + literal, regenerated - literal);
    *done += regenerated - literal;
    return true;
}

bool artfZstdDecode(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize) {
    size_t at = 0, done = 0;
    uint8_t* literals = artfMalloc(131072);
    Frame* fr = artfMalloc(sizeof *fr);
    bool ok = true;
    while (ok && at < srcSize) {
        if (srcSize - at < 4) { ok = false; break; }
        const uint32_t magic = (uint32_t)src[at] | (uint32_t)src[at + 1] << 8 | (uint32_t)src[at + 2] << 16 | (uint32_t)src[at + 3] << 24;
        at += 4;
        if ((magic & 0xFFFFFFF0u) == 0x184D2A50u) {
            /* a frame to skip */
            if (srcSize - at < 4) { ok = false; break; }
            const uint32_t length = (uint32_t)src[at] | (uint32_t)src[at + 1] << 8 | (uint32_t)src[at + 2] << 16 | (uint32_t)src[at + 3] << 24;
            at += 4;
            if (length > srcSize - at) { ok = false; break; }
            at += length;
            continue;
        }
        if (magic != 0xFD2FB528u || at >= srcSize) { ok = false; break; }
        const int descriptor = src[at++];
        const int sizeFlag = descriptor >> 6, single = descriptor >> 5 & 1, checksum = descriptor >> 2 & 1, dictionary = descriptor & 3;
        if (descriptor & 8) { ok = false; break; }
        static const int dictionaryBytes[4] = {0, 1, 2, 4};
        const size_t sizeBytes = sizeFlag == 0 ? (size_t)single : (size_t)1 << sizeFlag;
        const size_t skip = (single ? 0u : 1u) + (size_t)dictionaryBytes[dictionary] + sizeBytes;
        if (skip > srcSize - at) { ok = false; break; }
        if (dictionary) {
            uint32_t id = 0;
            for (int i = 0; i < dictionaryBytes[dictionary]; ++i) id |= (uint32_t)src[at + (single ? 0u : 1u) + (size_t)i] << (8 * i);
            if (id) { ok = false; break; } /* (no dictionaries) */
        }
        at += skip;
        memset(fr, 0, sizeof *fr);
        fr->repeat[0] = 1;
        fr->repeat[1] = 4;
        fr->repeat[2] = 8;
        for (;;) {
            if (srcSize - at < 3) { ok = false; break; }
            const uint32_t head = (uint32_t)src[at] | (uint32_t)src[at + 1] << 8 | (uint32_t)src[at + 2] << 16;
            at += 3;
            const bool last = head & 1;
            const int type = head >> 1 & 3;
            const size_t size = head >> 3;
            if (type == 0) {
                if (size > srcSize - at || size > dstSize - done) { ok = false; break; }
                memcpy(dst + done, src + at, size);
                done += size;
                at += size;
            } else if (type == 1) {
                if (at >= srcSize || size > dstSize - done) { ok = false; break; }
                memset(dst + done, src[at], size);
                done += size;
                at += 1;
            } else if (type == 2) {
                if (size > srcSize - at || size > 131072 || !compressedBlock(fr, src + at, size, dst, &done, dstSize, literals)) { ok = false; break; }
                at += size;
            } else { ok = false; break; }
            if (last) break;
        }
        if (ok && checksum) {
            if (srcSize - at < 4) ok = false;
            else at += 4;
        }
    }
    artfFree(literals);
    artfFree(fr);
    return ok && done == dstSize;
}
