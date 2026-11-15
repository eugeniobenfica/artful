/* artful — what makes a large world cheap, decided once a frame before any backend draws, so that all draw the same:
 *
 *   cubes of instances merged into one lighter mesh, drawn in their place past a distance (Unreal's HLOD);
 *   levels of detail for what bends, each instance taking a copy of the level its distance calls for;
 *   which levels of meshes are held, by distance and a budget, as textures' mipmaps are (residency).
 *
 * Nothing here is a backend's: an instance's opacity, its mesh, and which levels a mesh has are all the backends see,
 * and they see them as they see any other. */
#include "scene/sceneInternal.h"
#include "scene/simplifyInternal.h"
#include "core/threads.h"

#include <stdlib.h>

typedef struct ArtfScale ArtfScale;

typedef struct Hlod {
    ArtfInstance proxy;     /* the merged mesh's instance */
    ArtfMesh mesh;
    ArtfInstance* members;
    uint16_t* opacities;    /* each member's own, while the merged mesh stands for it */
    uint32_t memberCount;
    float center[3], radius;
    float distance, error;  /* distance 0: by the error */
    bool far;               /* the merged mesh shows */
} Hlod;

/* How to read a mesh again. */
typedef struct Stream {
    uint32_t mesh;
    uint32_t vertexCount, indexCount;
    ArtfMeshReadFn read;
    void* user;
} Stream;

struct ArtfScale {
    ARTF_VEC(Hlod) hlods;
    ARTF_VEC(uint8_t) held;  /* by an instance handle's number: in a merged mesh, or one */
    uint32_t shown;
    uint32_t skinned;        /* meshes that bend with levels: their instances choose */
    ARTF_VEC(Stream) streams;
    uint32_t partial;        /* meshes not all held */
    int meshWait;
};

static ArtfScale* scaleOf(ArtfScene* s) {
    if (!s->scale) s->scale = ARTF_NEW(ArtfScale);
    return s->scale;
}

void artfScaleFree(ArtfScale* g) {
    if (!g) return;
    for (size_t i = 0; i < g->hlods.len; ++i) {
        artfFree(g->hlods.items[i].members);
        artfFree(g->hlods.items[i].opacities);
    }
    artfVecFree(&g->hlods);
    artfVecFree(&g->held);
    artfVecFree(&g->streams);
    artfFree(g);
}

/* What a frame chooses by: how far its eye is from a ball, and the scale levels go by (as artfSceneFrame's). */
static float lodScaleOf(const ArtfScene* s, int width, int height) {
    if (!s->features[ARTF_FEATURE_LOD]) return 0;
    const ArtfMat4 proj = artfPerspective(s->camera.fovY, (float)width / (float)artfMaxi(height, 1), s->camera.near_);
    return (float)height * .5f * proj.m[5] / s->lodError;
}

static float distanceTo(const ArtfScene* s, const float* center, float radius) {
    const ArtfVec3 e = s->camera.position;
    const float dx = center[0] - e.x, dy = center[1] - e.y, dz = center[2] - e.z;
    return artfMaxf(sqrtf(dx * dx + dy * dy + dz * dz) - radius, s->camera.near_);
}

static void worldCenter(const ArtfInstanceData* d, const float* c, float* out) {
    const float* m = d->m;
    for (int k = 0; k < 3; ++k) out[k] = m[k * 4] * c[0] + m[k * 4 + 1] * c[1] + m[k * 4 + 2] * c[2] + m[k * 4 + 3];
}

/* ---- many as one ---- */

static bool isHeld(const ArtfScale* g, ArtfInstance i) {
    const uint32_t number = i & 0xFFFFFFu;
    return number < g->held.len && g->held.items[number];
}

static void hold(ArtfScale* g, ArtfInstance i, uint8_t on) {
    const uint32_t number = i & 0xFFFFFFu;
    while (g->held.len <= number) artfVecPush(&g->held, 0);
    g->held.items[number] = on;
}

/* What a member gives a merged mesh: its first lighter level, where it has one held (what is merged is seen from far),
 * and how far that is from the whole already, in the world. */
static uint32_t mergedLevel(const ArtfScene* s, const ArtfInstanceData* d, float* error) {
    const ArtfMeshData* m = &s->meshes.items[d->mesh];
    const uint32_t held = artfMeshHeld(s, d->mesh), first = m->residentFrom ? m->residentFrom : m->lodCount ? 1 : 0;
    *error = first ? m->lodErrors[first - 1] * d->maxScale : 0.f;
    return first ? m->lods[first - 1] : held;
}

