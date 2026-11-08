/* artful — the scene. */
#include "scene/sceneInternal.h"
#include "scene/simplifyInternal.h"

#include <stdatomic.h>
#include <stdlib.h>

#define LOD_MIN_TRIANGLES 128

static const char* const featureNames[ARTF_FEATURE_COUNT] = {"lod", "occlusion", "cones", "occlusion-auto", "lod-fade", "layers-sorted"};

/* ARTFUL_FEATURES=+name,-name */
static void readEnvironment(ArtfScene* s) {
    const char* text = getenv("ARTFUL_FEATURES");
    while (text && *text) {
        const bool on = *text != '-';
        if (*text == '+' || *text == '-') ++text;
        size_t len = 0;
        while (text[len] && text[len] != ',') ++len;
        for (int i = 0; i < ARTF_FEATURE_COUNT; ++i)
            if (strlen(featureNames[i]) == len && !memcmp(featureNames[i], text, len)) s->features[i] = on;
        text += len;
        if (*text == ',') ++text;
    }
}

ArtfScene* artfSceneNew(void) {
    static atomic_ullong lastId;
    ArtfScene* s = ARTF_NEW(ArtfScene);
    s->id = atomic_fetch_add(&lastId, 1) + 1;
    const uint32_t white = 0xFFFFFFFFu;
    artfSceneAddTexture(s, &white, 1, 1, 1);
    ArtfMaterialData grey = {{.6f, .6f, .6f}, 1.f, {0, 0, 0}, .5f, 0, 1.f, 0, 0, 0, 0, 1.f, 1.f, 0, 0, 0, 0, {{0}}};
    artfVecPush(&s->materials, grey);
    s->toSun = artfVec3Normalize(artfVec3(.4f, .8f, .45f));
    s->sunColor = artfVec3(1.f, .96f, .9f);
    s->sky = artfVec3(.28f, .32f, .4f);
    s->ground = artfVec3(.12f, .1f, .09f);
    s->background = artfVec3(.05f, .06f, .08f);
    s->backgroundAlpha = 1.f;
    s->camera.position = artfVec3(0, 2, 6);
    s->camera.up = artfVec3(0, 1, 0);
    s->camera.fovY = 1.f;
    s->camera.near_ = .1f;
    s->features[ARTF_FEATURE_LOD] = true;
    s->features[ARTF_FEATURE_CONES] = true;
    s->lodError = 1.f;
    readEnvironment(s);
    return s;
}

void artfSceneFree(ArtfScene* s) {
    if (!s) return;
    for (size_t i = 0; i < s->meshes.len; ++i) artfMeshFini(&s->meshes.items[i]);
    for (size_t i = 0; i < s->textures.len; ++i) {
        artfFree(s->textures.items[i].pixels);
        artfFree(s->textures.items[i].blocks);
    }
    artfVecFree(&s->meshes);
    artfVecFree(&s->freeMeshes);
    artfVecFree(&s->textures);
    artfVecFree(&s->materials);
    artfVecFree(&s->instances);
    artfVecFree(&s->instanceHandles);
    artfVecFree(&s->handleSlots);
    artfVecFree(&s->handleGenerations);
    artfVecFree(&s->deforming);
    artfVecFree(&s->freeTextures);
    for (size_t i = 0; i < s->graphs.len; ++i) artfFree(s->graphs.items[i].nodes);
    artfVecFree(&s->graphs);
    if (s->modelsFree) s->modelsFree(s->models);
    artfTraceFini(&s->trace);
    artfFree(s->hole.map);
    artfFluidFree(s->hole.fluid);
    artfFree(s);
}

/* ---- meshes ---- */

/* Lighter versions of a mesh, each a quarter of the one before. Touches no scene: any thread may do this. */
static uint32_t makeLevels(const ArtfMeshData* m, ArtfMeshData* made, float* errors) {
    const uint32_t triangles = m->indexCount / 3;
    if (m->isLod || m->lodCount || m->skin || triangles < LOD_MIN_TRIANGLES) return 0;
    const uint32_t targets[ARTF_MAX_LODS] = {triangles / 4, triangles / 16, triangles / 64};
    uint32_t wanted = 0;
    while (wanted < ARTF_MAX_LODS && targets[wanted] >= 8) ++wanted;
    ArtfVertex* vertices = artfMalloc((size_t)m->vertexCount * sizeof *vertices);
    artfMeshUnpack(m, vertices);
    ArtfSimplified levels[ARTF_MAX_LODS];
    const uint32_t count = artfSimplify(vertices, m->vertexCount, m->indices, m->indexCount, targets, wanted, levels);
    const uint32_t vertexCount = m->vertexCount;
    uint32_t count2 = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (artfMeshBuild(&made[count2], vertices, vertexCount, levels[i].indices, levels[i].indexCount, NULL, NULL, 0)) {
            made[count2].isLod = true;
            errors[count2++] = levels[i].error;
        }
        artfFree(levels[i].indices);
    }
    artfFree(vertices);
    return count2;
}

/* A place for a mesh: one that was left, or a new one. What was there is seen to have gone. */
static uint32_t meshPlace(ArtfScene* s, const ArtfMeshData* m) {
    if (!s->freeMeshes.len) {
        artfVecPush(&s->meshes, *m);
        return (uint32_t)s->meshes.len - 1;
    }
    const uint32_t index = s->freeMeshes.items[--s->freeMeshes.len];
    const uint32_t generation = s->meshes.items[index].generation + 1;
    s->meshes.items[index] = *m;
    s->meshes.items[index].generation = generation;
    ++s->meshRemovals;
    return index;
}

/* What was made goes into the scene, after its mesh. */
static void takeLevels(ArtfScene* s, uint32_t index, const ArtfMeshData* made, const float* errors, uint32_t count) {
    uint32_t lods[ARTF_MAX_LODS];
    for (uint32_t i = 0; i < count; ++i) {
        lods[i] = meshPlace(s, &made[i]);
    }
    ArtfMeshData* base = &s->meshes.items[index]; /* (the list may have moved) */
    base->lodCount = count;
    ++s->lodVersion;
    memcpy(base->lods, lods, (size_t)count * sizeof *lods);
    memcpy(base->lodErrors, errors, (size_t)count * sizeof *errors);
}

static void addLods(ArtfScene* s, uint32_t index) {
    ArtfMeshData made[ARTF_MAX_LODS];
    float errors[ARTF_MAX_LODS];
    const uint32_t count = makeLevels(&s->meshes.items[index], made, errors);
    if (count || (!s->meshes.items[index].lodCount && !s->meshes.items[index].isLod && !s->meshes.items[index].skin && s->meshes.items[index].indexCount / 3 >= LOD_MIN_TRIANGLES)) takeLevels(s, index, made, errors, count);
}

struct ArtfPreparedMesh {
    ArtfMeshData mesh, levels[ARTF_MAX_LODS];
    float errors[ARTF_MAX_LODS];
    uint32_t levelCount;
};

ArtfPreparedMesh* artfPrepareMesh(const ArtfVertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount, bool levels) {
    ArtfPreparedMesh* p = ARTF_NEW(ArtfPreparedMesh);
    if (!artfMeshBuild(&p->mesh, vertices, vertexCount, indices, indexCount, NULL, NULL, 0)) {
        artfFree(p);
        return NULL;
    }
    if (levels) p->levelCount = makeLevels(&p->mesh, p->levels, p->errors);
    return p;
}

