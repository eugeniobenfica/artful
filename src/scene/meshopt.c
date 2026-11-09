/* artful — glTF's EXT_meshopt_compression: meshoptimizer's codecs for vertices, triangles and indices, and its
 * filters, decoded as its specification has them (version 0 of vertices; 0 and 1 of triangles and of indices). */
#include "scene/sceneInternal.h"

#include <string.h>

/* ---- vertices: each byte of a vertex on its own, as differences from the vertex before, in blocks ---- */

enum { BLOCK_BYTES = 8192, BLOCK_MOST = 256, GROUP = 16, GROUP_LIMIT = 24, TAIL_LEAST = 32 };

static const uint8_t* bytesGroup(const uint8_t* data, uint8_t* out, int bitsLog2) {
    if (bitsLog2 == 0) {
        memset(out, 0, GROUP);
        return data;
    }
    if (bitsLog2 == 3) {
        memcpy(out, data, GROUP);
        return data + GROUP;
    }
    /* 2 or 4 bits each, the highest first; all ones: the byte is among those after */
    const int bits = bitsLog2 == 1 ? 2 : 4, each = 8 / bits, all = (1 << bits) - 1;
    const uint8_t* extra = data + GROUP / each;
    for (int i = 0; i < GROUP; ++i) {
        const int value = data[i / each] >> (8 - bits * (i % each + 1)) & all;
        out[i] = value == all ? *extra++ : (uint8_t)value;
    }
    return extra;
}

static const uint8_t* bytesOf(const uint8_t* data, const uint8_t* end, uint8_t* out, size_t count) {
    const size_t headers = (count / GROUP + 3) / 4;
    if ((size_t)(end - data) < headers) return NULL;
    const uint8_t* header = data;
    data += headers;
    for (size_t i = 0; i < count; i += GROUP) {
        if (end - data < GROUP_LIMIT) return NULL;
        const size_t at = i / GROUP;
        data = bytesGroup(data, out + i, header[at / 4] >> (at % 4 * 2) & 3);
    }
    return data;
}

int artfMeshoptDecodeVertices(uint8_t* out, size_t count, size_t stride, const uint8_t* data, size_t size) {
    const uint8_t* end = data + size;
    if (stride == 0 || stride > 256 || stride % 4 || size < 1 + stride) return -2;
    if ((*data & 0xF0) != 0xA0 || (*data & 0x0F) > 0) return -1;
    ++data;
    uint8_t last[256], deltas[BLOCK_MOST];
    memcpy(last, end - stride, stride);
    size_t block = BLOCK_BYTES / stride & ~(size_t)(GROUP - 1);
    if (block > BLOCK_MOST) block = BLOCK_MOST;
    for (size_t first = 0; first < count; first += block) {
        const size_t n = count - first < block ? count - first : block, aligned = (n + GROUP - 1) & ~(size_t)(GROUP - 1);
        for (size_t k = 0; k < stride; ++k) {
            data = bytesOf(data, end, deltas, aligned);
            if (!data) return -2;
            uint8_t p = last[k];
            for (size_t i = 0; i < n; ++i) {
                /* (zig-zag: the low bit says the sign) */
                const uint8_t d = deltas[i];
                p = (uint8_t)(p + ((d >> 1) ^ (uint8_t)(0 - (d & 1))));
                out[(first + i) * stride + k] = p;
            }
            last[k] = p;
        }
    }
    const size_t tail = stride < TAIL_LEAST ? TAIL_LEAST : stride;
    return (size_t)(end - data) == tail ? 0 : -3;
}

/* ---- triangles: edges and vertices met lately, in two rings of sixteen ---- */

static uint32_t vbyte(const uint8_t** data) {
    const uint8_t* p = *data;
    uint32_t lead = *p++;
    if (lead < 128) {
        *data = p;
        return lead;
    }
    uint32_t result = lead & 127, shift = 7;
    for (int i = 0; i < 4; ++i) {
        const uint32_t group = *p++;
        result |= (group & 127) << shift;
        shift += 7;
        if (group < 128) break;
    }
    *data = p;
    return result;
}

static uint32_t freeIndex(const uint8_t** data, uint32_t last) {
    const uint32_t v = vbyte(data);
    return last + ((v >> 1) ^ (0u - (v & 1)));
}