/* What a merged mesh can take: what stays, is solid and is not merged already. */
static bool mergeable(const ArtfScene* s, const ArtfScale* g, size_t dense) {
    const ArtfInstanceData* d = &s->instances.items[dense];
    const ArtfMeshData* m = &s->meshes.items[artfMeshHeld(s, d->mesh)];
    float error;
    return !d->translucent && !(d->flags & ARTF_INSTANCE_PARTICLE) && !m->skin && s->meshes.items[mergedLevel(s, d, &error)].vertices && !isHeld(g, s->instanceHandles.items[dense]);
}

typedef struct Merge {
    const ArtfScene* scene;
    const uint32_t* dense;  /* the instances, by their places */
    uint32_t count;
    float keep;
    ArtfPreparedMesh* made;
    float error;
} Merge;

/* One cube's instances in the world, as one mesh, made lighter: touches no scene, so cubes go on all cores. */
static void mergeJob(void* user, uint32_t index, uint32_t worker) {
    (void)worker;
    Merge* j = &((Merge*)user)[index];
    const ArtfScene* s = j->scene;
    size_t vertexCount = 0, indexCount = 0;
    float before = 0;
    for (uint32_t i = 0; i < j->count; ++i) {
        float error;
        const ArtfMeshData* m = &s->meshes.items[mergedLevel(s, &s->instances.items[j->dense[i]], &error)];
        vertexCount += m->vertexCount;
        indexCount += m->indexCount;
        before = artfMaxf(before, error);
    }
    ArtfVertex* vertices = artfMalloc((vertexCount + 1) * sizeof *vertices);
    uint32_t* indices = artfMalloc((indexCount + 1) * 4);
    size_t vertexAt = 0, indexAt = 0;
    for (uint32_t i = 0; i < j->count; ++i) {
        const ArtfInstanceData* d = &s->instances.items[j->dense[i]];
        float error;
        const ArtfMeshData* m = &s->meshes.items[mergedLevel(s, d, &error)];
        ArtfVertex* v = vertices + vertexAt;
        artfMeshUnpack(m, v);
        const float* t = d->m;
        /* normals through the cofactors, so that what is stretched stays right; a mirror turns its triangles over */
        const ArtfVec3 c0 = artfVec3(t[0], t[4], t[8]), c1 = artfVec3(t[1], t[5], t[9]), c2 = artfVec3(t[2], t[6], t[10]);
        const ArtfVec3 k0 = artfVec3Cross(c1, c2), k1 = artfVec3Cross(c2, c0), k2 = artfVec3Cross(c0, c1);
        const bool mirrored = artfVec3Dot(c0, k0) < 0;
        for (uint32_t k = 0; k < m->vertexCount; ++k) {
            const float p[3] = {v[k].px, v[k].py, v[k].pz};
            v[k].px = t[0] * p[0] + t[1] * p[1] + t[2] * p[2] + t[3];
            v[k].py = t[4] * p[0] + t[5] * p[1] + t[6] * p[2] + t[7];
            v[k].pz = t[8] * p[0] + t[9] * p[1] + t[10] * p[2] + t[11];
            const ArtfVec3 n = artfVec3Normalize(artfVec3Add(artfVec3Add(artfVec3Scale(k0, v[k].nx), artfVec3Scale(k1, v[k].ny)), artfVec3Scale(k2, v[k].nz)));
            v[k].nx = n.x;
            v[k].ny = n.y;
            v[k].nz = n.z;
        }
        for (uint32_t k = 0; k < m->indexCount; k += 3) {
            indices[indexAt + k] = (uint32_t)vertexAt + m->indices[k];
            indices[indexAt + k + 1] = (uint32_t)vertexAt + m->indices[k + (mirrored ? 2 : 1)];
            indices[indexAt + k + 2] = (uint32_t)vertexAt + m->indices[k + (mirrored ? 1 : 2)];
        }
        vertexAt += m->vertexCount;
        indexAt += m->indexCount;
    }
    const uint32_t triangles = (uint32_t)(indexAt / 3);
    const uint32_t target = (uint32_t)artfMaxf((float)triangles * j->keep, 8.f);
    ArtfSimplified lighter;
    const bool simpler = target < triangles && artfSimplify(vertices, (uint32_t)vertexAt, indices, (uint32_t)indexAt, &target, 1, &lighter) == 1;
    j->error = before + (simpler ? lighter.error : 0.f);
    j->made = artfPrepareMesh(vertices, (uint32_t)vertexAt, simpler ? lighter.indices : indices, simpler ? lighter.indexCount : (uint32_t)indexAt, s->features[ARTF_FEATURE_LOD]);
    if (simpler) artfFree(lighter.indices);
    artfFree(vertices);
    artfFree(indices);
}