void artfPreparedMeshFree(ArtfPreparedMesh* p) {
    if (!p) return;
    artfMeshFini(&p->mesh);
    for (uint32_t i = 0; i < p->levelCount; ++i) artfMeshFini(&p->levels[i]);
    artfFree(p);
}

ArtfMesh artfSceneAddPreparedMesh(ArtfScene* s, ArtfPreparedMesh* p) {
    if (!p) return 0;
    const uint32_t index = meshPlace(s, &p->mesh);
    if (p->levelCount) takeLevels(s, index, p->levels, p->errors, p->levelCount);
    artfFree(p);
    return index + 1;
}

/* Is a mesh, or a copy of its skin, still drawn? */
static bool meshInUse(const ArtfScene* s, uint32_t index) {
    const ArtfMeshData* m = &s->meshes.items[index];
    for (size_t i = 0; i < s->instances.len; ++i) {
        const ArtfMeshData* used = &s->meshes.items[s->instances.items[i].mesh];
        if (s->instances.items[i].mesh == index || (m->skin && used->skin == m->skin)) return true;
    }
    return false;
}

static void forgetMesh(ArtfScene* s, uint32_t index) {
    ArtfMeshData* m = &s->meshes.items[index];
    const uint32_t generation = m->generation;
    artfMeshFini(m);
    memset(m, 0, sizeof *m);
    m->removed = true;
    m->generation = generation;
    artfVecPush(&s->freeMeshes, index);
    ++s->meshRemovals;
}

bool artfSceneRemoveMesh(ArtfScene* s, ArtfMesh mesh) {
    if (!mesh || mesh > s->meshes.len || s->meshes.items[mesh - 1].removed) {
        artfLog("artful: mesh %u is not there", mesh);
        return false;
    }
    if (meshInUse(s, mesh - 1)) {
        artfLog("artful: mesh %u is still in use", mesh);
        return false;
    }
    const ArtfMeshData* m = &s->meshes.items[mesh - 1];
    uint32_t lods[ARTF_MAX_LODS];
    const uint32_t count = m->lodCount;
    memcpy(lods, m->lods, sizeof lods);
    for (uint32_t i = 0; i < count; ++i) forgetMesh(s, lods[i]);
    forgetMesh(s, mesh - 1);
    return true;
}

ArtfMesh artfSceneAddMesh(ArtfScene* s, const ArtfVertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount) {
    ArtfMeshData m;
    if (!artfMeshBuild(&m, vertices, vertexCount, indices, indexCount, NULL, NULL, 0)) return 0;
    const uint32_t index = meshPlace(s, &m);
    if (s->features[ARTF_FEATURE_LOD]) addLods(s, index);
    return index + 1;
}

ArtfMesh artfSceneAddSkinnedMesh(ArtfScene* s, const ArtfVertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount, const ArtfVertexSkin* skin) {
    ArtfMeshData m;
    if (!artfMeshBuild(&m, vertices, vertexCount, indices, indexCount, skin, NULL, 0)) return 0;
    return meshPlace(s, &m) + 1;
}

ArtfMesh artfSceneAddMorphMesh(ArtfScene* s, const ArtfVertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount, const ArtfVertexSkin* skin, const ArtfMorphTarget* targets, uint32_t targetCount) {
    ArtfMeshData m;
    if (!artfMeshBuild(&m, vertices, vertexCount, indices, indexCount, skin, targets, targetCount)) return 0;
    return meshPlace(s, &m) + 1;
}

void artfSceneSetFeature(ArtfScene* s, ArtfFeature feature, bool on) {
    if (feature >= ARTF_FEATURE_COUNT) return;
    s->features[feature] = on;
    if (feature == ARTF_FEATURE_LOD && on) {
        const size_t count = s->meshes.len; /* (not the ones this makes) */
        for (size_t i = 0; i < count; ++i)
            if (!s->meshes.items[i].isLod) addLods(s, (uint32_t)i);
    }
}

bool artfSceneFeature(const ArtfScene* s, ArtfFeature feature) { return feature < ARTF_FEATURE_COUNT && s->features[feature]; }
void artfSceneSetLodError(ArtfScene* s, float pixels) { s->lodError = artfMaxf(pixels, .01f); }

ArtfMesh artfSceneAddCube(ArtfScene* s, float size) {
    static const float faces[6][9] = { /* normal, then the u and the v axes */
        {0, 0, 1, 1, 0, 0, 0, 1, 0},   {0, 0, -1, -1, 0, 0, 0, 1, 0}, {1, 0, 0, 0, 0, -1, 0, 1, 0},
        {-1, 0, 0, 0, 0, 1, 0, 1, 0},  {0, 1, 0, 1, 0, 0, 0, 0, -1},  {0, -1, 0, 1, 0, 0, 0, 0, 1}};
    ArtfVertex v[24];
    uint32_t idx[36];
    const float h = size * .5f;
    for (int f = 0; f < 6; ++f) {
        const float* a = faces[f];
        for (int c = 0; c < 4; ++c) {
            float su = (c == 1 || c == 2) ? 1.f : -1.f, sv = c >= 2 ? 1.f : -1.f;
            ArtfVertex* o = &v[f * 4 + c];
            o->px = (a[0] + a[3] * su + a[6] * sv) * h;
            o->py = (a[1] + a[4] * su + a[7] * sv) * h;
            o->pz = (a[2] + a[5] * su + a[8] * sv) * h;
            o->nx = a[0];
            o->ny = a[1];
            o->nz = a[2];
            o->u = su * .5f + .5f;
            o->v = .5f - sv * .5f;
            o->color = 0;
        }
        static const uint32_t quad[6] = {0, 1, 2, 0, 2, 3};
        for (int i = 0; i < 6; ++i) idx[f * 6 + i] = (uint32_t)f * 4 + quad[i];
    }
    return artfSceneAddMesh(s, v, 24, idx, 36);
}

/* A grid of rings x segments, wrapped by fn; the triangles face as the grid's u cross v does. */
typedef void (*GridFn)(const float* args, float u, float v, ArtfVertex* out);

static ArtfMesh addGrid(ArtfScene* s, GridFn fn, const float* args, int rings, int segments) {
    rings = artfMaxi(rings, 2);
    segments = artfMaxi(segments, 3);
    const uint32_t columns = (uint32_t)segments + 1, count = ((uint32_t)rings + 1) * columns;
    ArtfVertex* v = artfMalloc((size_t)count * sizeof *v);
    uint32_t* idx = artfMalloc((size_t)rings * segments * 6 * 4);
    uint32_t n = 0;
    for (int r = 0; r <= rings; ++r)
        for (int c = 0; c <= segments; ++c) {
            float u = (float)c / (float)segments, w = (float)r / (float)rings;
            ArtfVertex* o = &v[(uint32_t)r * columns + (uint32_t)c];
            fn(args, u, w, o);
            o->u = u;
            o->v = w;
            o->color = 0;
        }
    for (uint32_t r = 0; r < (uint32_t)rings; ++r)
        for (uint32_t c = 0; c < (uint32_t)segments; ++c) {
            uint32_t a = r * columns + c, b = a + 1, d = a + columns, e = d + 1;
            idx[n++] = a; idx[n++] = d; idx[n++] = b;
            idx[n++] = b; idx[n++] = d; idx[n++] = e;
        }
    ArtfMesh m = artfSceneAddMesh(s, v, count, idx, n);
    artfFree(v);
    artfFree(idx);
    return m;
}

#define PI 3.14159265358979f

