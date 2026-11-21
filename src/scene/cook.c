/* artful — what takes long to make, kept in a file (the plan's phase 4.3): meshes as built (meshlets, the positions the
 * CPU reads, lighter levels), so that reading them is a copy.
 *
 * A file is "ARTFCOOK", a version and a kind, then what that kind holds, little-endian. A record says its length first,
 * so that a reader skips what a newer writer put after what it knows: new versions read old files, and old ones read
 * what they can of new ones. */
#include "scene/sceneInternal.h"

#include <string.h>

void artfCookPut(ArtfCookBytes* b, const void* data, size_t size) {
    if (b->len + size > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->len + size) cap *= 2;
        b->items = artfRealloc(b->items, cap);
        b->cap = cap;
    }
    if (size) memcpy(b->items + b->len, data, size);
    b->len += size;
}
void artfCookPut32(ArtfCookBytes* b, uint32_t v) {
    const uint8_t w[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    artfCookPut(b, w, 4);
}
void artfCookPutFloat(ArtfCookBytes* b, float v) {
    uint32_t bits;
    memcpy(&bits, &v, 4);
    artfCookPut32(b, bits);
}

bool artfCookGet(ArtfCookReader* r, void* out, size_t size) {
    if (r->failed || size > r->size - r->at) {
        r->failed = true;
        if (out && size) memset(out, 0, size);
        return false;
    }
    if (out && size) memcpy(out, r->data + r->at, size);
    r->at += size;
    return true;
}
uint32_t artfCookGet32(ArtfCookReader* r) {
    uint8_t w[4];
    if (!artfCookGet(r, w, 4)) return 0;
    return (uint32_t)w[0] | (uint32_t)w[1] << 8 | (uint32_t)w[2] << 16 | (uint32_t)w[3] << 24;
}
float artfCookGetFloat(ArtfCookReader* r) {
    const uint32_t bits = artfCookGet32(r);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

/* (an array: its count of bytes, then them; the struct's own layout, as this build has it: what a file holds is the
 * layout of the version that wrote it, which ArtfPackedVertex and ArtfMeshlet keep) */
static void putArray(ArtfCookBytes* b, const void* data, size_t bytes) {
    artfCookPut32(b, (uint32_t)bytes);
    artfCookPut(b, data, bytes);
}
static void* getArray(ArtfCookReader* r, size_t want, bool aligned) {
    const uint32_t bytes = artfCookGet32(r);
    /* (nothing made of what the file does not hold: a count it says is not memory it takes) */
    if (r->failed || bytes != want || bytes > r->size - r->at) {
        r->failed = true;
        return NULL;
    }
    void* out = aligned ? artfAlignedMalloc(want + 64, 64) : artfMalloc(want + 1);
    if (!artfCookGet(r, out, want)) {
        if (aligned) artfAlignedFree(out);
        else artfFree(out);
        return NULL;
    }
    return out;
}

void artfCookPutMesh(ArtfCookBytes* b, const ArtfMeshData* m) {
    artfCookPut32(b, m->vertexCount);
    artfCookPut32(b, m->indexCount);
    artfCookPut32(b, m->meshletCount);
    artfCookPut32(b, m->positionStride);
    for (int k = 0; k < 3; ++k) artfCookPutFloat(b, m->scale[k]);
    for (int k = 0; k < 3; ++k) artfCookPutFloat(b, m->offset[k]);
    for (int k = 0; k < 3; ++k) artfCookPutFloat(b, m->center[k]);
    artfCookPutFloat(b, m->radius);
    artfCookPut32(b, (m->hasAlpha ? 1u : 0u) | (m->isLod ? 2u : 0u) | (m->colors ? 4u : 0u) | (m->dagBounds ? 8u : 0u));
    if (m->dagBounds) {
        /* (a graph of clusters: all its indices, the full mesh's meshlets, and each meshlet's bounds, after the colors) */
        artfCookPut32(b, m->dagIndexCount);
        artfCookPut32(b, m->dagWhole);
    }
    putArray(b, m->vertices, (size_t)m->vertexCount * sizeof *m->vertices);
    putArray(b, m->indices, (size_t)ARTF_MESH_ALL_INDICES(m) * 4);
    putArray(b, m->meshlets, (size_t)m->meshletCount * sizeof *m->meshlets);
    putArray(b, m->meshletVertices, (size_t)m->positionStride * 4);
    putArray(b, m->meshletTriangles, (size_t)ARTF_MESH_ALL_INDICES(m));
    putArray(b, m->positions, (size_t)m->positionStride * 3 * sizeof(float));
    if (m->colors) putArray(b, m->colors, (size_t)m->vertexCount * 4);
    if (m->dagBounds) putArray(b, m->dagBounds, (size_t)m->meshletCount * 10 * sizeof(float));
}

bool artfCookGetMesh(ArtfCookReader* r, ArtfMeshData* m) {
    memset(m, 0, sizeof *m);
    m->vertexCount = artfCookGet32(r);
    m->indexCount = artfCookGet32(r);
    m->meshletCount = artfCookGet32(r);
    m->positionStride = artfCookGet32(r);
    for (int k = 0; k < 3; ++k) m->scale[k] = artfCookGetFloat(r);
    for (int k = 0; k < 3; ++k) m->offset[k] = artfCookGetFloat(r);
    for (int k = 0; k < 3; ++k) m->center[k] = artfCookGetFloat(r);
    m->radius = artfCookGetFloat(r);
    const uint32_t flags = artfCookGet32(r);
    m->hasAlpha = flags & 1;
    m->isLod = (flags & 2) != 0;
    const bool dag = (flags & 8) != 0;
    uint32_t all = m->indexCount;
    if (dag) {
        all = artfCookGet32(r);
        m->dagWhole = artfCookGet32(r);
        if (all < m->indexCount || all > (1u << 30) || m->dagWhole > m->meshletCount) r->failed = true;
    }
    /* (no mesh of a file is past what memory takes: the counts are checked against what each array says it holds) */
    if (r->failed || m->vertexCount > (1u << 28) || m->indexCount > (1u << 30) || m->meshletCount > (1u << 26) || m->positionStride > (1u << 28)) {
        r->failed = true;
        return false;
    }
    m->vertices = getArray(r, (size_t)m->vertexCount * sizeof *m->vertices, false);
    m->indices = getArray(r, (size_t)all * 4, false);
    m->meshlets = getArray(r, (size_t)m->meshletCount * sizeof *m->meshlets, false);
    m->meshletVertices = getArray(r, (size_t)m->positionStride * 4, false);
    m->meshletTriangles = getArray(r, (size_t)all, false);
    m->positions = getArray(r, (size_t)m->positionStride * 3 * sizeof(float), true);
    if (flags & 4) m->colors = getArray(r, (size_t)m->vertexCount * 4, false);
    if (dag) {
        m->dagBounds = getArray(r, (size_t)m->meshletCount * 10 * sizeof(float), false);
        m->dagIndexCount = all;
    }
    if (r->failed) {
        artfMeshFini(m);
        memset(m, 0, sizeof *m);
        return false;
    }
    /* (what refers into the arrays must stay in them: a file is not trusted) */
    for (uint32_t i = 0; i < m->meshletCount; ++i) {
        const ArtfMeshlet* l = &m->meshlets[i];
        if ((uint64_t)l->vertexOffset + l->vertexCount > m->positionStride || ((uint64_t)l->triangleOffset + l->triangleCount) * 3 > all || l->vertexCount > 256) r->failed = true;
    }
    for (uint32_t i = 0; i < all && !r->failed; ++i)
        if (m->indices[i] >= m->vertexCount) r->failed = true;
    for (uint32_t i = 0; i < m->positionStride && !r->failed; ++i)
        if (m->meshletVertices[i] >= m->vertexCount && m->vertexCount) r->failed = true;
    if (r->failed) {
        artfMeshFini(m);
        memset(m, 0, sizeof *m);
        return false;
    }
    return true;
}

uint64_t artfCookHash(const void* data, size_t size) {
    const uint8_t* p = data;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

/* (all a texture's mipmaps hold, in pixels) */
static size_t texturePixels(const ArtfTextureData* t) {
    size_t total = 0;
    int w = t->width, h = t->height;
    for (int level = 0; level < t->mipCount; ++level) {
        total += (size_t)w * (size_t)h;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return total;
}

void artfCookPutTexture(ArtfCookBytes* b, const ArtfTextureData* t) {
    artfCookPut32(b, (uint32_t)t->width);
    artfCookPut32(b, (uint32_t)t->height);
    artfCookPut32(b, (uint32_t)t->mipCount);
    artfCookPut32(b, t->light ? 1u : 0u);
    for (int k = 0; k < 16; ++k) artfCookPut32(b, t->offsets[k]);
    putArray(b, t->pixels, texturePixels(t) * 4);
}

bool artfCookGetTexture(ArtfCookReader* r, ArtfTextureData* t) {
    memset(t, 0, sizeof *t);
    t->width = (int)artfCookGet32(r);
    t->height = (int)artfCookGet32(r);
    t->mipCount = (int)artfCookGet32(r);
    t->light = (artfCookGet32(r) & 1) != 0;
    for (int k = 0; k < 16; ++k) t->offsets[k] = artfCookGet32(r);
    if (r->failed || t->width <= 0 || t->height <= 0 || t->width > 65536 || t->height > 65536 || t->mipCount < 1 || t->mipCount > 16) {
        r->failed = true;
        return false;
    }
    /* (the mipmaps where they are, one after another: as artfPrepareTexture lays them) */
    size_t at = 0;
    int w = t->width, h = t->height;
    for (int level = 0; level < t->mipCount; ++level) {
        if (t->offsets[level] != at) {
            r->failed = true;
            return false;
        }
        at += (size_t)w * (size_t)h;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    t->pixels = getArray(r, at * 4, false);
    return t->pixels != NULL;
}

/* (of the instances there are, the first count: what they are, and where; what a cooked file was made against) */
uint64_t artfCookInstancesPrint(const ArtfScene* s, uint32_t count) {
    uint64_t h = 1469598103934665603ull;
#define MIX(v) do { h ^= (uint64_t)(v); h *= 1099511628211ull; } while (0)
    for (uint32_t i = 0; i < count && i < s->instances.len; ++i) {
        const ArtfInstanceData* d = &s->instances.items[i];
        MIX(d->mesh);
        MIX(d->material);
        uint32_t bits[12];
        memcpy(bits, d->m, sizeof bits);
        for (int k = 0; k < 12; ++k) MIX(bits[k]);
    }
#undef MIX
    return h;
}