/* The merged meshes go into the scene, each in place of what it holds. */
static uint32_t takeMerges(ArtfScene* s, Merge* merges, uint32_t count, const ArtfHlodDesc* desc, ArtfInstance* last) {
    ArtfScale* g = scaleOf(s);
    uint32_t made = 0;
    for (uint32_t c = 0; c < count; ++c) {
        Merge* j = &merges[c];
        const ArtfMesh mesh = artfSceneAddPreparedMesh(s, j->made);
        if (!mesh) continue;
        const ArtfMaterial material = s->instances.items[j->dense[0]].material;
        Hlod h;
        memset(&h, 0, sizeof h);
        h.members = artfMalloc((size_t)j->count * sizeof *h.members);
        h.opacities = artfMalloc((size_t)j->count * sizeof *h.opacities);
        h.memberCount = j->count;
        for (uint32_t i = 0; i < j->count; ++i) h.members[i] = s->instanceHandles.items[j->dense[i]];
        h.proxy = artfSceneAddInstance(s, mesh, material, NULL);
        artfInstanceSetOpacity(s, h.proxy, 0);
        for (uint32_t i = 0; i < j->count; ++i) hold(g, h.members[i], 1);
        hold(g, h.proxy, 1);
        h.mesh = mesh;
        memcpy(h.center, s->meshes.items[mesh - 1].center, sizeof h.center);
        h.radius = s->meshes.items[mesh - 1].radius;
        h.distance = desc && desc->distance > 0 ? desc->distance : 0.f;
        h.error = j->error;
        artfVecPush(&g->hlods, h);
        if (last) *last = h.proxy;
        ++made;
    }
    return made;
}

/* Instances by their cube and material. */
typedef struct Placed { int32_t cell[3]; uint32_t material, dense; } Placed;

static int byCell(const void* a, const void* b) {
    const Placed *x = a, *y = b;
    for (int k = 0; k < 3; ++k)
        if (x->cell[k] != y->cell[k]) return x->cell[k] < y->cell[k] ? -1 : 1;
    if (x->material != y->material) return x->material < y->material ? -1 : 1;
    return x->dense < y->dense ? -1 : x->dense > y->dense;
}

static uint32_t runMerges(ArtfScene* s, Merge* merges, uint32_t count, const ArtfHlodDesc* desc, ArtfInstance* last) {
    if (!count) return 0;
    ArtfPool* pool = count > 1 ? artfPoolNew(0) : NULL;
    if (pool) artfPoolRun(pool, mergeJob, merges, count);
    else mergeJob(merges, 0, 0);
    artfPoolFree(pool);
    return takeMerges(s, merges, count, desc, last);
}

uint32_t artfSceneBuildHlod(ArtfScene* s, const ArtfHlodDesc* desc) {
    ArtfScale* g = scaleOf(s);
    const float cell = desc && desc->cell > 0 ? desc->cell : 32.f, keep = desc && desc->keep > 0 ? artfMinf(desc->keep, 1.f) : .125f;
    const uint32_t least = desc && desc->least > 0 ? desc->least : 2;
    Placed* placed = artfMalloc((s->instances.len + 1) * sizeof *placed);
    uint32_t n = 0;
    for (size_t i = 0; i < s->instances.len; ++i) {
        if (!mergeable(s, g, i)) continue;
        const ArtfInstanceData* d = &s->instances.items[i];
        float w[3];
        worldCenter(d, s->meshes.items[d->mesh].center, w);
        Placed p = {{(int32_t)floorf(w[0] / cell), (int32_t)floorf(w[1] / cell), (int32_t)floorf(w[2] / cell)}, d->material, (uint32_t)i};
        placed[n++] = p;
    }
    qsort(placed, n, sizeof *placed, byCell);
    uint32_t* dense = artfMalloc(((size_t)n + 1) * 4);
    for (uint32_t i = 0; i < n; ++i) dense[i] = placed[i].dense;
    ARTF_VEC(Merge) merges = {0};
    for (uint32_t i = 0; i < n;) {
        uint32_t end = i + 1;
        while (end < n && !memcmp(placed[end].cell, placed[i].cell, sizeof placed[i].cell) && placed[end].material == placed[i].material) ++end;
        if (end - i >= least) {
            const Merge m = {s, dense + i, end - i, keep, NULL, 0};
            artfVecPush(&merges, m);
        }
        i = end;
    }
    const uint32_t made = runMerges(s, merges.items, (uint32_t)merges.len, desc, NULL);
    artfVecFree(&merges);
    artfFree(dense);
    artfFree(placed);
    return made;
}