static void spherePoint(const float* args, float u, float w, ArtfVertex* o) {
    float theta = u * 2 * PI, phi = w * PI;
    o->nx = sinf(phi) * sinf(theta);
    o->ny = cosf(phi);
    o->nz = sinf(phi) * cosf(theta);
    o->px = o->nx * args[0];
    o->py = o->ny * args[0];
    o->pz = o->nz * args[0];
}
ArtfMesh artfSceneAddSphere(ArtfScene* s, float radius, int rings, int segments) { return addGrid(s, spherePoint, &radius, rings, segments); }

static void torusPoint(const float* args, float u, float w, ArtfVertex* o) {
    float theta = u * 2 * PI, phi = w * 2 * PI;
    float cx = sinf(theta), cz = cosf(theta), cp = cosf(phi), sp = sinf(phi);
    o->nx = cx * cp;
    o->ny = -sp;
    o->nz = cz * cp;
    o->px = cx * args[0] + o->nx * args[1];
    o->py = o->ny * args[1];
    o->pz = cz * args[0] + o->nz * args[1];
}
ArtfMesh artfSceneAddTorus(ArtfScene* s, float radius, float tube, int rings, int segments) {
    const float args[2] = {radius, tube};
    return addGrid(s, torusPoint, args, rings, segments);
}

ArtfMesh artfSceneAddPlane(ArtfScene* s, float size, float uvRepeat) {
    const float h = size * .5f;
    ArtfVertex v[4] = {{-h, 0, -h, 0, 1, 0, 0, 0}, {-h, 0, h, 0, 1, 0, 0, uvRepeat}, {h, 0, h, 0, 1, 0, uvRepeat, uvRepeat}, {h, 0, -h, 0, 1, 0, uvRepeat, 0}};
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    return artfSceneAddMesh(s, v, 4, idx, 6);
}

/* ---- textures and materials ---- */

static uint32_t average(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    /* (each byte alone; two at a time, as they fit in the gaps) */
    uint32_t rb = ((a & 0xFF00FF) + (b & 0xFF00FF) + (c & 0xFF00FF) + (d & 0xFF00FF) + 0x020002) >> 2 & 0xFF00FF;
    uint32_t g = (((a >> 8) & 0xFF) + ((b >> 8) & 0xFF) + ((c >> 8) & 0xFF) + ((d >> 8) & 0xFF) + 2) >> 2 & 0xFF;
    uint32_t alpha = ((a >> 24) + (b >> 24) + (c >> 24) + (d >> 24) + 2) >> 2;
    return alpha << 24 | rb | g << 8;
}

struct ArtfPreparedTexture { ArtfTextureData texture; };

void artfPreparedTextureFree(ArtfPreparedTexture* p) {
    if (!p) return;
    artfFree(p->texture.pixels);
    artfFree(p->texture.blocks);
    artfFree(p);
}

/* In the place of one that went, or a new one. */
ArtfTexture artfSceneStoreTexture(ArtfScene* s, const ArtfTextureData* t) {
    ++s->textureVersion;
    s->decodedTextures = 0;
    if (s->freeTextures.len) {
        const uint32_t index = s->freeTextures.items[--s->freeTextures.len];
        const uint32_t generation = s->textures.items[index].generation;
        s->textures.items[index] = *t;
        s->textures.items[index].generation = generation + 1;
        return index;
    }
    artfVecPush(&s->textures, *t);
    return (ArtfTexture)(s->textures.len - 1);
}

ArtfPreparedTexture* artfPrepareTexture(const uint32_t* pixels, int width, int height, int stride) {
    if (!pixels || width <= 0 || height <= 0) return NULL;
    if (!stride) stride = width;
    ArtfTextureData t;
    memset(&t, 0, sizeof t);
    t.width = width;
    t.height = height;
    t.mipCount = 1;
    size_t total = 0;
    for (int w = width, h = height; t.mipCount <= 16; w = artfMaxi(w / 2, 1), h = artfMaxi(h / 2, 1)) {
        t.offsets[t.mipCount - 1] = (uint32_t)total;
        total += (size_t)w * h;
        if (w == 1 && h == 1) break;
        ++t.mipCount;
    }
    t.pixels = artfMalloc(total * 4);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) t.pixels[(size_t)y * width + x] = pixels[(size_t)y * stride + x];
    int w = width, h = height;
    for (int level = 1; level < t.mipCount; ++level) {
        const uint32_t* from = t.pixels + t.offsets[level - 1];
        uint32_t* to = t.pixels + t.offsets[level];
        int nw = artfMaxi(w / 2, 1), nh = artfMaxi(h / 2, 1);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                int x0 = artfMini(x * 2, w - 1), x1 = artfMini(x * 2 + 1, w - 1), y0 = artfMini(y * 2, h - 1), y1 = artfMini(y * 2 + 1, h - 1);
                to[(size_t)y * nw + x] = average(from[(size_t)y0 * w + x0], from[(size_t)y0 * w + x1], from[(size_t)y1 * w + x0], from[(size_t)y1 * w + x1]);
            }
        w = nw;
        h = nh;
    }
    ArtfPreparedTexture* p = ARTF_NEW(ArtfPreparedTexture);
    p->texture = t;
    return p;
}

ArtfTexture artfSceneAddPreparedTexture(ArtfScene* s, ArtfPreparedTexture* p) {
    if (!p) return 0;
    const ArtfTexture id = artfSceneStoreTexture(s, &p->texture);
    artfFree(p);
    return id;
}

ArtfTexture artfSceneAddTexture(ArtfScene* s, const uint32_t* pixels, int width, int height, int stride) {
    return artfSceneAddPreparedTexture(s, artfPrepareTexture(pixels, width, height, stride));
}

bool artfSceneRemoveTexture(ArtfScene* s, ArtfTexture texture) {
    if (!texture || texture >= s->textures.len || s->textures.items[texture].removed) {
        artfLog("artful: texture %u is not there", texture);
        return false;
    }
    for (size_t i = 0; i < s->materials.len; ++i) {
        const ArtfMaterialData* m = &s->materials.items[i];
        if (m->texture == texture || m->normalMap == texture || m->surfaceMap == texture || m->occlusionMap == texture) {
            artfLog("artful: texture %u is still in use", texture);
            return false;
        }
    }
    for (size_t g = 0; g < s->graphs.len; ++g)
        for (uint32_t i = 0; i < s->graphs.items[g].count; ++i)
            if (s->graphs.items[g].nodes[i].op == ARTF_OP_TEXTURE && s->graphs.items[g].nodes[i].b == texture) {
                artfLog("artful: texture %u is still in use", texture);
                return false;
            }
    ArtfTextureData* t = &s->textures.items[texture];
    artfFree(t->pixels);
    artfFree(t->blocks);
    const uint32_t generation = t->generation;
    memset(t, 0, sizeof *t);
    t->generation = generation + 1;
    t->removed = true;
    artfVecPush(&s->freeTextures, texture);
    ++s->textureVersion;
    return true;
}

static size_t textureBytes(const ArtfTextureData* t, int fromMip) {
    size_t total = 0;
    int w = t->width, h = t->height;
    for (int level = 0; level < t->mipCount; ++level) {
        if (level >= fromMip) total += t->format ? (size_t)((w + 3) / 4) * ((h + 3) / 4) * artfTextureBlockBytes(t->format) : (size_t)w * h * 4;
        w = artfMaxi(w / 2, 1);
        h = artfMaxi(h / 2, 1);
    }
    return total;
}