static void put(uint8_t* out, size_t at, size_t indexSize, uint32_t a) {
    if (indexSize == 2) {
        const uint16_t v = (uint16_t)a;
        memcpy(out + at * 2, &v, 2);
    } else {
        memcpy(out + at * 4, &a, 4);
    }
}

typedef struct Rings {
    uint32_t edges[16][2], vertices[16];
    uint32_t edge, vertex;
} Rings;

static void pushVertex(Rings* r, uint32_t v, uint32_t advance) {
    r->vertices[r->vertex] = v;
    r->vertex = (r->vertex + advance) & 15;
}

static void pushEdge(Rings* r, uint32_t a, uint32_t b) {
    r->edges[r->edge][0] = a;
    r->edges[r->edge][1] = b;
    r->edge = (r->edge + 1) & 15;
}

int artfMeshoptDecodeTriangles(uint8_t* out, size_t count, size_t indexSize, const uint8_t* data, size_t size) {
    if (count % 3 || (indexSize != 2 && indexSize != 4)) return -1;
    if (size < 1 + count / 3 + 16) return -2;
    if ((data[0] & 0xF0) != 0xE0 || (data[0] & 0x0F) > 1) return -1;
    const uint32_t most = (data[0] & 0x0F) >= 1 ? 13 : 15;
    Rings r;
    memset(&r, 0xFF, sizeof r);
    r.edge = r.vertex = 0;
    uint32_t next = 0, last = 0;
    const uint8_t* code = data + 1;
    const uint8_t* at = code + count / 3;
    const uint8_t* safeEnd = data + size - 16;
    const uint8_t* table = safeEnd;
    for (size_t i = 0; i < count; i += 3) {
        if (at > safeEnd) return -2;
        const uint32_t tri = *code++;
        uint32_t a, b, c;
        if (tri < 0xF0) {
            /* an edge met lately, and a vertex: new, met lately, or free */
            const uint32_t fe = tri >> 4, fec = tri & 15;
            a = r.edges[(r.edge - 1 - fe) & 15][0];
            b = r.edges[(r.edge - 1 - fe) & 15][1];
            if (fec < most) {
                c = fec == 0 ? next : r.vertices[(r.vertex - 1 - fec) & 15];
                next += fec == 0;
                put(out, i, indexSize, a);
                put(out, i + 1, indexSize, b);
                put(out, i + 2, indexSize, c);
                pushVertex(&r, c, fec == 0);
            } else {
                /* (13 and 14 are the last free index, less or more one) */
                c = last = fec != 15 ? last + (fec - (fec ^ 3)) : freeIndex(&at, last);
                put(out, i, indexSize, a);
                put(out, i + 1, indexSize, b);
                put(out, i + 2, indexSize, c);
                pushVertex(&r, c, 1);
            }
            pushEdge(&r, c, b);
            pushEdge(&r, a, c);
            continue;
        }
        uint32_t feb, fec;
        if (tri < 0xFE) {
            /* three vertices, the first new: the others from the table */
            const uint32_t aux = table[tri & 15];
            feb = aux >> 4;
            fec = aux & 15;
            a = next++;
            b = feb == 0 ? next : r.vertices[(r.vertex - feb) & 15];
            next += feb == 0;
            c = fec == 0 ? next : r.vertices[(r.vertex - fec) & 15];
            next += fec == 0;
            put(out, i, indexSize, a);
            put(out, i + 1, indexSize, b);
            put(out, i + 2, indexSize, c);
            pushVertex(&r, a, 1);
            pushVertex(&r, b, feb == 0);
            pushVertex(&r, c, fec == 0);
        } else {
            const uint32_t aux = *at++, fea = tri == 0xFE ? 0 : 15;
            feb = aux >> 4;
            fec = aux & 15;
            if (aux == 0) next = 0;
            a = fea == 0 ? next++ : 0;
            b = feb == 0 ? next++ : r.vertices[(r.vertex - feb) & 15];
            c = fec == 0 ? next++ : r.vertices[(r.vertex - fec) & 15];
            if (fea == 15) last = a = freeIndex(&at, last);
            if (feb == 15) last = b = freeIndex(&at, last);
            if (fec == 15) last = c = freeIndex(&at, last);
            put(out, i, indexSize, a);
            put(out, i + 1, indexSize, b);
            put(out, i + 2, indexSize, c);
            pushVertex(&r, a, 1);
            pushVertex(&r, b, feb == 0 || feb == 15);
            pushVertex(&r, c, fec == 0 || fec == 15);
        }
        pushEdge(&r, b, a);
        pushEdge(&r, c, b);
        pushEdge(&r, a, c);
    }
    return at == safeEnd ? 0 : -3;
}