ArtfInstance artfSceneAddHlod(ArtfScene* s, const ArtfInstance* instances, uint32_t count, const ArtfHlodDesc* desc) {
    ArtfScale* g = scaleOf(s);
    uint32_t* dense = artfMalloc(((size_t)count + 1) * 4);
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const ArtfInstanceData* d = artfSceneInstance(s, instances[i]);
        if (!d) continue;
        const uint32_t at = (uint32_t)(d - s->instances.items);
        if (mergeable(s, g, at) && (!n || s->instances.items[dense[0]].material == d->material)) dense[n++] = at;
    }
    ArtfInstance proxy = 0;
    if (n) {
        Merge m = {s, dense, n, desc && desc->keep > 0 ? artfMinf(desc->keep, 1.f) : .125f, NULL, 0};
        runMerges(s, &m, 1, desc, &proxy);
    }
    artfFree(dense);
    return proxy;
}

/* A member as it was, or out of the way: its opacity is what says so to every backend. */
static void showMembers(ArtfScene* s, Hlod* h, bool show) {
    for (uint32_t i = 0; i < h->memberCount; ++i) {
        ArtfInstanceData* d = artfSceneInstance(s, h->members[i]);
        if (!d) continue;
        if (!show) h->opacities[i] = d->opacity;
        artfInstanceSetOpacity(s, h->members[i], show ? (float)h->opacities[i] / 65535.f : 0.f);
    }
    if (artfSceneInstance(s, h->proxy)) artfInstanceSetOpacity(s, h->proxy, show ? 0.f : 1.f);
    h->far = !show;
}

void artfSceneClearHlod(ArtfScene* s) {
    ArtfScale* g = s->scale;
    if (!g) return;
    for (size_t k = 0; k < g->hlods.len; ++k) {
        Hlod* h = &g->hlods.items[k];
        if (h->far) showMembers(s, h, true);
        if (artfSceneInstance(s, h->proxy)) artfSceneRemoveInstance(s, h->proxy);
        artfSceneRemoveMesh(s, h->mesh);
        artfFree(h->members);
        artfFree(h->opacities);
    }
    g->hlods.len = 0;
    g->held.len = 0;
    g->shown = 0;
}

uint32_t artfSceneHlodShown(const ArtfScene* s) { return s->scale ? s->scale->shown : 0; }

static void hlodStep(ArtfScene* s, ArtfScale* g, float lodScale, bool traced) {
    uint32_t shown = 0;
    for (size_t k = 0; k < g->hlods.len; ++k) {
        Hlod* h = &g->hlods.items[k];
        const float d = distanceTo(s, h->center, h->radius);
        const bool far = !traced && (h->distance > 0 ? d > h->distance : lodScale > 0 && h->error * lodScale <= d && d >= h->radius);
        if (far != h->far) showMembers(s, h, !far);
        shown += h->far;
    }
    g->shown = shown;
}

/* ---- levels of what bends ---- */