void artfSceneMemory(const ArtfScene* s, ArtfSceneMemory* out) {
    memset(out, 0, sizeof *out);
    for (size_t i = 0; i < s->meshes.len; ++i) {
        const ArtfMeshData* m = &s->meshes.items[i];
        out->meshes += (size_t)m->vertexCount * sizeof(ArtfPackedVertex) + (size_t)m->indexCount * 4 + (size_t)m->positionStride * 16 + (m->colors ? (size_t)m->vertexCount * 4 : 0);
    }
    for (size_t i = 1; i < s->textures.len; ++i) {
        const ArtfTextureData* t = &s->textures.items[i];
        if (t->removed) continue;
        out->textures += textureBytes(t, 0);
        out->residentTextures += textureBytes(t, t->residentMip);
    }
}

void artfSceneSetResidency(ArtfScene* s, const ArtfResidencyDesc* desc) {
    memset(&s->residency, 0, sizeof s->residency);
    if (desc) s->residency = *desc;
    s->residencyWait = 0;
}

/* Memory for the mipmaps from this one down: what is there is kept, and the rest is asked for. */
static bool streamFrom(ArtfTextureData* t, int mip) {
    uint32_t offsets[16] = {0};
    size_t total = 0;
    for (int level = mip; level < t->mipCount; ++level) {
        offsets[level] = (uint32_t)total;
        total += (size_t)artfMaxi(t->width >> level, 1) * (size_t)artfMaxi(t->height >> level, 1);
    }
    uint32_t* pixels = artfMalloc(total * 4);
    for (int level = mip; level < t->mipCount; ++level) {
        const size_t size = (size_t)artfMaxi(t->width >> level, 1) * (size_t)artfMaxi(t->height >> level, 1);
        if (t->pixels && level >= t->residentMip) {
            memcpy(pixels + offsets[level], t->pixels + t->offsets[level], size * 4);
        } else if (!t->read(t->readUser, level, pixels + offsets[level])) {
            artfLog("artful: mipmap %d of a texture could not be read", level);
            artfFree(pixels);
            return false;
        }
    }
    artfFree(t->pixels);
    t->pixels = pixels;
    memcpy(t->offsets, offsets, sizeof offsets);
    return true;
}

ArtfTexture artfSceneAddTextureStreamed(ArtfScene* s, int width, int height, int mipCount, int residentMip, ArtfTextureReadFn read, void* user) {
    if (width <= 0 || height <= 0 || !read) return 0;
    ArtfTextureData t;
    memset(&t, 0, sizeof t);
    t.width = width;
    t.height = height;
    t.read = read;
    t.readUser = user;
    int full = 1;
    for (int side = artfMaxi(width, height); side > 1 && full < 16; side /= 2) ++full;
    t.mipCount = mipCount > 0 ? artfMini(mipCount, full) : full;
    t.residentMip = artfClampi(residentMip, 0, t.mipCount - 1);
    if (!streamFrom(&t, t.residentMip)) return 0;
    return artfSceneStoreTexture(s, &t);
}

void artfTextureSetResidentMip(ArtfScene* s, ArtfTexture texture, int mip) {
    if (texture >= s->textures.len || s->textures.items[texture].removed) return;
    ArtfTextureData* t = &s->textures.items[texture];
    const int clamped = artfClampi(mip, 0, t->mipCount - 1);
    if (t->residentMip == clamped) return;
    if (t->read && !streamFrom(t, clamped)) return;
    t->residentMip = clamped;
    ++s->textureVersion;
}

/* The mipmap a texture needs to show across this many pixels. */
static int mipFor(const ArtfTextureData* t, float pixels) {
    int mip = 0;
    while (mip + 1 < t->mipCount && (float)(artfMaxi(t->width, t->height) >> (mip + 1)) >= pixels) ++mip;
    return mip;
}

void artfSceneResidencyStep(ArtfScene* s, const ArtfFrameData* frame, int height) {
    if (s->residency.mode == ARTF_RESIDENCY_MANUAL) return;
    if (s->residencyWait-- > 0) return;
    s->residencyWait = (s->residency.interval > 0 ? s->residency.interval : 30) - 1;
    const uint32_t count = (uint32_t)s->textures.len;
    int* wanted = artfMalloc(((size_t)count + 1) * sizeof *wanted);
    for (uint32_t i = 0; i < count; ++i) wanted[i] = s->textures.items[i].mipCount - 1;
    for (size_t i = 0; i < s->instances.len; ++i) {
        const ArtfInstanceData* inst = &s->instances.items[i];
        const ArtfMeshData* mesh = &s->meshes.items[inst->mesh];
        const float* m = inst->m;
        const float* c = mesh->center;
        const float wx = m[0] * c[0] + m[1] * c[1] + m[2] * c[2] + m[3], wy = m[4] * c[0] + m[5] * c[1] + m[6] * c[2] + m[7], wz = m[8] * c[0] + m[9] * c[1] + m[10] * c[2] + m[11];
        const float r = mesh->radius * inst->maxScale;
        bool inView = true;
        for (int p = 0; p < 5 && inView; ++p) {
            const float* pl = frame->planes[p];
            inView = pl[0] * wx + pl[1] * wy + pl[2] * wz + pl[3] >= -r;
        }
        /* (what is behind the eye still shows in a mirror) */
        if (!inView && s->trace.desc.mode == ARTF_TRACE_NONE) continue;
        const float dx = wx - frame->cameraPos[0], dy = wy - frame->cameraPos[1], dz = wz - frame->cameraPos[2];
        const float distance = artfMaxf(sqrtf(dx * dx + dy * dy + dz * dz) - r, frame->nearW);
        const float pixels = 2.f * r * frame->focalY * (float)height * .5f / distance;
        const ArtfMaterialData* material = &s->materials.items[inst->material];
        const uint32_t maps[4] = {material->texture, material->normalMap, material->surfaceMap, material->occlusionMap};
        for (int k = 0; k < 4; ++k)
            if (maps[k] && maps[k] < count) wanted[maps[k]] = artfMini(wanted[maps[k]], mipFor(&s->textures.items[maps[k]], pixels));
        /* (what a material in C reads can be stretched as it likes: it keeps all its detail) */
        if (material->graph) {
            const ArtfGraphData* g = &s->graphs.items[material->graph - 1];
            for (uint32_t n = 0; n < g->count; ++n)
                if (g->nodes[n].op == ARTF_OP_TEXTURE && g->nodes[n].b && g->nodes[n].b < count) wanted[g->nodes[n].b] = 0;
        }
    }
    /* over the budget: the largest gives up a level, until it fits */
    if (s->residency.budget) {
        for (;;) {
            size_t total = 0, largest = 0;
            uint32_t chosen = 0;
            for (uint32_t i = 1; i < count; ++i) {
                const ArtfTextureData* t = &s->textures.items[i];
                if (t->removed) continue;
                const size_t bytes = textureBytes(t, wanted[i]);
                total += bytes;
                if (bytes > largest && wanted[i] + 1 < t->mipCount) { largest = bytes; chosen = i; }
            }
            if (total <= s->residency.budget || !chosen) break;
            ++wanted[chosen];
        }
    }
    for (uint32_t i = 1; i < count; ++i) {
        if (s->textures.items[i].removed || s->textures.items[i].residentMip == wanted[i]) continue;
        if (s->residency.mode == ARTF_RESIDENCY_AUTOMATIC) artfTextureSetResidentMip(s, i, wanted[i]);
        else if (s->residency.report) s->residency.report(s->residency.user, i, wanted[i]);
    }
    artfFree(wanted);
}