/* ---- indices in any order: differences from one of the two last ---- */

int artfMeshoptDecodeIndices(uint8_t* out, size_t count, size_t indexSize, const uint8_t* data, size_t size) {
    if (indexSize != 2 && indexSize != 4) return -1;
    if (size < 1 + count + 4) return -2;
    if ((data[0] & 0xF0) != 0xD0 || (data[0] & 0x0F) > 1) return -1;
    const uint8_t* at = data + 1;
    const uint8_t* safeEnd = data + size - 4;
    uint32_t last[2] = {0, 0};
    for (size_t i = 0; i < count; ++i) {
        if (at >= safeEnd) return -2;
        uint32_t v = vbyte(&at);
        const uint32_t which = v & 1;
        v >>= 1;
        last[which] += (v >> 1) ^ (0u - (v & 1));
        put(out, i, indexSize, last[which]);
    }
    return at == safeEnd ? 0 : -3;
}

/* ---- filters: what the bytes were before they were made to compress well ---- */

static int rounded(float x) { return (int)(x + (x >= 0 ? .5f : -.5f)); }

void artfMeshoptFilter(uint8_t* data, size_t count, size_t stride, int filter) {
    if (filter == 1) {
        /* octahedral: x and y, and z to be made again; a length of one at the bits there are */
        for (size_t i = 0; i < count; ++i) {
            uint8_t* v = data + i * stride;
            int8_t b8[4];
            int16_t b16[4];
            float x, y, z, top;
            if (stride == 4) {
                memcpy(b8, v, 4);
                x = b8[0]; y = b8[1]; z = b8[2]; top = 127.f;
            } else {
                memcpy(b16, v, 8);
                x = b16[0]; y = b16[1]; z = b16[2]; top = 32767.f;
            }
            z = z - fabsf(x) - fabsf(y);
            const float t = z >= 0 ? 0.f : z;
            x += x >= 0 ? t : -t;
            y += y >= 0 ? t : -t;
            const float scale = top / sqrtf(x * x + y * y + z * z);
            const int out[3] = {rounded(x * scale), rounded(y * scale), rounded(z * scale)};
            if (stride == 4) {
                for (int k = 0; k < 3; ++k) b8[k] = (int8_t)out[k];
                memcpy(v, b8, 4);
            } else {
                for (int k = 0; k < 3; ++k) b16[k] = (int16_t)out[k];
                memcpy(v, b16, 8);
            }
        }
    } else if (filter == 2 && stride == 8) {
        /* quaternions: three, and which was left out, with the scale they were kept at */
        for (size_t i = 0; i < count; ++i) {
            int16_t q[4];
            memcpy(q, data + i * 8, 8);
            const int sf = q[3] | 3;
            const float ss = .70710678f / (float)sf;
            const float x = (float)q[0] * ss, y = (float)q[1] * ss, z = (float)q[2] * ss;
            const float ww = 1.f - x * x - y * y - z * z, w = sqrtf(ww >= 0 ? ww : 0);
            const int qc = q[3] & 3;
            int16_t out[4];
            out[(qc + 1) & 3] = (int16_t)rounded(x * 32767.f);
            out[(qc + 2) & 3] = (int16_t)rounded(y * 32767.f);
            out[(qc + 3) & 3] = (int16_t)rounded(z * 32767.f);
            out[qc] = (int16_t)(int)(w * 32767.f + .5f);
            memcpy(data + i * 8, out, 8);
        }
    } else if (filter == 3) {
        /* exponential: a mantissa of 24 bits and an exponent of 8, to a float */
        for (size_t i = 0; i < count * (stride / 4); ++i) {
            uint32_t v;
            memcpy(&v, data + i * 4, 4);
            const int32_t m = (int32_t)(v << 8) >> 8, e = (int32_t)v >> 24;
            uint32_t bits = (uint32_t)(e + 127) << 23;
            float f;
            memcpy(&f, &bits, 4);
            f *= (float)m;
            memcpy(data + i * 4, &f, 4);
        }
    }
}