void artfSceneSkinLevels(ArtfScene* s, uint32_t index) {
    const ArtfMeshData* m = &s->meshes.items[index];
    const uint32_t triangles = m->indexCount / 3, n = m->vertexCount;
    if (!s->features[ARTF_FEATURE_LOD] || !m->skin || m->isLod || triangles < 128) return;
    const ArtfSkinData* skin = m->skin;
    /* the vertices as they were given: the rest's floats, not the packed ones */
    ArtfVertex* vertices = artfMalloc(((size_t)n + 1) * sizeof *vertices);
    artfMeshUnpack(m, vertices);
    for (uint32_t v = 0; v < n; ++v) {
        const float* r = skin->rest + (size_t)v * 6;
        vertices[v].px = r[0]; vertices[v].py = r[1]; vertices[v].pz = r[2];
        vertices[v].nx = r[3]; vertices[v].ny = r[4]; vertices[v].nz = r[5];
    }
    ArtfVertexSkin* skins = NULL;
    if (skin->joints) {
        skins = artfMalloc(((size_t)n + 1) * sizeof *skins);
        for (uint32_t v = 0; v < n; ++v) {
            memcpy(skins[v].joints, skin->joints + (size_t)v * 8, 16);
            memcpy(skins[v].weights, skin->weights + (size_t)v * 8, 32);
        }
    }
    const uint32_t morphCount = skin->morphCount;
    float* split = morphCount ? artfMalloc((size_t)morphCount * n * 24) : NULL;
    ArtfMorphTarget targets[64];
    const uint32_t targetCount = artfMini((int)morphCount, 64);
    for (uint32_t t = 0; t < targetCount; ++t) {
        float* positions = split + (size_t)t * n * 6;
        float* normals = positions + (size_t)n * 3;
        for (uint32_t v = 0; v < n; ++v) {
            memcpy(positions + (size_t)v * 3, skin->morphs + ((size_t)t * n + v) * 6, 12);
            memcpy(normals + (size_t)v * 3, skin->morphs + ((size_t)t * n + v) * 6 + 3, 12);
        }
        targets[t].positions = positions;
        targets[t].normals = normals;
    }
    const uint32_t wantedTargets[ARTF_MAX_LODS] = {triangles / 4, triangles / 16, triangles / 64};
    uint32_t wanted = 0;
    while (wanted < ARTF_MAX_LODS && wantedTargets[wanted] >= 8) ++wanted;
    ArtfSimplified levels[ARTF_MAX_LODS];
    const uint32_t count = artfSimplifySkinned(vertices, n, m->indices, m->indexCount, wantedTargets, wanted, levels, skins, targetCount ? skin->morphs : NULL, targetCount);
    uint32_t places[ARTF_MAX_LODS];
    float errors[ARTF_MAX_LODS];
    uint32_t made = 0;
    for (uint32_t i = 0; i < count; ++i) {
        ArtfMeshData level;
        if (artfMeshBuild(&level, vertices, n, levels[i].indices, levels[i].indexCount, skins, targetCount ? targets : NULL, targetCount)) {
            level.isLod = true;
            places[made] = artfSceneMeshPlace(s, &level);
            errors[made++] = levels[i].error;
        }
        artfFree(levels[i].indices);
    }
    ArtfMeshData* base = &s->meshes.items[index]; /* (the list may have moved) */
    memcpy(base->skinLods, places, (size_t)made * 4);
    memcpy(base->skinLodErrors, errors, (size_t)made * 4);
    base->skinLodCount = made;
    if (made) ++scaleOf(s)->skinned;
    artfFree(vertices);
    artfFree(skins);
    artfFree(split);
}

void artfSceneCopySkinLevels(ArtfScene* s, uint32_t source, uint32_t home) {
    const uint32_t count = s->meshes.items[source].skinLodCount;
    s->meshes.items[home].skinLodCount = 0;
    s->meshes.items[home].home = 0;
    if (!count) return;
    uint32_t copies[ARTF_MAX_LODS];
    for (uint32_t k = 0; k < count; ++k) {
        ArtfMeshData copy;
        artfMeshCopy(&copy, &s->meshes.items[s->meshes.items[source].skinLods[k]]);
        copy.home = home + 1;
        copies[k] = artfSceneMeshPlace(s, &copy);
        artfVecPush(&s->deforming, copies[k]);
    }
    ArtfMeshData* h = &s->meshes.items[home];
    h->home = home + 1;
    h->skinLodCount = count;
    memcpy(h->skinLods, copies, (size_t)count * 4);
    memcpy(h->skinLodErrors, s->meshes.items[source].skinLodErrors, (size_t)count * 4);
}

static void* copyOf(const void* from, size_t bytes) {
    if (!from) return NULL;
    void* to = artfMalloc(bytes + 1);
    memcpy(to, from, bytes);
    return to;
}

/* An instance takes another of its copies: what was asked of the pose goes with it, and is bent there. */
static void takeLevel(ArtfScene* s, ArtfInstanceData* d, uint32_t to) {
    const ArtfPose* from = &s->meshes.items[d->mesh].pose;
    ArtfMeshData* t = &s->meshes.items[to];
    artfFree(t->pose.joints);
    artfFree(t->pose.weights);
    t->pose.joints = copyOf(from->joints, (size_t)from->jointCount * 48);
    t->pose.jointCount = from->jointCount;
    t->pose.weights = copyOf(from->weights, (size_t)t->skin->morphCount * 4);
    if (artfMeshPoseFit(t)) ++s->lodVersion;
    ++t->deformVersion;
    artfSceneInstanceChanged(s, d);
    d->mesh = to;
    artfSceneInstanceChanged(s, d);
    ++s->structureVersion;
}