static ArtfMaterialData materialData(const ArtfScene* s, const ArtfMaterialDesc* d) {
    const bool mask = d->alphaMode == ARTF_ALPHA_MASK, blend = d->alphaMode == ARTF_ALPHA_BLEND;
    const uint32_t count = (uint32_t)s->textures.len;
    ArtfMaterialData m = {{d->baseColor.x, d->baseColor.y, d->baseColor.z}, artfClampf(d->specular, 0, 1),
                          {d->emissive.x, d->emissive.y, d->emissive.z}, artfClampf(d->roughness, .045f, 1.f),
                          d->texture < s->textures.len ? d->texture : 0,
                          blend ? 1.f - artfClampf(d->transparency, 0, 1) : 1.f,
                          mask ? (d->alphaCutoff > 0 ? d->alphaCutoff : .5f) : 0.f,
                          (mask ? ARTF_MATERIAL_MASK : 0u) | (blend ? ARTF_MATERIAL_BLEND : 0u) | (d->doubleSided ? ARTF_MATERIAL_DOUBLE : 0u),
                          d->ior > 0 ? d->ior : 0.f, artfClampf(d->metallic, 0, 1), d->normalScale > 0 ? d->normalScale : 1.f, d->occlusionStrength > 0 ? d->occlusionStrength : 1.f,
                          d->normalMap < count ? d->normalMap : 0, d->surfaceMap < count ? d->surfaceMap : 0, d->occlusionMap < count ? d->occlusionMap : 0,
                          d->graph <= s->graphs.len ? d->graph : 0, {{0}}};
    return m;
}

/* What an instance is, from its opacity, its mesh and its material. */
static void refreshInstance(ArtfScene* s, ArtfInstanceData* d) {
    const uint32_t material = s->materials.items[d->material].flags;
    const bool graph = s->materials.items[d->material].graph != 0;
    s->translucentCount -= d->translucent;
    s->cutoutCount -= (d->flags & ARTF_INSTANCE_CUTOUT) != 0;
    d->translucent = d->opacity != 65535 || s->meshes.items[d->mesh].hasAlpha || material & ARTF_MATERIAL_BLEND;
    d->flags = (uint8_t)((d->flags & ~ARTF_INSTANCE_CUTOUT) | (graph || material & (ARTF_MATERIAL_MASK | ARTF_MATERIAL_DOUBLE) ? ARTF_INSTANCE_CUTOUT : 0));
    s->translucentCount += d->translucent;
    s->cutoutCount += (d->flags & ARTF_INSTANCE_CUTOUT) != 0;
}

ArtfMaterialDesc artfMaterialDefaults(void) {
    ArtfMaterialDesc d;
    memset(&d, 0, sizeof d);
    d.baseColor = artfVec3(1, 1, 1);
    d.specular = 1.f;
    d.roughness = .5f;
    return d;
}

ArtfMaterial artfSceneAddMaterial(ArtfScene* s, const ArtfMaterialDesc* desc) {
    ArtfMaterialData m = materialData(s, desc);
    artfVecPush(&s->materials, m);
    ++s->materialVersion;
    return (ArtfMaterial)(s->materials.len - 1);
}

void artfMaterialSet(ArtfScene* s, ArtfMaterial m, const ArtfMaterialDesc* desc) {
    if (m >= s->materials.len) return;
    const ArtfMaterialData old = s->materials.items[m];
    s->materials.items[m] = materialData(s, desc);
    memcpy(s->materials.items[m].params, old.params, sizeof old.params);
    ++s->materialVersion;
    if (old.flags == s->materials.items[m].flags && old.graph == s->materials.items[m].graph) return;
    for (size_t i = 0; i < s->instances.len; ++i)
        if (s->instances.items[i].material == m) refreshInstance(s, &s->instances.items[i]);
    ++s->instanceVersion;
}

void artfMaterialSetParam(ArtfScene* s, ArtfMaterial m, uint32_t index, float x, float y, float z, float w) {
    if (m >= s->materials.len || index > 3) return;
    const float v[4] = {x, y, z, w};
    memcpy(s->materials.items[m].params[index], v, sizeof v);
    ++s->materialVersion;
}

/* ---- instances ---- */

#define HANDLE_NUMBER(i) ((i) & 0xFFFFFFu)
#define HANDLE_GENERATION(i) ((i) >> 24)

/* What a handle stands for; NULL, and said so, when it stands for nothing. */
static ArtfInstanceData* instanceOf(ArtfScene* s, ArtfInstance i) {
    const uint32_t number = HANDLE_NUMBER(i);
    if (number && number <= s->handleSlots.len && s->handleGenerations.items[number - 1] == HANDLE_GENERATION(i)) return &s->instances.items[s->handleSlots.items[number - 1]];
    artfLog("artful: instance %u is not there", i);
    return NULL;
}

/* Where an instance is, for maps to know what they can keep. */
static void noteChange(ArtfScene* s, const ArtfInstanceData* d) {
    const ArtfMeshData* mesh = &s->meshes.items[d->mesh];
    const float* m = d->m;
    const float* c = mesh->center;
    float* out = s->shadowChanges[s->shadowChangeCount++ % ARTF_SHADOW_CHANGES];
    for (int k = 0; k < 3; ++k) out[k] = m[k * 4] * c[0] + m[k * 4 + 1] * c[1] + m[k * 4 + 2] * c[2] + m[k * 4 + 3];
    out[3] = mesh->radius * d->maxScale;
}

static void setTransform(ArtfInstanceData* d, const ArtfTransform* t) {
    memcpy(d->m, t->m, sizeof d->m);
    float sx = d->m[0] * d->m[0] + d->m[4] * d->m[4] + d->m[8] * d->m[8];
    float sy = d->m[1] * d->m[1] + d->m[5] * d->m[5] + d->m[9] * d->m[9];
    float sz = d->m[2] * d->m[2] + d->m[6] * d->m[6] + d->m[10] * d->m[10];
    float lo = artfMinf(sx, artfMinf(sy, sz)), hi = artfMaxf(sx, artfMaxf(sy, sz));
    d->maxScale = sqrtf(hi);
    d->flags = (uint8_t)((d->flags & ~ARTF_INSTANCE_UNIFORM) | (hi - lo <= hi * .001f ? ARTF_INSTANCE_UNIFORM : 0));
}

ArtfInstance artfSceneAddInstance(ArtfScene* s, ArtfMesh mesh, ArtfMaterial material, const ArtfTransform* transform) {
    if (!mesh || mesh > s->meshes.len || s->meshes.items[mesh - 1].removed) return 0;
    ArtfInstanceData d;
    memset(&d, 0, sizeof d);
    d.mesh = mesh - 1;
    if (s->meshes.items[d.mesh].skin) {
        /* each instance bends alone: it has a copy of its own, that all that draws sees as any other mesh */
        ArtfMeshData copy;
        artfMeshCopy(&copy, &s->meshes.items[d.mesh]);
        d.mesh = meshPlace(s, &copy);
        artfVecPush(&s->deforming, d.mesh);
    }
    d.material = material < s->materials.len ? material : 0;
    d.opacity = 65535;
    refreshInstance(s, &d);
    ArtfTransform identity = artfTransformIdentity();
    setTransform(&d, transform ? transform : &identity);

    uint32_t handle = s->freeHandle;
    if (handle) {
        s->freeHandle = s->handleSlots.items[handle - 1];
    } else {
        artfVecPush(&s->handleSlots, 0);
        artfVecPush(&s->handleGenerations, 0);
        handle = (uint32_t)s->handleSlots.len;
    }
    s->handleSlots.items[handle - 1] = (uint32_t)s->instances.len;
    noteChange(s, &d);
    artfVecPush(&s->instances, d);
    artfVecPush(&s->instanceHandles, handle);
    ++s->structureVersion;
    ++s->instanceVersion;
    return (uint32_t)s->handleGenerations.items[handle - 1] << 24 | handle;
}