static void skinStep(ArtfScene* s, float lodScale, bool traced) {
    for (size_t i = 0; i < s->instances.len; ++i) {
        ArtfInstanceData* d = &s->instances.items[i];
        const ArtfMeshData* m = &s->meshes.items[d->mesh];
        if (!m->home) continue;
        const uint32_t homeIndex = m->home - 1;
        const ArtfMeshData* home = &s->meshes.items[homeIndex];
        /* (by the mesh at rest, whatever a pose did to the box: so that the choice does not go back and forth) */
        const float* lo = home->skin->restLow, *hi = home->skin->restHigh;
        const float c[3] = {(lo[0] + hi[0]) * .5f, (lo[1] + hi[1]) * .5f, (lo[2] + hi[2]) * .5f};
        const float r = .5f * sqrtf((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2])) * d->maxScale;
        float w[3];
        worldCenter(d, c, w);
        const float distance = distanceTo(s, w, r);
        uint32_t chosen = homeIndex;
        if (!traced && lodScale > 0)
            for (uint32_t k = 0; k < home->skinLodCount; ++k)
                if (home->skinLodErrors[k] * d->maxScale * lodScale <= distance) chosen = home->skinLods[k];
        if (chosen != d->mesh) takeLevel(s, d, chosen);
    }
}

void artfSceneSkinWholesPosed(ArtfScene* s) {
    if (!s->scale || !s->scale->skinned) return;
    for (size_t i = 0; i < s->instances.len; ++i) {
        const ArtfInstanceData* d = &s->instances.items[i];
        const ArtfMeshData* level = &s->meshes.items[d->mesh];
        if (!level->home || level->home - 1 == d->mesh) continue;
        ArtfMeshData* whole = &s->meshes.items[level->home - 1];
        if (whole->posedFrom == d->mesh + 1 && whole->posedAt == level->deformVersion) continue;
        const ArtfPose* from = &level->pose;
        artfFree(whole->pose.joints);
        artfFree(whole->pose.weights);
        whole->pose.joints = copyOf(from->joints, (size_t)from->jointCount * 48);
        whole->pose.jointCount = from->jointCount;
        whole->pose.weights = NULL;
        if (from->weights && whole->skin->morphCount) {
            /* (a level keeps at most 64 targets) */
            whole->pose.weights = artfCalloc(whole->skin->morphCount, 4);
            memcpy(whole->pose.weights, from->weights, (size_t)artfMini((int)level->skin->morphCount, (int)whole->skin->morphCount) * 4);
        }
        if (artfMeshPoseFit(whole)) ++s->lodVersion;
        ++whole->deformVersion;
        whole->posedFrom = d->mesh + 1;
        whole->posedAt = level->deformVersion;
    }
}

/* ---- which levels of meshes are held ---- */

static Stream* streamOf(ArtfScene* s, uint32_t mesh) {
    ArtfScale* g = s->scale;
    for (size_t i = 0; g && s->meshes.items[mesh].streamed && i < g->streams.len; ++i)
        if (g->streams.items[i].mesh == mesh) return &g->streams.items[i];
    return NULL;
}

/* What a mesh's triangles take, held. */
static size_t meshBytes(const ArtfMeshData* m) {
    return (size_t)m->vertexCount * (sizeof(ArtfPackedVertex) + (m->colors ? 4 : 0)) + (size_t)m->indexCount * 4;
}

/* A level's own data goes; what says where it is and how large stays, for choosing. */
static void dropData(ArtfMeshData* m) {
    artfFree(m->vertices);
    artfFree(m->indices);
    artfFree(m->meshlets);
    artfFree(m->meshletVertices);
    artfFree(m->meshletTriangles);
    artfFree(m->colors);
    artfAlignedFree(m->positions);
    m->vertices = NULL;
    m->indices = NULL;
    m->meshlets = NULL;
    m->meshletVertices = NULL;
    m->meshletTriangles = NULL;
    m->colors = NULL;
    m->positions = NULL;
    m->meshletCount = 0;
    m->positionStride = 0;
}