void artfSceneRemoveInstance(ArtfScene* s, ArtfInstance i) {
    if (!instanceOf(s, i)) return;
    i = HANDLE_NUMBER(i);
    uint32_t slot = s->handleSlots.items[i - 1], last = (uint32_t)s->instances.len - 1;
    s->translucentCount -= s->instances.items[slot].translucent;
    s->cutoutCount -= (s->instances.items[slot].flags & ARTF_INSTANCE_CUTOUT) != 0;
    noteChange(s, &s->instances.items[slot]);
    /* (a mesh that bends is this instance's own) */
    const uint32_t own = s->instances.items[slot].mesh;
    if (s->meshes.items[own].skin && !s->meshes.items[own].ownsSkin) {
        forgetMesh(s, own);
        for (size_t k = 0; k < s->deforming.len; ++k)
            if (s->deforming.items[k] == own) s->deforming.items[k] = s->deforming.items[--s->deforming.len];
    }
    s->instances.items[slot] = s->instances.items[last];
    s->instanceHandles.items[slot] = s->instanceHandles.items[last];
    s->handleSlots.items[s->instanceHandles.items[slot] - 1] = slot;
    --s->instances.len;
    --s->instanceHandles.len;
    s->handleSlots.items[i - 1] = s->freeHandle;
    ++s->handleGenerations.items[i - 1];
    s->freeHandle = i;
    ++s->structureVersion;
    ++s->instanceVersion;
}

void artfInstanceSetTransform(ArtfScene* s, ArtfInstance i, const ArtfTransform* transform) {
    ArtfInstanceData* d = instanceOf(s, i);
    if (!d) return;
    noteChange(s, d);
    setTransform(d, transform);
    noteChange(s, d);
    ++s->instanceVersion;
}

void artfInstanceSetMaterial(ArtfScene* s, ArtfInstance i, ArtfMaterial material) {
    ArtfInstanceData* d = instanceOf(s, i);
    if (!d) return;
    d->material = material < s->materials.len ? material : 0;
    refreshInstance(s, d);
    noteChange(s, d);
    ++s->instanceVersion;
}

void artfInstanceSetOpacity(ArtfScene* s, ArtfInstance i, float opacity) {
    ArtfInstanceData* d = instanceOf(s, i);
    if (!d) return;
    d->opacity = (uint16_t)(artfClampf(opacity, 0, 1) * 65535.f + .5f);
    refreshInstance(s, d);
    noteChange(s, d);
    ++s->instanceVersion;
}

/* Vertices follow when something reads them. A box of another size is written to the GPU's table with the rest. */
static void posed(ArtfScene* s, ArtfInstanceData* d, ArtfMeshData* m) {
    noteChange(s, d);
    if (artfMeshPoseFit(m)) ++s->lodVersion;
    noteChange(s, d);
    ++m->deformVersion;
    ++s->instanceVersion;
}

void artfInstanceSetJoints(ArtfScene* s, ArtfInstance i, const ArtfTransform* joints, uint32_t count) {
    ArtfInstanceData* d = instanceOf(s, i);
    if (!d) return;
    ArtfMeshData* m = &s->meshes.items[d->mesh];
    if (!m->skin) {
        artfLog("artful: instance %u has no skin", i);
        return;
    }
    artfFree(m->pose.joints);
    m->pose.joints = artfMalloc(((size_t)count + 1) * 48);
    if (count) memcpy(m->pose.joints, joints->m, (size_t)count * 48);
    m->pose.jointCount = count;
    posed(s, d, m);
}

void artfInstanceSetMorphWeights(ArtfScene* s, ArtfInstance i, const float* weights, uint32_t count) {
    ArtfInstanceData* d = instanceOf(s, i);
    if (!d) return;
    ArtfMeshData* m = &s->meshes.items[d->mesh];
    if (!m->skin || !m->skin->morphCount) {
        artfLog("artful: instance %u has no targets", i);
        return;
    }
    if (!m->pose.weights) m->pose.weights = artfCalloc(m->skin->morphCount, 4);
    memcpy(m->pose.weights, weights, (size_t)artfMini((int)count, (int)m->skin->morphCount) * 4);
    posed(s, d, m);
}

void artfScenePosesApply(ArtfScene* s) {
    for (size_t i = 0; i < s->deforming.len; ++i) {
        ArtfMeshData* m = &s->meshes.items[s->deforming.items[i]];
        if (m->pose.applied != m->deformVersion) artfMeshDeform(m);
    }
}

uint32_t artfSceneInstanceCount(const ArtfScene* s) { return (uint32_t)s->instances.len; }

bool artfSceneBounds(const ArtfScene* s, ArtfVec3* center, float* radius) {
    if (!s->instances.len) return false;
    ArtfVec3 lo = artfVec3(3e38f, 3e38f, 3e38f), hi = artfVec3(-3e38f, -3e38f, -3e38f);
    for (size_t i = 0; i < s->instances.len; ++i) {
        const ArtfInstanceData* d = &s->instances.items[i];
        const ArtfMeshData* mesh = &s->meshes.items[d->mesh];
        const float* m = d->m;
        const float* c = mesh->center;
        const float r = mesh->radius * d->maxScale;
        const float w[3] = {m[0] * c[0] + m[1] * c[1] + m[2] * c[2] + m[3], m[4] * c[0] + m[5] * c[1] + m[6] * c[2] + m[7], m[8] * c[0] + m[9] * c[1] + m[10] * c[2] + m[11]};
        lo = artfVec3(artfMinf(lo.x, w[0] - r), artfMinf(lo.y, w[1] - r), artfMinf(lo.z, w[2] - r));
        hi = artfVec3(artfMaxf(hi.x, w[0] + r), artfMaxf(hi.y, w[1] + r), artfMaxf(hi.z, w[2] + r));
    }
    *center = artfVec3Scale(artfVec3Add(lo, hi), .5f);
    *radius = artfMaxf(artfVec3Length(artfVec3Sub(hi, lo)) * .5f, 1e-6f);
    return true;
}

static int fartherFirst(const void* a, const void* b) {
    const ArtfLayer *x = a, *y = b;
    if (x->distance != y->distance) return x->distance > y->distance ? -1 : 1;
    return x->instance < y->instance ? -1 : x->instance > y->instance;
}

uint32_t artfSceneLayers(const ArtfScene* s, const ArtfFrameData* frame, ArtfLayer* out) {
    uint32_t n = 0;
    if (!s->translucentCount) return 0;
    for (size_t i = 0; i < s->instances.len; ++i) {
        const ArtfInstanceData* inst = &s->instances.items[i];
        if (!inst->translucent || !inst->opacity) continue;
        const ArtfMeshData* mesh = &s->meshes.items[inst->mesh];
        const float* m = inst->m;
        const float* c = mesh->center;
        const float wx = m[0] * c[0] + m[1] * c[1] + m[2] * c[2] + m[3];
        const float wy = m[4] * c[0] + m[5] * c[1] + m[6] * c[2] + m[7];
        const float wz = m[8] * c[0] + m[9] * c[1] + m[10] * c[2] + m[11];
        const float r = mesh->radius * inst->maxScale;
        bool inView = true;
        for (int p = 0; p < 5 && inView; ++p) {
            const float* pl = frame->planes[p];
            inView = pl[0] * wx + pl[1] * wy + pl[2] * wz + pl[3] >= -r;
        }
        if (!inView) continue;
        const float dx = wx - frame->cameraPos[0], dy = wy - frame->cameraPos[1], dz = wz - frame->cameraPos[2];
        const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
        const ArtfLayer layer = {(uint32_t)i, artfMeshLevel(s, inst->mesh, frame->lodScale, inst->maxScale, artfMaxf(distance - r, frame->nearW)), distance};
        out[n++] = layer;
    }
    qsort(out, n, sizeof *out, fartherFirst);
    return n;
}