/* What was made goes into a level's place, keeping what the scene says of the place. */
static void takeData(ArtfMeshData* to, ArtfMeshData* from) {
    dropData(to);
    to->vertices = from->vertices;
    to->vertexCount = from->vertexCount;
    to->indices = from->indices;
    to->indexCount = from->indexCount;
    memcpy(to->scale, from->scale, sizeof to->scale);
    memcpy(to->offset, from->offset, sizeof to->offset);
    memcpy(to->center, from->center, sizeof to->center);
    to->radius = from->radius;
    to->meshlets = from->meshlets;
    to->meshletCount = from->meshletCount;
    to->meshletVertices = from->meshletVertices;
    to->meshletTriangles = from->meshletTriangles;
    to->positions = from->positions;
    to->positionStride = from->positionStride;
    to->colors = from->colors;
    to->hasAlpha = from->hasAlpha;
}

/* The whole of a streamed mesh and its levels, read again: made as they were made when it was added, so the same. */
static bool readAgain(ArtfScene* s, uint32_t base, const Stream* stream, bool* wanted) {
    ArtfVertex* vertices = artfMalloc(((size_t)stream->vertexCount + 1) * sizeof *vertices);
    uint32_t* indices = artfMalloc(((size_t)stream->indexCount + 1) * 4);
    bool ok = stream->read(stream->user, vertices, indices);
    ArtfMeshData whole;
    ok = ok && artfMeshBuild(&whole, vertices, stream->vertexCount, indices, stream->indexCount, NULL, NULL, 0);
    artfFree(vertices);
    artfFree(indices);
    if (!ok) {
        artfLog("artful: a mesh could not be read again");
        return false;
    }
    ArtfMeshData* m = &s->meshes.items[base];
    ArtfMeshData made[ARTF_MAX_LODS];
    float errors[ARTF_MAX_LODS];
    bool levels = false;
    for (uint32_t k = 0; k < m->lodCount; ++k) levels = levels || (wanted[k + 1] && !s->meshes.items[m->lods[k]].vertices);
    const uint32_t count = levels ? artfSceneMakeLevels(&whole, made, errors) : 0;
    for (uint32_t k = 0; k < count; ++k) {
        ArtfMeshData* level = &s->meshes.items[m->lods[k]];
        if (k < m->lodCount && wanted[k + 1] && !level->vertices) takeData(level, &made[k]);
        else artfMeshFini(&made[k]);
    }
    if (wanted[0] && !m->vertices) takeData(m, &whole);
    else artfMeshFini(&whole);
    return true;
}

void artfMeshSetResidentLevel(ArtfScene* s, ArtfMesh mesh, int level) {
    if (!mesh || mesh > s->meshes.len || s->meshes.items[mesh - 1].removed) return;
    const uint32_t base = mesh - 1;
    ArtfMeshData* m = &s->meshes.items[base];
    if (m->isLod || !m->lodCount) return;
    const uint32_t from = (uint32_t)artfClampi(level, 0, (int)m->lodCount);
    if (from == m->residentFrom) return;
    ArtfScale* g = scaleOf(s);
    const Stream* stream = streamOf(s, base);
    bool wanted[ARTF_MAX_LODS + 1];
    for (uint32_t k = 0; k <= m->lodCount; ++k) wanted[k] = k >= from;
    /* what is read again first: if it cannot be, what was there stays */
    if (stream && from < m->residentFrom && !readAgain(s, base, stream, wanted)) return;
    m = &s->meshes.items[base];
    for (uint32_t k = 0; k <= m->lodCount; ++k) {
        ArtfMeshData* level = &s->meshes.items[k ? m->lods[k - 1] : base];
        if (level->evicted == !wanted[k]) continue;
        level->evicted = !wanted[k];
        if (level->evicted && stream) dropData(level);
        ++level->generation; /* (what a GPU holds of it goes, or comes) */
    }
    g->partial += (m->residentFrom == 0) - (from == 0);
    m->residentFrom = from;
    ++s->meshRemovals;
    ++s->lodVersion;
    ++s->structureVersion;
}

int artfMeshResidentLevel(const ArtfScene* s, ArtfMesh mesh) {
    if (!mesh || mesh > s->meshes.len) return 0;
    return (int)s->meshes.items[mesh - 1].residentFrom;
}