/* ---- light and the view ---- */

void artfSceneSetSun(ArtfScene* s, ArtfVec3 direction, ArtfVec3 color) {
    s->toSun = artfVec3Normalize(artfVec3Scale(direction, -1.f));
    s->sunColor = color;
}

void artfSceneSetAmbient(ArtfScene* s, ArtfVec3 sky, ArtfVec3 ground) {
    s->sky = sky;
    s->ground = ground;
}

ArtfLight artfSceneAddLight(ArtfScene* s, ArtfVec3 position, ArtfVec3 color, float radius) {
    for (uint32_t i = 0; i < ARTF_MAX_LIGHTS; ++i)
        if (!s->lightOn[i]) {
            s->lightOn[i] = true;
            artfLightSet(s, i + 1, position, color, radius);
            return i + 1;
        }
    return 0;
}

void artfLightSet(ArtfScene* s, ArtfLight l, ArtfVec3 position, ArtfVec3 color, float radius) {
    if (!l || l > ARTF_MAX_LIGHTS) return;
    ArtfLightData d = {{position.x, position.y, position.z}, artfMaxf(radius, 1e-3f), {color.x, color.y, color.z}, 1.f, {0, 0, 0}, -1.f};
    s->lights[l - 1] = d;
}

void artfLightSetSpot(ArtfScene* s, ArtfLight l, ArtfVec3 direction, float inner, float outer) {
    if (!l || l > ARTF_MAX_LIGHTS) return;
    ArtfLightData* d = &s->lights[l - 1];
    const ArtfVec3 n = artfVec3Normalize(direction);
    const float in = cosf(inner), out = cosf(artfMaxf(outer, inner + 1e-3f));
    memcpy(d->direction, &n, sizeof d->direction);
    d->spotCut = out;
    d->spotScale = 1.f / (in - out);
}

void artfSceneRemoveLight(ArtfScene* s, ArtfLight l) {
    if (l && l <= ARTF_MAX_LIGHTS) s->lightOn[l - 1] = false;
}

void artfSceneSetCamera(ArtfScene* s, const ArtfCamera* camera) { s->camera = *camera; }
void artfSceneSetBackground(ArtfScene* s, ArtfVec3 color) { s->background = color; }
void artfSceneSetShadows(ArtfScene* s, const ArtfShadowDesc* desc) {
    memset(&s->shadows, 0, sizeof s->shadows);
    if (!desc || desc->resolution <= 0) return;
    s->shadows.resolution = artfClampi((desc->resolution + 63) & ~63, 64, 4096);
    s->shadows.distance = desc->distance > 0 ? desc->distance : 60.f;
}

void artfLightSetShadow(ArtfScene* s, ArtfLight l, bool casts) {
    if (l && l <= ARTF_MAX_LIGHTS) s->lightCasts[l - 1] = casts;
}

/* From a place looking along forward, with up as near as it can be: the first three rows of a view. */
static void lookAlong(ArtfVec3 eye, ArtfVec3 forward, float* right, float* up, float* offsets) {
    const ArtfVec3 f = artfVec3Normalize(forward);
    const ArtfVec3 hint = fabsf(f.y) > .9f ? artfVec3(1, 0, 0) : artfVec3(0, 1, 0);
    const ArtfVec3 r = artfVec3Normalize(artfVec3Cross(hint, f)), u = artfVec3Cross(f, r);
    memcpy(right, &r, 12);
    memcpy(up, &u, 12);
    offsets[0] = artfVec3Dot(r, eye);
    offsets[1] = artfVec3Dot(u, eye);
    offsets[2] = artfVec3Dot(f, eye);
}

/* A lamp's view: tanHalf is the tangent of half its field. */
static void lampView(ArtfShadowView* v, ArtfVec3 position, ArtfVec3 forward, float tanHalf, float reach, int side) {
    const ArtfVec3 f = artfVec3Normalize(forward);
    float r[3], u[3], o[3];
    lookAlong(position, f, r, u, o);
    const float near_ = reach * .01f, scale = 1.f / tanHalf, a = reach / (reach - near_), b = -near_ * a;
    float* m = v->m;
    for (int k = 0; k < 3; ++k) {
        m[k] = r[k] * scale;
        m[4 + k] = u[k] * scale;
        m[8 + k] = (&f.x)[k] * a;
        m[12 + k] = (&f.x)[k];
    }
    m[3] = -o[0] * scale;
    m[7] = -o[1] * scale;
    m[15] = -o[2];
    m[11] = -o[2] * a + b;
    v->info[2] = 2.f * tanHalf / (float)side;
    v->info[3] = 1e-4f;
    v->place[0] = position.x;
    v->place[1] = position.y;
    v->place[2] = position.z;
    v->place[3] = reach;
}

uint64_t artfShadowViewKey(const ArtfScene* s, const ArtfShadowView* view, uint32_t side) {
    const uint64_t versions[5] = {s->id, s->materialVersion, s->meshRemovals, s->textureVersion, side};
    uint64_t h = 1469598103934665603ull;
    const uint8_t* parts[2] = {(const uint8_t*)versions, (const uint8_t*)view};
    const size_t sizes[2] = {sizeof versions, sizeof *view};
    for (int p = 0; p < 2; ++p)
        for (size_t i = 0; i < sizes[p]; ++i) h = (h ^ parts[p][i]) * 1099511628211ull;
    return h | 1; /* (0: nothing there yet) */
}

/* The test in geometryJob (shadows.c) and in the GPU's shadowCull. */
bool artfShadowViewTouched(const ArtfScene* s, const ArtfShadowView* view, uint32_t side, uint64_t seen) {
    const uint64_t count = s->shadowChangeCount;
    if (count - seen > ARTF_SHADOW_CHANGES) return true;
    const float* m = view->m;
    const bool sun = view->place[3] <= 0;
    const float half = view->info[2] * (float)side * .5f;
    for (uint64_t i = seen; i < count; ++i) {
        const float* c = s->shadowChanges[i % ARTF_SHADOW_CHANGES];
        if (sun) {
            const float x = m[0] * c[0] + m[1] * c[1] + m[2] * c[2] + m[3], y = m[4] * c[0] + m[5] * c[1] + m[6] * c[2] + m[7];
            if (fabsf(x) <= 1.f + c[3] / half && fabsf(y) <= 1.f + c[3] / half) return true;
        } else {
            const float dx = c[0] - view->place[0], dy = c[1] - view->place[1], dz = c[2] - view->place[2], reach = view->place[3] + c[3];
            if (dx * dx + dy * dy + dz * dz <= reach * reach) return true;
        }
    }
    return false;
}

uint32_t artfSceneShadowFrame(const ArtfScene* s, const ArtfFrameData* frame, ArtfShadowFrame* out) {
    memset(out, 0, sizeof *out);
    const int side = s->shadows.resolution;
    if (!side || !s->instances.len) return 0;
    out->sun[2] = (uint32_t)side;
    uint32_t views = 0;

    if (s->sunColor.x > 0 || s->sunColor.y > 0 || s->sunColor.z > 0) {
        /* Each map holds a ball around a stretch of the view, and moves by whole texels, so that shadows do not
         * shimmer as the eye moves. Depth reaches all that can cast into it. */
        ArtfVec3 middle;
        float sceneRadius;
        artfSceneBounds(s, &middle, &sceneRadius);
        const ArtfCamera* c = &s->camera;
        const ArtfVec3 forward = artfVec3Normalize(artfVec3Sub(c->target, c->position));
        const float tanX = 1.f / frame->focalX, tanY = 1.f / frame->focalY, spread = sqrtf(1.f + tanX * tanX + tanY * tanY);
        const float ends[ARTF_SHADOW_CASCADES] = {s->shadows.distance / 16.f, s->shadows.distance / 4.f, s->shadows.distance};
        const ArtfVec3 toward = s->toSun;
        float r[3], u[3], o[3];
        lookAlong(artfVec3(0, 0, 0), artfVec3Scale(toward, -1.f), r, u, o);
        const ArtfVec3 right = artfVec3(r[0], r[1], r[2]), upward = artfVec3(u[0], u[1], u[2]);
        out->sun[0] = 1;
        out->sun[1] = ARTF_SHADOW_CASCADES;
        float from = 0;
        for (uint32_t i = 0; i < ARTF_SHADOW_CASCADES; ++i) {
            const float to = ends[i], along = (from + to) * .5f, wide = to * spread * sqrtf(1.f - 1.f / (spread * spread));
            const float half = sqrtf((to - along) * (to - along) + wide * wide) * 1.02f, texel = 2.f * half / (float)side;
            out->splits[i] = to;
            const ArtfVec3 center = artfVec3Add(c->position, artfVec3Scale(forward, along));
            const float cx = floorf(artfVec3Dot(center, right) / texel) * texel, cy = floorf(artfVec3Dot(center, upward) / texel) * texel;
            const float depthMiddle = artfVec3Dot(center, toward);
            const float top = artfMaxf(artfVec3Dot(middle, toward) + sceneRadius, depthMiddle + half), bottom = depthMiddle - half, range = top - bottom;
            ArtfShadowView* v = &out->views[views++];
            float* m = v->m;
            const float scale = 1.f / half;
            memcpy(m, &right, 12);
            memcpy(m + 4, &upward, 12);
            for (int k = 0; k < 3; ++k) {
                m[k] *= scale;
                m[4 + k] *= scale;
                m[8 + k] = -(&toward.x)[k] / range;
            }
            m[3] = -cx * scale;
            m[7] = -cy * scale;
            m[11] = top / range;
            m[15] = 1.f;
            v->info[2] = texel;
            v->info[3] = 2.f * texel / range;
            memcpy(v->place, &toward, 12);
            from = to;
        }
    }

    uint32_t frameLight = 0;
    for (uint32_t i = 0; i < ARTF_MAX_LIGHTS; ++i) {
        if (!s->lightOn[i]) continue;
        const uint32_t index = frameLight++;
        if (!s->lightCasts[i]) continue;
        const ArtfLightData* L = &s->lights[i];
        const ArtfVec3 position = artfVec3(L->position[0], L->position[1], L->position[2]);
        const bool spot = L->direction[0] != 0 || L->direction[1] != 0 || L->direction[2] != 0;
        const uint32_t need = spot ? 1 : 6;
        if (views + need > ARTF_SHADOW_VIEWS) {
            artfLog("artful: no room for the shadows of light %u", i + 1);
            continue;
        }
        out->lights[index][0] = (views + 1) | need << 8;
        if (spot) {
            const float cut = artfClampf(L->spotCut, .05f, .999f);
            lampView(&out->views[views++], position, artfVec3(L->direction[0], L->direction[1], L->direction[2]), sqrtf(1.f - cut * cut) / cut * 1.02f, L->radius, side);
        } else {
            static const float faces[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            for (int f = 0; f < 6; ++f) lampView(&out->views[views++], position, artfVec3(faces[f][0], faces[f][1], faces[f][2]), 1.01f, L->radius, side);
        }
    }
    for (uint32_t i = 0; i < views; ++i) {
        out->views[i].info[0] = (float)((i % ARTF_SHADOW_ATLAS_COLUMNS) * (uint32_t)side);
        out->views[i].info[1] = (float)((i / ARTF_SHADOW_ATLAS_COLUMNS) * (uint32_t)side);
    }
    return views;
}

void artfSceneSetBackgroundAlpha(ArtfScene* s, float alpha) { s->backgroundAlpha = artfClampf(alpha, 0, 1); }
void artfSceneSetExposure(ArtfScene* s, float exposure) { s->exposure = artfMaxf(exposure, 0); }
void artfSceneSetSunSize(ArtfScene* s, float size) { s->sunSize = artfMaxf(size, 0); }
void artfSceneSetFog(ArtfScene* s, ArtfVec3 color, float density) {
    s->fogColor = color;
    s->fogDensity = artfMaxf(density, 0);
}

void artfSceneFrame(const ArtfScene* s, int width, int height, ArtfFrameData* out) {
    memset(out, 0, sizeof *out);
    const ArtfCamera* c = &s->camera;
    ArtfMat4 view = artfMat4LookAt(c->position, c->target, c->up);
    ArtfMat4 proj = artfMat4Perspective(c->fovY, (float)width / (float)artfMaxi(height, 1), c->near_);
    ArtfMat4 vp = artfMat4Mul(proj, view);
    memcpy(out->viewProj, vp.m, sizeof vp.m);

    /* from the rows of the matrix: x + w >= 0, w - x >= 0, y + w >= 0, w - y >= 0, w - z >= 0 */
    static const float signs[5][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {0, 0}};
    for (int i = 0; i < 5; ++i) {
        float p[4], len = 0;
        for (int k = 0; k < 4; ++k) {
            const float* col = &vp.m[k * 4];
            p[k] = col[3] + signs[i][0] * col[0] + signs[i][1] * col[1];
            if (i == 4) p[k] = col[3] - col[2];
        }
        len = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        for (int k = 0; k < 4; ++k) out->planes[i][k] = p[k] / len;
    }
    out->cameraPos[0] = c->position.x;
    out->cameraPos[1] = c->position.y;
    out->cameraPos[2] = c->position.z;
#define SET3(a, v) (a)[0] = (v).x, (a)[1] = (v).y, (a)[2] = (v).z
    SET3(out->toSun, s->toSun);
    SET3(out->sunColor, s->sunColor);
    SET3(out->sky, s->sky);
    SET3(out->ground, s->ground);
    /* (premultiplied, as the frame is) */
    const ArtfVec3 background = artfVec3Scale(s->background, s->backgroundAlpha);
    SET3(out->background, background);
    out->background[3] = s->backgroundAlpha;
    SET3(out->fog, s->fogColor);
    out->look[0] = s->exposure;
    out->look[1] = s->fogDensity;
    out->look[2] = s->sunSize;
#undef SET3
    out->instanceCount = (uint32_t)s->instances.len;
    out->focalY = proj.m[5];
    out->focalX = proj.m[0];
    out->nearW = c->near_;
    out->meshCount = (uint32_t)s->meshes.len;
    out->lodScale = s->features[ARTF_FEATURE_LOD] ? (float)height * .5f * proj.m[5] / s->lodError : 0.f;
    out->look[3] = s->features[ARTF_FEATURE_LOD_FADE] ? ARTF_LOD_FADE : 0.f;
    out->fog[3] = (float)width;
    for (uint32_t i = 0; i < ARTF_MAX_LIGHTS; ++i)
        if (s->lightOn[i]) out->lights[out->lightCount++] = s->lights[i];
}