ArtfMesh artfSceneAddMeshStreamed(ArtfScene* s, uint32_t vertexCount, uint32_t indexCount, ArtfMeshReadFn read, void* user) {
    if (!read || !vertexCount || indexCount < 3) return 0;
    ArtfVertex* vertices = artfMalloc(((size_t)vertexCount + 1) * sizeof *vertices);
    uint32_t* indices = artfMalloc(((size_t)indexCount + 1) * 4);
    ArtfMesh mesh = read(user, vertices, indices) ? artfSceneAddMesh(s, vertices, vertexCount, indices, indexCount) : 0;
    artfFree(vertices);
    artfFree(indices);
    if (!mesh) return 0;
    /* (a place used again: what read the mesh that was there goes) */
    ArtfScale* g = scaleOf(s);
    for (size_t i = 0; i < g->streams.len; ++i)
        if (g->streams.items[i].mesh == mesh - 1) g->streams.items[i--] = g->streams.items[--g->streams.len];
    const Stream stream = {mesh - 1, vertexCount, indexCount, read, user};
    artfVecPush(&g->streams, stream);
    s->meshes.items[mesh - 1].streamed = true;
    /* (only the lightest stays, until more is wanted) */
    artfMeshSetResidentLevel(s, mesh, (int)s->meshes.items[mesh - 1].lodCount);
    return mesh;
}

/* As textures': each mesh's heaviest level that what uses it needs, as near as half its distance; then, over the
 * budget, the largest gives up a level until it fits. */
static void meshResidency(ArtfScene* s, ArtfScale* g, float lodScale, bool all) {
    const ArtfResidencyDesc* r = &s->residency;
    const uint32_t count = (uint32_t)s->meshes.len;
    int* wanted = artfMalloc(((size_t)count + 1) * sizeof *wanted);
    for (uint32_t i = 0; i < count; ++i) wanted[i] = all ? 0 : (int)s->meshes.items[i].lodCount;
    for (size_t i = 0; i < s->instances.len && !all; ++i) {
        const ArtfInstanceData* d = &s->instances.items[i];
        const ArtfMeshData* m = &s->meshes.items[d->mesh];
        if (!m->lodCount || m->isLod) continue;
        float w[3];
        worldCenter(d, m->center, w);
        const float distance = distanceTo(s, w, m->radius * d->maxScale) * .5f;
        int level = 0;
        if (lodScale > 0)
            for (uint32_t k = 0; k < m->lodCount; ++k)
                if (m->lodErrors[k] * d->maxScale * lodScale <= distance) level = (int)k + 1;
        wanted[d->mesh] = artfMini(wanted[d->mesh], level);
    }
    if (!all && r->meshBudget) {
        for (;;) {
            size_t total = 0, largest = 0;
            uint32_t chosen = UINT32_MAX;
            for (uint32_t i = 0; i < count; ++i) {
                const ArtfMeshData* m = &s->meshes.items[i];
                if (m->removed || !m->lodCount || m->isLod) continue;
                for (uint32_t k = (uint32_t)wanted[i]; k <= m->lodCount; ++k) total += meshBytes(&s->meshes.items[k ? m->lods[k - 1] : i]);
                const size_t heaviest = meshBytes(&s->meshes.items[wanted[i] ? m->lods[wanted[i] - 1] : i]);
                if ((uint32_t)wanted[i] < m->lodCount && heaviest > largest) { largest = heaviest; chosen = i; }
            }
            if (total <= r->meshBudget || chosen == UINT32_MAX) break;
            ++wanted[chosen];
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        const ArtfMeshData* m = &s->meshes.items[i];
        if (m->removed || !m->lodCount || m->isLod || (int)m->residentFrom == wanted[i]) continue;
        if (all || r->mode == ARTF_RESIDENCY_AUTOMATIC) artfMeshSetResidentLevel(s, i + 1, wanted[i]);
        else if (r->reportMesh) r->reportMesh(r->user, i + 1, wanted[i]);
    }
    artfFree(wanted);
    (void)g;
}

/* ---- each frame ---- */

void artfSceneScaleStep(ArtfScene* s, int width, int height) {
    const bool meshes = s->residency.meshes && s->residency.mode != ARTF_RESIDENCY_MANUAL;
    if (!s->scale && !meshes) return;
    ArtfScale* g = scaleOf(s);
    if (!g->hlods.len && !g->skinned && !meshes && !g->partial) return;
    const bool traced = s->trace.desc.mode != ARTF_TRACE_NONE;
    const float lodScale = lodScaleOf(s, width, height);
    if (g->hlods.len) hlodStep(s, g, lodScale, traced);
    if (g->skinned) skinStep(s, lodScale, traced);
    /* (what is traced, or chosen with no levels, needs all of it: at once) */
    if (g->partial && (traced || lodScale <= 0)) {
        meshResidency(s, g, lodScale, true);
    } else if (meshes && g->meshWait-- <= 0) {
        g->meshWait = (s->residency.interval > 0 ? s->residency.interval : 30) - 1;
        meshResidency(s, g, lodScale, false);
    }
}
