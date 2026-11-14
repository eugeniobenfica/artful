/* artful-tests: the scene's data, the rasterizer's promises, and the two backends against each other.
 * The GPU's part runs where Vulkan does, and says so where it does not. */
#include "artful/artful.h"
#include "scene/sceneInternal.h"
#include "render/rendererInternal.h"
#include "core/threads.h"
#include "holeInternal.h" /* (extensions/blackhole) */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0, failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

#define W 320
#define H 200

static int compareEdgeKeys(const void* a, const void* b) {
    const uint64_t *x = a, *y = b;
    if (x[0] != y[0]) return x[0] < y[0] ? -1 : 1;
    return x[1] < y[1] ? -1 : x[1] > y[1];
}

static uint64_t hashOf(const uint32_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static ArtfRendererOptions options(ArtfBackend backend, int threads, bool portable) {
    ArtfRendererOptions o;
    memset(&o, 0, sizeof o);
    o.backend = backend;
    o.width = W;
    o.height = H;
    o.threads = threads;
    o.simd = portable ? ARTF_SIMD_PORTABLE : ARTF_SIMD_AUTO;
    return o;
}

static void setCamera(ArtfScene* s, float x, float y, float z, float tx, float ty, float tz) {
    ArtfCamera c = {artfVec3(x, y, z), artfVec3(tx, ty, tz), artfVec3(0, 1, 0), 1.f, .1f};
    artfSceneSetCamera(s, &c);
}

/* the same scene as a test of everything at once: texture, lights, overlapping objects */
static ArtfScene* richScene(void) {
    ArtfScene* s = artfSceneNew();
    static uint32_t px[64 * 64];
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) px[y * 64 + x] = ((x / 8 + y / 8) & 1) ? 0xFFD0C8B0u : 0xFF404858u;
    ArtfMaterialDesc floorDesc = {artfVec3(.9f, .9f, .9f), artfSceneAddTexture(s, px, 64, 64, 0), .2f, .33f, artfVec3(0, 0, 0)};
    ArtfTransform t = artfTransformAt(0, -1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 30, 8), artfSceneAddMaterial(s, &floorDesc), &t);
    ArtfMaterialDesc red = {artfVec3(.8f, .15f, .1f), 0, .7f, .17f, artfVec3(0, 0, 0)};
    ArtfMaterialDesc glow = {artfVec3(.1f, .3f, .2f), 0, 0, 1, artfVec3(.05f, .2f, .1f)};
    ArtfMesh sphere = artfSceneAddSphere(s, 1, 32, 48), torus = artfSceneAddTorus(s, 1, .3f, 48, 24), cube = artfSceneAddCube(s, 1.4f);
    t = artfTransformAt(-1.8f, 0, 0, 1);
    artfSceneAddInstance(s, sphere, artfSceneAddMaterial(s, &red), &t);
    t = artfTransformTrs(artfVec3(.4f, 0, .3f), artfQuatAxisAngle(artfVec3(1, 0, .3f), 1.f), artfVec3(1.1f, .8f, 1.3f)); /* stretched */
    artfSceneAddInstance(s, torus, artfSceneAddMaterial(s, &glow), &t);
    t = artfTransformTrs(artfVec3(2, 0, -.5f), artfQuatAxisAngle(artfVec3(0, 1, 0), .5f), artfVec3(1, 1, 1));
    artfSceneAddInstance(s, cube, 0, &t);
    artfSceneAddLight(s, artfVec3(0, 1.5f, 2.5f), artfVec3(.2f, .5f, 1.f), 6.f);
    artfSceneAddLight(s, artfVec3(-3, .5f, 1), artfVec3(1.f, .3f, .1f), 3.f);
    setCamera(s, 0, 2, 6, 0, 0, 0);
    return s;
}

/* ---- the mesh's data ---- */

static void testPacking(void) {
    /* halves and the octahedron come back close to what went in */
    ArtfScene* s = artfSceneNew();
    const ArtfVertex v[3] = {{0, 0, 0, 0, 0, 1, .25f, .5f}, {10, 0, 0, .6f, 0, .8f, 1.5f, -2.f}, {0, 4, 2, 0, -1, 0, 0, 1}};
    const uint32_t idx[3] = {0, 1, 2};
    ArtfMesh mesh = artfSceneAddMesh(s, v, 3, idx, 3);
    CHECK(mesh == 1);
    const ArtfMeshData* m = &s->meshes.items[0];
    CHECK(m->vertexCount == 3 && m->indexCount == 3 && m->meshletCount == 1);
    for (uint32_t i = 0; i < 3; ++i) {
        const ArtfPackedVertex* p = &m->vertices[i];
        const float u = artfHalfToFloat(p->u), w = artfHalfToFloat(p->v);
        const ArtfVec3 n = artfVec3Normalize(artfOctUnfold(p->nx, p->ny));
        float x = (float)p->px / 65535.f * m->scale[0] + m->offset[0];
        bool found = false;
        for (int k = 0; k < 3; ++k)
            found = found || (fabsf(x - v[k].px) < 1e-3f && fabsf(u - v[k].u) < 1e-3f && fabsf(w - v[k].v) < 1e-3f &&
                              fabsf(n.x - v[k].nx) < 1e-3f && fabsf(n.y - v[k].ny) < 1e-3f && fabsf(n.z - v[k].nz) < 1e-3f);
        CHECK(found);
    }
    artfSceneFree(s);
    CHECK(sizeof(ArtfPackedVertex) == 16 && sizeof(ArtfInstanceData) == 64 && sizeof(ArtfMaterialData) == 144 && sizeof(ArtfFrameData) == 178 * 16);
}

static void testMeshlets(void) {
    ArtfScene* s = artfSceneNew();
    artfSceneAddSphere(s, 1, 64, 96);
    const ArtfMeshData* m = &s->meshes.items[0];
    uint64_t triangles = 0;
    for (uint32_t i = 0; i < m->meshletCount; ++i) {
        const ArtfMeshlet* l = &m->meshlets[i];
        CHECK(l->vertexCount <= ARTF_MESHLET_VERTICES && l->triangleCount <= ARTF_MESHLET_TRIANGLES);
        CHECK((l->vertexOffset & 7) == 0);
        triangles += l->triangleCount;
        for (uint32_t t = 0; t < l->triangleCount * 3; ++t) CHECK(m->meshletTriangles[(size_t)l->triangleOffset * 3 + t] < l->vertexCount);
    }
    CHECK(triangles == m->indexCount / 3);
    /* the order is for the cache: a 16-entry FIFO should miss well under one vertex a triangle */
    uint32_t fifo[16], head = 0, misses = 0;
    memset(fifo, 0xFF, sizeof fifo);
    for (uint32_t i = 0; i < m->indexCount; ++i) {
        bool hit = false;
        for (int k = 0; k < 16; ++k) hit = hit || fifo[k] == m->indices[i];
        if (!hit) { fifo[head++ & 15] = m->indices[i]; ++misses; }
    }
    const float acmr = (float)misses / (float)(m->indexCount / 3);
    CHECK(acmr < .85f);
    artfSceneFree(s);
}

static void testInstances(void) {
    ArtfScene* s = artfSceneNew();
    ArtfMesh cube = artfSceneAddCube(s, 1);
    ArtfInstance a = artfSceneAddInstance(s, cube, 0, NULL), b = artfSceneAddInstance(s, cube, 0, NULL), c = artfSceneAddInstance(s, cube, 0, NULL);
    CHECK(artfSceneInstanceCount(s) == 3);
    artfSceneRemoveInstance(s, a);
    CHECK(artfSceneInstanceCount(s) == 2);
    ArtfTransform t = artfTransformAt(7, 8, 9, 1);
    artfInstanceSetTransform(s, c, &t); /* c moved in the dense list: its handle still finds it */
    CHECK(s->instances.items[s->handleSlots.items[c - 1]].m[3] == 7.f);
    CHECK(s->instances.items[s->handleSlots.items[b - 1]].m[3] == 0.f);
    ArtfInstance d = artfSceneAddInstance(s, cube, 0, NULL);
    CHECK((d & 0xFFFFFFu) == (a & 0xFFFFFFu) && d != a); /* places come back, and handles do not */
    CHECK(artfSceneAddInstance(s, 99, 0, NULL) == 0);
    artfSceneFree(s);
}

/* ---- the CPU's promises ---- */

static void testSamePixels(void) {
    /* however many threads, with AVX2 or without */
    ArtfScene* s = richScene();
    uint32_t* frame = artfMalloc((size_t)W * H * 4);
    uint64_t reference = 0;
    static const struct { int threads; ArtfSimd simd; } runs[] = {{1, ARTF_SIMD_AUTO}, {4, ARTF_SIMD_AUTO}, {3, ARTF_SIMD_AUTO}, {1, ARTF_SIMD_PORTABLE}, {4, ARTF_SIMD_PORTABLE}, {2, ARTF_SIMD_128}, {3, ARTF_SIMD_256}};
    for (size_t i = 0; i < ARTF_COUNT(runs); ++i) {
        ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, runs[i].threads, false);
        o.simd = runs[i].simd;
        ArtfRenderer* r = artfRendererNew(&o);
        for (int k = 0; k < 2; ++k) CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, frame));
        uint64_t h = hashOf(frame, (size_t)W * H);
        if (!i) reference = h;
        CHECK(h == reference);
        artfRendererFree(r);
    }
    artfFree(frame);
    artfSceneFree(s);
}

static uint32_t backgroundOf(ArtfVec3 c) {
    return 0xFF000000u | (uint32_t)(c.x * 255.f + .5f) << 16 | (uint32_t)(c.y * 255.f + .5f) << 8 | (uint32_t)(c.z * 255.f + .5f);
}

/* Triangles that share an edge leave no gap: a mesh of many, seen whole, covers everything. */
static void testNoGaps(void) {
    ArtfScene* s = artfSceneNew();
    const ArtfVec3 bg = artfVec3(1, 0, 1);
    artfSceneSetBackground(s, bg);
    enum { N = 48 };
    static ArtfVertex v[(N + 1) * (N + 1)];
    static uint32_t idx[N * N * 6];
    uint32_t n = 0;
    uint32_t seed = 1;
    for (int y = 0; y <= N; ++y)
        for (int x = 0; x <= N; ++x) {
            seed = seed * 1664525u + 1013904223u;
            float jx = x && x < N ? ((float)(seed >> 16 & 255) / 255.f - .5f) * .5f : 0, jy = y && y < N ? ((float)(seed >> 24) / 255.f - .5f) * .5f : 0;
            ArtfVertex q = {((float)x + jx) / N * 40.f - 20.f, 0, ((float)y + jy) / N * 40.f - 20.f, 0, 1, 0, 0, 0};
            v[y * (N + 1) + x] = q;
        }
    for (uint32_t y = 0; y < N; ++y)
        for (uint32_t x = 0; x < N; ++x) {
            uint32_t a = y * (N + 1) + x, b = a + 1, c = a + N + 1, d = c + 1;
            idx[n++] = a; idx[n++] = c; idx[n++] = b;
            idx[n++] = b; idx[n++] = c; idx[n++] = d;
        }
    artfSceneAddInstance(s, artfSceneAddMesh(s, v, (N + 1) * (N + 1), idx, n), 0, NULL);
    setCamera(s, 0, 5, .01f, 0, 0, 0);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    CHECK(artfRendererDraw(r, s));
    const uint32_t* p = artfRendererPixels(r), want = backgroundOf(bg);
    int holes = 0;
    for (int i = 0; i < W * H; ++i) holes += p[i] == want;
    CHECK(holes == 0);
    artfRendererFree(r);
    artfSceneFree(s);
}

/* A floor from under the eye to far away: its triangles cross the near plane and go past every edge. */
static void testClipping(void) {
    ArtfScene* s = artfSceneNew();
    const ArtfVec3 bg = artfVec3(1, 0, 1);
    artfSceneSetBackground(s, bg);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 100000.f, 1.f), 0, NULL);
    setCamera(s, 0, 1, 0, 0, -.4f, -1);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    CHECK(artfRendererDraw(r, s));
    const uint32_t* p = artfRendererPixels(r), want = backgroundOf(bg);
    /* floor all over, with no crack along the edge its two triangles share */
    int holes = 0;
    for (int i = 0; i < W * H; ++i) holes += p[i] == want;
    CHECK(holes == 0);
    artfRendererFree(r);
    artfSceneFree(s);
}

/* What is behind the eye and what is outside the view draws nothing. */
static void testCulling(void) {
    ArtfScene* s = artfSceneNew();
    ArtfMesh cube = artfSceneAddCube(s, 1);
    ArtfTransform behind = artfTransformAt(0, 0, 50, 1), inView = artfTransformAt(0, 0, -10, 1), aside = artfTransformAt(500, 0, -10, 1);
    artfSceneAddInstance(s, cube, 0, &behind);
    artfSceneAddInstance(s, cube, 0, &inView);
    artfSceneAddInstance(s, cube, 0, &aside);
    setCamera(s, 0, 0, 0, 0, 0, -1);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    CHECK(artfRendererDraw(r, s));
    ArtfFrameStats st;
    artfRendererStats(r, &st);
    CHECK(st.instancesDrawn == 1);
    CHECK(st.trianglesBinned >= 2 && st.trianglesBinned <= 6); /* the faces in view, not the back ones */
    artfRendererFree(r);
    artfSceneFree(s);
}

static void testOverlay(void) {
    ArtfScene* s = richScene();
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    CHECK(artfRendererDraw(r, s));
    uint32_t* plain = artfMalloc((size_t)W * H * 4);
    memcpy(plain, artfRendererPixels(r), (size_t)W * H * 4);

    /* an empty overlay changes nothing */
    uint32_t* over = artfCalloc((size_t)W * H, 4);
    ArtfOverlay overlay = {over, W, H, 0, false, NULL, 0};
    artfRendererSetOverlay(r, &overlay);
    CHECK(artfRendererDraw(r, s));
    CHECK(!memcmp(plain, artfRendererPixels(r), (size_t)W * H * 4));

    /* an opaque corner shows as it is, and its tiles are not drawn */
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) over[y * W + x] = 0xFF112233u;
    /* a translucent pixel: premultiplied white at half */
    over[150 * W + 200] = 0x80808080u;
    ArtfRect damage[2] = {{0, 0, 128, 128}, {200, 150, 1, 1}};
    ArtfOverlay partial = {over, W, H, 0, true, damage, 2};
    artfRendererSetOverlay(r, &partial);
    CHECK(artfRendererDraw(r, s));
    const uint32_t* p = artfRendererPixels(r);
    ArtfFrameStats st;
    artfRendererStats(r, &st);
    CHECK(st.tilesSkipped == 4);
    CHECK(p[0] == 0xFF112233u && p[127 * W + 127] == 0xFF112233u);
    CHECK(p[128] == plain[128]);
    for (int c = 0; c < 24; c += 8) {
        const int base = (int)(plain[150 * W + 200] >> c & 255), got = (int)(p[150 * W + 200] >> c & 255);
        const int want = 128 + base * 127 / 255;
        CHECK(abs(got - want) <= 1);
    }
    CHECK(p[150 * W + 200] >> 24 == 255);

    /* the overlay goes, and the scene is back */
    artfRendererSetOverlay(r, NULL);
    CHECK(artfRendererDraw(r, s));
    CHECK(!memcmp(plain, artfRendererPixels(r), (size_t)W * H * 4));

    /* a smaller one covers only its corner */
    uint32_t small[16 * 16];
    for (int i = 0; i < 256; ++i) small[i] = 0xFFFF0000u;
    ArtfOverlay little = {small, 16, 16, 0, false, NULL, 0};
    artfRendererSetOverlay(r, &little);
    CHECK(artfRendererDraw(r, s));
    p = artfRendererPixels(r);
    CHECK(p[15 * W + 15] == 0xFFFF0000u && p[16] == plain[16] && p[16 * W] == plain[16 * W]);

    artfFree(plain);
    artfFree(over);
    artfRendererFree(r);
    artfSceneFree(s);
}

static void testResize(void) {
    ArtfScene* s = richScene();
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 2, false);
    ArtfRenderer* r = artfRendererNew(&o);
    CHECK(artfRendererDraw(r, s));
    artfRendererResize(r, 97, 53); /* not a whole number of tiles */
    CHECK(artfRendererDraw(r, s));
    uint32_t* a = artfMalloc(97 * 53 * 4);
    memcpy(a, artfRendererPixels(r), 97 * 53 * 4);
    /* a new renderer draws the same scene the same: nothing of it lived in the old one */
    artfRendererFree(r);
    o.width = 97;
    o.height = 53;
    r = artfRendererNew(&o);
    CHECK(artfRendererDraw(r, s));
    CHECK(!memcmp(a, artfRendererPixels(r), 97 * 53 * 4));
    artfFree(a);
    artfRendererFree(r);
    artfSceneFree(s);
}

/* ---- levels of detail ---- */

static void testSimplify(void) {
    ArtfScene* s = artfSceneNew();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true); /* (whatever ARTFUL_FEATURES says) */
    ArtfMesh cube = artfSceneAddCube(s, 1), sphere = artfSceneAddSphere(s, 1, 64, 96);
    CHECK(s->meshes.items[cube - 1].lodCount == 0); /* too small to be worth it */
    const ArtfMeshData* m = &s->meshes.items[sphere - 1];
    CHECK(m->lodCount >= 3); /* (its seam goes too, on both sides at once; its poles stay) */
    uint32_t triangles = m->indexCount / 3;
    float error = 0;
    for (uint32_t i = 0; i < m->lodCount; ++i) {
        const ArtfMeshData* l = &s->meshes.items[m->lods[i]];
        CHECK(l->isLod);
        CHECK(l->indexCount / 3 <= triangles / 3); /* about a quarter of the one before */
        CHECK(m->lodErrors[i] >= error);
        triangles = l->indexCount / 3;
        error = m->lodErrors[i];
        /* the surface stayed where it was: every vertex still on the sphere, within what was reported */
        for (uint32_t v = 0; v < l->vertexCount; ++v) {
            const ArtfPackedVertex* p = &l->vertices[v];
            const float x = (float)p->px / 65535.f * l->scale[0] + l->offset[0], y = (float)p->py / 65535.f * l->scale[1] + l->offset[1], z = (float)p->pz / 65535.f * l->scale[2] + l->offset[2];
            CHECK(fabsf(sqrtf(x * x + y * y + z * z) - 1.f) < .01f);
        }
        CHECK(fabsf(l->radius - 1.f) < .05f);
    }
    /* The cut stays closed: by where they are, each edge of each level has two sides, as at the start (where the
     * smallest triangles at the poles round to nothing, a few do not). */
    uint32_t openAtFirst = 0;
    for (uint32_t i = 0; i <= m->lodCount; ++i) {
        const ArtfMeshData* l = i ? &s->meshes.items[m->lods[i - 1]] : m;
        const uint32_t edges = l->indexCount;
        uint64_t* keys = artfMalloc((size_t)edges * 16);
        for (uint32_t c = 0; c < edges; ++c) {
            const ArtfPackedVertex *a = &l->vertices[l->indices[c]], *b = &l->vertices[l->indices[c / 3 * 3 + (c + 1) % 3]];
            const uint64_t ka = (uint64_t)a->px << 32 | (uint64_t)a->py << 16 | a->pz, kb = (uint64_t)b->px << 32 | (uint64_t)b->py << 16 | b->pz;
            keys[c * 2] = ka < kb ? ka : kb;
            keys[c * 2 + 1] = ka < kb ? kb : ka;
        }
        qsort(keys, edges, 16, compareEdgeKeys);
        uint32_t open = 0;
        for (uint32_t c = 0; c < edges;) {
            uint32_t j = c + 1;
            while (j < edges && keys[j * 2] == keys[c * 2] && keys[j * 2 + 1] == keys[c * 2 + 1]) ++j;
            open += (j - c) & 1;
            c = j;
        }
        if (!i) openAtFirst = open;
        CHECK(open <= openAtFirst);
        artfFree(keys);
    }
    /* What is reported is honest: the middle of a chord of the lightest level really is that far in.
     * For triangles of this size on a unit sphere the sag is about 1 / 8 of an edge squared. */
    {
        const ArtfMeshData* l = &s->meshes.items[m->lods[m->lodCount - 1]];
        double edges = 0;
        for (uint32_t i = 0; i + 1 < l->indexCount; i += 3) {
            const ArtfPackedVertex *a = &l->vertices[l->indices[i]], *b = &l->vertices[l->indices[i + 1]];
            const double dx = ((double)a->px - b->px) / 65535.0 * l->scale[0], dy = ((double)a->py - b->py) / 65535.0 * l->scale[1], dz = ((double)a->pz - b->pz) / 65535.0 * l->scale[2];
            edges += sqrt(dx * dx + dy * dy + dz * dz);
        }
        const double mean = edges / (l->indexCount / 3), sag = mean * mean / 8;
        CHECK(m->lodErrors[m->lodCount - 1] > sag * .5);
    }
    artfSceneFree(s);

    /* a sheet with a border keeps its outline: its corners are still where they were */
    s = artfSceneNew();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true);
    enum { N = 40 };
    static ArtfVertex v[(N + 1) * (N + 1)];
    static uint32_t idx[N * N * 6];
    uint32_t n = 0;
    for (int y = 0; y <= N; ++y)
        for (int x = 0; x <= N; ++x) {
            ArtfVertex q = {(float)x, 0, (float)y, 0, 1, 0, 0, 0};
            v[y * (N + 1) + x] = q;
        }
    for (uint32_t y = 0; y < N; ++y)
        for (uint32_t x = 0; x < N; ++x) {
            uint32_t a = y * (N + 1) + x, b = a + 1, c = a + N + 1, d = c + 1;
            idx[n++] = a; idx[n++] = c; idx[n++] = b;
            idx[n++] = b; idx[n++] = c; idx[n++] = d;
        }
    ArtfMesh sheet = artfSceneAddMesh(s, v, (N + 1) * (N + 1), idx, n);
    const ArtfMeshData* base = &s->meshes.items[sheet - 1];
    CHECK(base->lodCount >= 1);
    const ArtfMeshData* light = &s->meshes.items[base->lods[0]];
    CHECK(light->indexCount < base->indexCount / 2);
    CHECK(light->scale[0] == base->scale[0] && light->scale[2] == base->scale[2]);
    CHECK(base->lodErrors[0] < 1e-3f); /* a flat sheet loses nothing */
    artfSceneFree(s);
}

static void testLod(void) {
    ArtfScene* s = artfSceneNew();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true);
    ArtfMesh ball = artfSceneAddSphere(s, 1, 96, 128);
    ArtfTransform t = artfTransformAt(0, 0, -60, 1);
    artfSceneAddInstance(s, ball, 0, &t);
    setCamera(s, 0, 0, 0, 0, 0, -1);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    uint32_t* full = artfMalloc((size_t)W * H * 4);

    artfSceneSetFeature(s, ARTF_FEATURE_LOD, false);
    CHECK(!artfSceneFeature(s, ARTF_FEATURE_LOD));
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, full));
    ArtfFrameStats off, on;
    artfRendererStats(r, &off);
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true);
    CHECK(artfRendererDraw(r, s));
    artfRendererStats(r, &on);
    CHECK(on.geometryMs >= 0);
    const uint32_t* p = artfRendererPixels(r);
    /* the outline has not moved by much: what differs is little */
    int different = 0;
    for (int i = 0; i < W * H; ++i) {
        int worst = 0;
        for (int c = 0; c < 24; c += 8) worst = artfMaxi(worst, abs((int)(p[i] >> c & 255) - (int)(full[i] >> c & 255)));
        different += worst > 24;
    }
    CHECK(different < W * H / 200);
    /* close by, and asked for little error, no lighter mesh fits: the pixels are the full mesh's */
    ArtfTransform close = artfTransformAt(0, 0, -4, 1);
    artfInstanceSetTransform(s, 1, &close);
    artfSceneSetLodError(s, .01f);
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, false);
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, full));
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true);
    CHECK(artfRendererDraw(r, s));
    CHECK(!memcmp(full, artfRendererPixels(r), (size_t)W * H * 4));

    /* a mesh added with the feature off gets its levels when it comes on */
    ArtfScene* later = artfSceneNew();
    artfSceneSetFeature(later, ARTF_FEATURE_LOD, false);
    ArtfMesh mesh = artfSceneAddSphere(later, 1, 32, 48);
    CHECK(later->meshes.items[mesh - 1].lodCount == 0);
    artfSceneSetFeature(later, ARTF_FEATURE_LOD, true);
    CHECK(later->meshes.items[mesh - 1].lodCount > 0);
    artfSceneFree(later);

    artfFree(full);
    artfRendererFree(r);
    artfSceneFree(s);
}

/* ---- occlusion ---- */

/* a wall, with a crowd behind it and a few things before it */
static ArtfScene* wallScene(ArtfInstance* wall) {
    ArtfScene* s = artfSceneNew();
    ArtfMesh cube = artfSceneAddCube(s, 1), ball = artfSceneAddSphere(s, .4f, 12, 16);
    ArtfTransform t = artfTransformTrs(artfVec3(0, 0, 0), artfQuatIdentity(), artfVec3(60, 40, 1));
    *wall = artfSceneAddInstance(s, cube, 0, &t);
    ArtfMaterialDesc d = {artfVec3(.9f, .3f, .2f), 0, .4f, .24f, artfVec3(0, 0, 0)};
    ArtfMaterial m = artfSceneAddMaterial(s, &d);
    for (int z = 0; z < 20; ++z)
        for (int x = 0; x < 20; ++x) {
            t = artfTransformAt((float)x - 10.f, (float)(z % 5) - 2.f, -3.f - (float)z, 1);
            artfSceneAddInstance(s, ball, m, &t);
        }
    t = artfTransformAt(-1, 0, 4, 1);
    artfSceneAddInstance(s, ball, m, &t);
    setCamera(s, 0, 0, 12, 0, 0, 0);
    return s;
}

static void testOcclusion(ArtfBackend backend) {
    ArtfInstance wall;
    ArtfScene* s = wallScene(&wall);
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, false);
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) { artfSceneFree(s); return; }
    uint32_t* plain = artfMalloc((size_t)W * H * 4);
    uint32_t* got = artfMalloc((size_t)W * H * 4);

    /* each frame, over several, against the same scene drawn without */
    artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, false);
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, plain));
    artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, true);
    for (int i = 0; i < 4; ++i) {
        CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, got));
        CHECK(!memcmp(plain, got, (size_t)W * H * 4));
    }
    ArtfFrameStats st;
    artfRendererStats(r, &st);
    if (backend == ARTF_BACKEND_SOFTWARE) {
        CHECK(st.instancesHidden > 300);  /* the crowd */
        CHECK(st.instancesDrawn < 50);
    }

    /* the wall goes: what it hid must be there in this very frame, not the next */
    ArtfTransform away = artfTransformAt(0, 300, 0, 1);
    artfInstanceSetTransform(s, wall, &away);
    artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, false);
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, plain));
    /* (a second scene state would be needed to come back: so the feature goes on with the wall back) */
    ArtfTransform back = artfTransformTrs(artfVec3(0, 0, 0), artfQuatIdentity(), artfVec3(60, 40, 1));
    artfInstanceSetTransform(s, wall, &back);
    artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, true);
    for (int i = 0; i < 3; ++i) CHECK(artfRendererDraw(r, s)); /* the crowd is hidden again */
    artfInstanceSetTransform(s, wall, &away);
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, got));
    CHECK(!memcmp(plain, got, (size_t)W * H * 4));

    /* instances going while it runs: the flags of the old order cannot lose one */
    artfInstanceSetTransform(s, wall, &back);
    for (int i = 0; i < 2; ++i) CHECK(artfRendererDraw(r, s));
    artfSceneRemoveInstance(s, wall);
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, got));
    artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, false);
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, plain));
    CHECK(!memcmp(plain, got, (size_t)W * H * 4));

    artfFree(plain);
    artfFree(got);
    artfRendererFree(r);
    artfSceneFree(s);
}

/* What an opaque overlay covers hides the scene as a wall does. */
static void testOcclusionUnderOverlay(void) {
    ArtfInstance wall;
    ArtfScene* s = wallScene(&wall);
    artfSceneRemoveInstance(s, wall);
    artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, true);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    uint32_t* over = artfMalloc((size_t)W * H * 4);
    for (int i = 0; i < W * H; ++i) over[i] = 0xFF204060u;
    ArtfOverlay overlay = {over, W, H, 0, false, NULL, 0};
    artfRendererSetOverlay(r, &overlay);
    for (int i = 0; i < 3; ++i) CHECK(artfRendererDraw(r, s));
    ArtfFrameStats st;
    artfRendererStats(r, &st);
    CHECK(st.instancesDrawn <= 1);
    CHECK(artfRendererPixels(r)[0] == 0xFF204060u);
    artfFree(over);
    artfRendererFree(r);
    artfSceneFree(s);
}

/* ---- the GPU against the CPU ---- */

/* How far apart two pictures are: a GPU rounds and samples in its own way, and its edges fall on
 * slightly other pixels, so what is compared is how many pixels differ by much. */
static double differentShare(const uint32_t* a, const uint32_t* b, int threshold) {
    int different = 0;
    for (int i = 0; i < W * H; ++i) {
        int worst = 0;
        for (int c = 0; c < 24; c += 8) worst = artfMaxi(worst, abs((int)(a[i] >> c & 255) - (int)(b[i] >> c & 255)));
        different += worst > threshold;
    }
    return (double)different / (W * H);
}

static void testVulkan(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (!gpu) { puts("no Vulkan here: its tests were not run"); return; }
    printf("on %s\n", artfRendererDescription(gpu));
    o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    ArtfScene* s = richScene();
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);

    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(gpu, a));
    CHECK(artfRendererReadPixels(cpu, b));
    const double far = differentShare(a, b, 24);
    printf("pixels that differ by more than 24 of 255: %.2f%%\n", far * 100);
    CHECK(far < .02);

    /* the scene changes: what the GPU keeps follows */
    ArtfMesh more = artfSceneAddCube(s, 1);
    ArtfTransform t = artfTransformAt(0, 1.5f, 1, 1);
    ArtfMaterialDesc d = {artfVec3(.1f, .2f, .9f), 0, 0, 1, artfVec3(0, 0, 0)};
    ArtfInstance added = artfSceneAddInstance(s, more, artfSceneAddMaterial(s, &d), &t);
    for (int i = 0; i < 2000; ++i) { /* past what the buffers started with */
        ArtfTransform far2 = artfTransformAt((float)i, 0, 500, 1);
        artfSceneAddInstance(s, more, 0, &far2);
    }
    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(gpu, a));
    CHECK(artfRendererReadPixels(cpu, b));
    CHECK(differentShare(a, b, 24) < .02);
    artfSceneRemoveInstance(s, added);
    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(gpu, a));
    CHECK(artfRendererReadPixels(cpu, b));
    CHECK(differentShare(a, b, 24) < .02);

    /* the overlay, whole and then in part */
    uint32_t* over = artfCalloc((size_t)W * H, 4);
    for (int y = 10; y < 60; ++y)
        for (int x = 10; x < 110; ++x) over[y * W + x] = 0x80800000u; /* half red, premultiplied */
    ArtfOverlay overlay = {over, W, H, 0, false, NULL, 0};
    artfRendererSetOverlay(gpu, &overlay);
    artfRendererSetOverlay(cpu, &overlay);
    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(gpu, a));
    CHECK(artfRendererReadPixels(cpu, b));
    CHECK(differentShare(a, b, 24) < .02);
    for (int y = 100; y < 130; ++y)
        for (int x = 200; x < 300; ++x) over[y * W + x] = 0xFF00FF00u;
    ArtfRect damage = {200, 100, 100, 30};
    ArtfOverlay partial = {over, W, H, 0, true, &damage, 1};
    artfRendererSetOverlay(gpu, &partial);
    artfRendererSetOverlay(cpu, &partial);
    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(gpu, a));
    CHECK(artfRendererReadPixels(cpu, b));
    CHECK(a[110 * W + 250] == 0xFF00FF00u);
    CHECK(differentShare(a, b, 24) < .02);

    /* a size of its own */
    artfRendererResize(gpu, 200, 120);
    artfRendererSetOverlay(gpu, NULL);
    CHECK(artfRendererDraw(gpu, s));
    CHECK(artfRendererReadPixels(gpu, a));

    artfFree(over);
    artfFree(a);
    artfFree(b);
    artfRendererFree(gpu);
    artfRendererFree(cpu);
    artfSceneFree(s);
}

#ifdef ARTFUL_COUNT_ALLOCATIONS
long artfAllocationsAlive(void);
#endif

/* ---- a black hole ---- */

static ArtfScene* holeScene(float distance, float tilt, float doppler, float stars) {
    ArtfScene* s = artfSceneNew();
    ArtfBlackHoleDesc d = artfBlackHoleDefaults();
    d.spin = 0;
    d.axis = artfVec3(0, 1, 0);
    d.doppler = doppler;
    d.stars = stars;
    d.bloom = 0;
    artfSceneSetBlackHole(s, &d);
    ArtfCamera c = {artfVec3(0, sinf(tilt) * distance, cosf(tilt) * distance), artfVec3(0, 0, 0), artfVec3(0, 0, -1), .9f, .1f};
    if (fabsf(tilt) < 1.f) c.up = artfVec3(0, 1, 0);
    artfSceneSetCamera(s, &c);
    return s;
}

/* How far the black in the middle of a frame reaches, in pixels: the least of what it does in sixteen
 * directions, as the gas that lights its edge has gaps. */
static float shadowRadius(ArtfRenderer* r, ArtfScene* s, uint32_t* a) {
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, a));
    float least = 1e9f;
    for (int i = 0; i < 16; ++i) {
        const float dx = cosf((float)i * .3926991f), dy = sinf((float)i * .3926991f);
        for (float at = 1.f; at < (float)H * .5f; at += .25f) {
            const int x = (int)((float)W * .5f + dx * at), y = (int)((float)H * .5f + dy * at);
            if (((a[(size_t)y * W + x] >> 16) & 255) >= 24) { least = artfMinf(least, at); break; }
        }
    }
    return least;
}

/* The mean brightness of a column range. */
static double brightness(const uint32_t* a, int x0, int x1) {
    double sum = 0;
    for (int y = 0; y < H; ++y)
        for (int x = x0; x < x1; ++x) {
            const uint32_t p = a[y * W + x];
            sum += (double)((p >> 16 & 255) + (p >> 8 & 255) + (p & 255));
        }
    return sum / ((double)H * (x1 - x0));
}

/* The fluid: chaotic, and yet the same every time; bounded however long it runs; and what it does is
 * what gas in orbit does. */
/* ---- colors, layers, alpha and samples ---- */

#define CHANNEL(p, shift) ((int)((p) >> (shift) & 255))

/* Light that gives a surface's own color back: what is drawn is what was asked for. */
static ArtfScene* plainScene(void) {
    ArtfScene* s = artfSceneNew();
    artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(0, 0, 0));
    artfSceneSetAmbient(s, artfVec3(1, 1, 1), artfVec3(1, 1, 1));
    artfSceneSetBackground(s, artfVec3(0, 0, 0));
    setCamera(s, 0, 0, 5, 0, 0, 0);
    return s;
}

static ArtfMaterial whiteMaterial(ArtfScene* s) {
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), 0, 0, 1, artfVec3(0, 0, 0)};
    return artfSceneAddMaterial(s, &d);
}

/* A square of side 2 in the xy plane, facing the eye; its corners from the bottom left, going round. */
static ArtfMesh addSquare(ArtfScene* s, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    const ArtfVertex v[4] = {{-1, -1, 0, 0, 0, 1, 0, 0, a}, {1, -1, 0, 0, 0, 1, 1, 0, b}, {1, 1, 0, 0, 0, 1, 1, 1, c}, {-1, 1, 0, 0, 0, 1, 0, 1, d}};
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    return artfSceneAddMesh(s, v, 4, idx, 6);
}

static ArtfInstance addAt(ArtfScene* s, ArtfMesh mesh, ArtfMaterial material, float x, float y, float z, float scale) {
    const ArtfTransform t = artfTransformAt(x, y, z, scale);
    return artfSceneAddInstance(s, mesh, material, &t);
}

static void drawInto(ArtfRenderer* r, ArtfScene* s, uint32_t* frame) {
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, frame));
}

static bool near_(int a, int b, int margin) { return abs(a - b) <= margin; }

static void testColorData(void) {
    ArtfScene* s = artfSceneNew();
    const uint32_t plain = addSquare(s, 0, 0, 0, 0) - 1, white = addSquare(s, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0xFFFFFFFFu) - 1;
    CHECK(!s->meshes.items[plain].colors && !s->meshes.items[plain].hasAlpha);
    CHECK(!s->meshes.items[white].colors);
    const uint32_t solid = addSquare(s, 0xFF102030u, 0, 0xFF405060u, 0xFF708090u) - 1;
    const uint32_t clear = addSquare(s, 0x80102030u, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu) - 1;
    const ArtfMeshData* m = &s->meshes.items[solid];
    CHECK(m->colors && !m->hasAlpha);
    CHECK(s->meshes.items[clear].colors && s->meshes.items[clear].hasAlpha);
    CHECK(sizeof(ArtfPackedVertex) == 16 && sizeof(ArtfInstanceData) == 64);
    /* what a mesh is unpacked to, for its lighter versions, keeps them; 0 is white */
    ArtfVertex back[4];
    artfMeshUnpack(m, back);
    int found = 0;
    for (int i = 0; i < 4; ++i) found += back[i].color == 0xFF102030u || back[i].color == 0xFFFFFFFFu || back[i].color == 0xFF405060u || back[i].color == 0xFF708090u;
    CHECK(found == 4);

    /* a layer is one while it needs to be, and only that long */
    ArtfInstance a = addAt(s, solid + 1, 0, 0, 0, 0, 1), b = addAt(s, clear + 1, 0, 0, 0, 0, 1);
    CHECK(s->translucentCount == 1);
    artfInstanceSetOpacity(s, a, .5f);
    CHECK(s->translucentCount == 2);
    artfInstanceSetOpacity(s, a, 1.f);
    artfInstanceSetOpacity(s, b, 1.f); /* (its colors still ask for it) */
    CHECK(s->translucentCount == 1);
    artfSceneRemoveInstance(s, b);
    CHECK(s->translucentCount == 0);
    artfSceneFree(s);

    /* with the lighter versions, too: a colored mesh's levels are colored */
    s = artfSceneNew();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true);
    enum { N = 24 };
    static ArtfVertex v[(N + 1) * (N + 1)];
    static uint32_t idx[N * N * 6];
    uint32_t n = 0;
    for (int y = 0; y <= N; ++y)
        for (int x = 0; x <= N; ++x) {
            ArtfVertex q = {(float)x, (float)y, 0, 0, 0, 1, 0, 0, 0xFF00FF00u};
            v[y * (N + 1) + x] = q;
        }
    for (uint32_t y = 0; y < N; ++y)
        for (uint32_t x = 0; x < N; ++x) {
            const uint32_t a0 = y * (N + 1) + x, b0 = a0 + 1, d0 = a0 + N + 1, e0 = d0 + 1;
            idx[n++] = a0; idx[n++] = b0; idx[n++] = e0;
            idx[n++] = a0; idx[n++] = e0; idx[n++] = d0;
        }
    const uint32_t first = artfSceneAddMesh(s, v, (N + 1) * (N + 1), idx, n) - 1;
    const ArtfMeshData* base = &s->meshes.items[first];
    CHECK(base->lodCount > 0);
    for (uint32_t i = 0; i < base->lodCount; ++i) {
        const ArtfMeshData* level = &s->meshes.items[base->lods[i]];
        CHECK(level->colors && level->colors[0] == 0xFF00FF00u);
    }
    artfSceneFree(s);
}

/* What the scene's colors, layers and alpha come to, on one backend. */
static void testLayers(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    const size_t mid = (size_t)(H / 2) * W + W / 2;
    const int margin = backend == ARTF_BACKEND_VULKAN ? 3 : 2;

    /* a vertex's color is what shows */
    ArtfScene* s = plainScene();
    ArtfMaterial white = whiteMaterial(s);
    addAt(s, addSquare(s, 0xFF804020u, 0xFF804020u, 0xFF804020u, 0xFF804020u), white, 0, 0, 0, 1);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[mid], 16), 0x80, margin) && near_(CHANNEL(a[mid], 8), 0x40, margin) && near_(CHANNEL(a[mid], 0), 0x20, margin));
    CHECK(a[mid] >> 24 == 255);
    artfSceneFree(s);

    /* and it goes between vertices as light does: half of red and half of blue, in the frame's colors */
    s = plainScene();
    white = whiteMaterial(s);
    addAt(s, addSquare(s, 0xFFFF0000u, 0xFF0000FFu, 0xFF0000FFu, 0xFFFF0000u), white, 0, 0, 0, 1);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[mid], 16), 180, 6) && near_(CHANNEL(a[mid], 0), 180, 6) && CHANNEL(a[mid], 8) == 0);
    artfSceneFree(s);

    /* it multiplies the material, and its texture */
    s = plainScene();
    ArtfMaterialDesc half = {artfVec3(.25f, .25f, .25f), 0, 0, 1, artfVec3(0, 0, 0)};
    addAt(s, addSquare(s, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFF808080u, 0xFF808080u), artfSceneAddMaterial(s, &half), 0, 0, 0, 1);
    drawInto(r, s, a);
    CHECK(CHANNEL(a[mid], 8) < 128 && CHANNEL(a[mid], 8) > 80);
    artfSceneFree(s);

    /* opacity: over black, half of it */
    s = plainScene();
    white = whiteMaterial(s);
    ArtfMesh plain = addSquare(s, 0, 0, 0, 0);
    ArtfInstance front = addAt(s, plain, white, 0, 0, 0, 1);
    drawInto(r, s, a);
    const uint64_t solid = hashOf(a, (size_t)W * H);
    CHECK(a[mid] == 0xFFFFFFFFu);
    artfInstanceSetOpacity(s, front, .5f);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[mid], 8), 128, margin) && a[mid] >> 24 == 255);
    /* what opacity 1 and 0 come to is the same bytes as never having a layer, or having nothing */
    artfInstanceSetOpacity(s, front, 1.f);
    drawInto(r, s, a);
    CHECK(hashOf(a, (size_t)W * H) == solid);
    artfInstanceSetOpacity(s, front, 0.f);
    drawInto(r, s, a);
    bool nothing = true;
    for (size_t i = 0; i < (size_t)W * H; ++i) nothing = nothing && a[i] == 0xFF000000u;
    CHECK(nothing);
    artfSceneFree(s);

    /* layers go by distance, whatever order they were added in; what is solid is behind or hides them */
    for (int order = 0; order < 2; ++order) {
        s = plainScene();
        white = whiteMaterial(s);
        ArtfMesh red = addSquare(s, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u), blue = addSquare(s, 0xFF0000FFu, 0xFF0000FFu, 0xFF0000FFu, 0xFF0000FFu);
        ArtfInstance near1 = addAt(s, order ? blue : red, white, 0, 0, order ? 1.f : 0.f, 1), near2 = addAt(s, order ? red : blue, white, 0, 0, order ? 0.f : 1.f, 1);
        artfInstanceSetOpacity(s, near1, .5f);
        artfInstanceSetOpacity(s, near2, .5f);
        drawInto(r, s, a);
        CHECK(near_(CHANNEL(a[mid], 0), 128, margin) && near_(CHANNEL(a[mid], 16), 64, margin));
        artfSceneFree(s);
    }
    s = plainScene();
    white = whiteMaterial(s);
    ArtfMesh green = addSquare(s, 0xFF00FF00u, 0xFF00FF00u, 0xFF00FF00u, 0xFF00FF00u);
    ArtfInstance hidden = addAt(s, addSquare(s, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u), white, 0, 0, 0, 1);
    artfInstanceSetOpacity(s, hidden, .5f);
    addAt(s, green, white, 0, 0, 1, 1); /* in front of the layer: nothing of it shows */
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFF00FF00u);
    artfSceneFree(s);

    /* a fading object shows its nearest surface alone, not its insides */
    s = plainScene();
    white = whiteMaterial(s);
    ArtfTransform turned = artfTransformTrs(artfVec3(0, 0, 0), artfQuatAxisAngle(artfVec3(1, 1, 0), .7f), artfVec3(1, 1, 1));
    ArtfInstance cube = artfSceneAddInstance(s, artfSceneAddCube(s, 2), white, &turned);
    drawInto(r, s, a);
    artfInstanceSetOpacity(s, cube, .5f);
    drawInto(r, s, b);
    int off = 0;
    for (size_t i = 0; i < (size_t)W * H; ++i) off = artfMaxi(off, abs(CHANNEL(a[i], 8) / 2 - CHANNEL(b[i], 8)));
    CHECK(off <= margin);
    artfSceneFree(s);

    /* alpha in a vertex's color: a layer without being asked, with no more opacity than it says */
    s = plainScene();
    white = whiteMaterial(s);
    addAt(s, addSquare(s, 0x80FFFFFFu, 0x80FFFFFFu, 0x80FFFFFFu, 0x80FFFFFFu), white, 0, 0, 0, 1);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[mid], 8), 128, margin));
    artfSceneFree(s);

    /* no background: what the frame has is what the scene covered, premultiplied */
    s = plainScene();
    white = whiteMaterial(s);
    artfSceneSetBackground(s, artfVec3(1, 0, 0));
    artfSceneSetBackgroundAlpha(s, 0);
    ArtfInstance layer = addAt(s, addSquare(s, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFF0000u, 0xFFFF0000u), white, 0, 0, 0, 1);
    drawInto(r, s, a);
    CHECK(a[0] == 0 && a[(size_t)W * H - 1] == 0);
    CHECK(a[mid] >> 24 == 255);
    artfInstanceSetOpacity(s, layer, .5f);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[mid], 24), 128, margin));
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        const int alpha = CHANNEL(a[i], 24);
        if (CHANNEL(a[i], 16) > alpha + 1 || CHANNEL(a[i], 8) > alpha + 1 || CHANNEL(a[i], 0) > alpha + 1) { CHECK(!"past its alpha"); break; }
    }
    /* half a background is that much of its color, and of alpha */
    artfSceneSetBackgroundAlpha(s, .5f);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[0], 24), 128, 1) && near_(CHANNEL(a[0], 16), 128, 1) && CHANNEL(a[0], 8) == 0);
    artfSceneFree(s);

    /* an overlay over nothing keeps its own alpha, and adds to what is there */
    s = plainScene();
    artfSceneSetBackgroundAlpha(s, 0);
    uint32_t* over = artfCalloc((size_t)W * H, 4);
    for (int y = 0; y < 40; ++y)
        for (int x = 0; x < 40; ++x) over[y * W + x] = 0x80400000u;
    ArtfOverlay overlay = {over, W, H, 0, false, NULL, 0};
    artfRendererSetOverlay(r, &overlay);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[0], 24), 0x80, 1) && near_(CHANNEL(a[0], 16), 0x40, 1));
    CHECK(a[(size_t)100 * W + 100] == 0);
    artfRendererSetOverlay(r, NULL);
    artfFree(over);
    artfSceneFree(s);

    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

/* What a material can say about alpha and backs, on one backend. */
static void testMaterials(ArtfBackend backend, bool antialias) {
    ArtfRendererOptions o = options(backend, 0, false);
    o.antialias = antialias;
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    const size_t mid = (size_t)(H / 2) * W + W / 2;
    const int margin = backend == ARTF_BACKEND_VULKAN ? 3 : 2;
    /* a texture whose left half has alpha 0, and its right half 255; both white */
    static uint32_t px[64 * 64];
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) px[y * 64 + x] = x < 32 ? 0x00FFFFFFu : 0xFFFFFFFFu;
    const size_t left = (size_t)(H / 2) * W + W / 2 - 20, right = (size_t)(H / 2) * W + W / 2 + 20;

    /* opaque: alpha counts for nothing */
    ArtfScene* s = plainScene();
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), artfSceneAddTexture(s, px, 64, 64, 0), 0, 1, artfVec3(0, 0, 0)};
    ArtfMesh square = addSquare(s, 0, 0, 0, 0), under = addSquare(s, 0xFF00FF00u, 0xFF00FF00u, 0xFF00FF00u, 0xFF00FF00u);
    ArtfMaterial m = artfSceneAddMaterial(s, &d), plain = whiteMaterial(s);
    addAt(s, under, plain, 0, 0, -1, 2);
    addAt(s, square, m, 0, 0, 0, 1);
    drawInto(r, s, a);
    CHECK(a[left] == 0xFFFFFFFFu && a[right] == 0xFFFFFFFFu);
    CHECK(s->cutoutCount == 0 && s->translucentCount == 0);

    /* a mask: what is below shows through its holes, and the surface hides it elsewhere */
    d.alphaMode = ARTF_ALPHA_MASK;
    artfMaterialSet(s, m, &d);
    CHECK(s->cutoutCount == 1);
    drawInto(r, s, a);
    CHECK(a[left] == 0xFF00FF00u && a[right] == 0xFFFFFFFFu);

    /* blending by the texture's alpha, and by the material's own */
    d.alphaMode = ARTF_ALPHA_BLEND;
    d.transparency = .5f;
    artfMaterialSet(s, m, &d);
    CHECK(s->cutoutCount == 0 && s->translucentCount == 1);
    drawInto(r, s, a);
    CHECK(a[left] == 0xFF00FF00u);
    CHECK(near_(CHANNEL(a[right], 16), 128, margin) && near_(CHANNEL(a[right], 8), 255, margin));
    artfSceneFree(s);

    /* backs: not drawn, unless asked; then lit as a front */
    s = plainScene();
    ArtfMaterialDesc two = {artfVec3(.5f, .5f, .5f), 0, 0, 1, artfVec3(0, 0, 0)};
    artfSceneSetAmbient(s, artfVec3(1, 1, 1), artfVec3(0, 0, 0)); /* (a back would be dark if it were not turned) */
    const ArtfTransform turned = artfTransformTrs(artfVec3(0, 0, 0), artfQuatAxisAngle(artfVec3(0, 1, 0), 3.14159265f), artfVec3(1, 1, 1));
    ArtfMaterial back = artfSceneAddMaterial(s, &two);
    artfSceneAddInstance(s, square = addSquare(s, 0, 0, 0, 0), back, &turned);
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFF000000u);
    two.doubleSided = true;
    artfMaterialSet(s, back, &two);
    CHECK(s->cutoutCount == 1);
    drawInto(r, s, a);
    const uint32_t shown = a[mid];
    CHECK(CHANNEL(shown, 8) > 100);
    artfSceneFree(s);

    /* a material with a mask and an instance that fades: its holes stay as they were */
    s = plainScene();
    ArtfMaterialDesc cut = {artfVec3(1, 1, 1), artfSceneAddTexture(s, px, 64, 64, 0), 0, 1, artfVec3(0, 0, 0), ARTF_ALPHA_MASK, 0, 0, false};
    ArtfMaterial leaf = artfSceneAddMaterial(s, &cut);
    ArtfInstance i = addAt(s, addSquare(s, 0, 0, 0, 0), leaf, 0, 0, 0, 1);
    artfInstanceSetOpacity(s, i, .5f);
    drawInto(r, s, a);
    CHECK(a[left] == 0xFF000000u);
    CHECK(near_(CHANNEL(a[right], 8), 128, margin));
    artfSceneFree(s);

    artfFree(a);
    artfRendererFree(r);
}

/* A scene of everything at once, for what must not change with how it is drawn. */
static ArtfScene* layeredScene(void) {
    ArtfScene* s = richScene();
    ArtfMaterial white = whiteMaterial(s);
    ArtfInstance glass = addAt(s, artfSceneAddSphere(s, 1, 24, 32), white, 1.f, .4f, 1.5f, .9f);
    artfInstanceSetOpacity(s, glass, .45f);
    ArtfInstance fade = addAt(s, artfSceneAddTorus(s, 1, .3f, 32, 16), white, -1.2f, .8f, 1.5f, .7f);
    artfInstanceSetOpacity(s, fade, .7f);
    addAt(s, addSquare(s, 0x60FF2000u, 0xC020FF00u, 0xFF0020FFu, 0x30FFFF00u), white, 0, -.2f, 2.5f, .8f);
    addAt(s, addSquare(s, 0xFFFF2000u, 0xFF20FF00u, 0xFF0020FFu, 0xFFFFFF00u), white, 1.8f, 1.4f, 0, .6f);
    /* a fence, with holes and a back */
    static uint32_t bars[32 * 32];
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) bars[y * 32 + x] = (x / 4 + y / 4) & 1 ? 0xFFC08040u : 0x00FFFFFFu;
    ArtfMaterialDesc fence = {artfVec3(1, 1, 1), artfSceneAddTexture(s, bars, 32, 32, 0), .3f, .33f, artfVec3(0, 0, 0), ARTF_ALPHA_MASK, .5f, 0, true};
    const ArtfTransform t = artfTransformTrs(artfVec3(-2.2f, 0, 1), artfQuatAxisAngle(artfVec3(0, 1, 0), 2.6f), artfVec3(1, 1, 1));
    artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &fence), &t);
    return s;
}

static void testSamePixelsOfLayers(void) {
    uint32_t* frame = artfMalloc((size_t)W * H * 4);
    static const struct { int threads; ArtfSimd simd; } runs[] = {{1, ARTF_SIMD_AUTO}, {4, ARTF_SIMD_AUTO}, {3, ARTF_SIMD_AUTO}, {1, ARTF_SIMD_PORTABLE}, {4, ARTF_SIMD_PORTABLE}, {2, ARTF_SIMD_128}, {3, ARTF_SIMD_256}};
    for (int antialias = 0; antialias < 2; ++antialias)
        for (int clear = 0; clear < 2; ++clear) {
            ArtfScene* s = layeredScene();
            if (clear) artfSceneSetBackgroundAlpha(s, 0);
            uint64_t reference = 0;
            for (size_t i = 0; i < ARTF_COUNT(runs); ++i) {
                ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, runs[i].threads, false);
                o.simd = runs[i].simd;
                o.antialias = antialias != 0;
                ArtfRenderer* r = artfRendererNew(&o);
                drawInto(r, s, frame);
                const uint64_t h = hashOf(frame, (size_t)W * H);
                if (!i) reference = h;
                CHECK(h == reference);
                artfRendererFree(r);
            }
            /* what occlusion does never shows, with layers and samples too */
            artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, true);
            ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
            o.antialias = antialias != 0;
            ArtfRenderer* r = artfRendererNew(&o);
            drawInto(r, s, frame);
            CHECK(hashOf(frame, (size_t)W * H) == reference);
            artfRendererFree(r);
            artfSceneFree(s);
        }
    artfFree(frame);
}

static void testAntialias(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* hard = artfRendererNew(&o);
    if (!hard) return;
    o.antialias = true;
    ArtfRenderer* soft = artfRendererNew(&o);
    CHECK(soft != NULL);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);

    /* a white shape on nothing: without samples each pixel is in or out, with them an edge is between */
    ArtfScene* s = plainScene();
    ArtfMaterial white = whiteMaterial(s);
    artfSceneSetBackgroundAlpha(s, 0);
    ArtfTransform turned = artfTransformTrs(artfVec3(0, 0, 0), artfQuatAxisAngle(artfVec3(0, 0, 1), .4f), artfVec3(1, 1, 1));
    artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), white, &turned);
    drawInto(hard, s, a);
    drawInto(soft, s, b);
    int between = 0, inside = 0, moved = 0;
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        const int ha = CHANNEL(a[i], 24), sa = CHANNEL(b[i], 24);
        CHECK(ha == 0 || ha == 255);
        between += sa > 0 && sa < 255;
        inside += ha == 255 && sa == 255;
        moved += abs(ha - sa) > 192; /* (nothing can be as far off as that) */
        if (sa && sa < 255) { CHECK(sa == 64 || sa == 128 || sa == 191 || near_(sa, 64, 1) || near_(sa, 128, 1) || near_(sa, 191, 1)); }
        if (CHANNEL(b[i], 8) > sa + 1) { CHECK(!"past its alpha"); break; }
    }
    CHECK(between > 200);
    CHECK(inside > 2000);
    CHECK(moved == 0);
    artfSceneFree(s);

    /* over a solid, a surface's inside is what it was without samples */
    s = richScene();
    drawInto(hard, s, a);
    drawInto(soft, s, b);
    int same = 0;
    for (size_t i = 0; i < (size_t)W * H; ++i) same += near_(CHANNEL(a[i], 8), CHANNEL(b[i], 8), 2) && near_(CHANNEL(a[i], 16), CHANNEL(b[i], 16), 2);
    CHECK(same > W * H * 9 / 10);
    artfSceneFree(s);

    /* triangles that share an edge leave nothing between them, in any sample */
    s = plainScene();
    white = whiteMaterial(s);
    artfSceneSetBackgroundAlpha(s, 0);
    enum { N = 40 };
    static ArtfVertex v[(N + 1) * (N + 1)];
    static uint32_t idx[N * N * 6];
    uint32_t n = 0, state = 12345;
    for (int y = 0; y <= N; ++y)
        for (int x = 0; x <= N; ++x) {
            state = state * 1664525u + 1013904223u;
            const float jx = x && x < N ? (float)(state >> 8 & 255) / 700.f : 0, jy = y && y < N ? (float)(state >> 16 & 255) / 700.f : 0;
            ArtfVertex q = {((float)x + jx) / N * 40.f - 20.f, ((float)y + jy) / N * 40.f - 20.f, 0, 0, 0, 1, 0, 0, 0};
            v[y * (N + 1) + x] = q;
        }
    for (uint32_t y = 0; y < N; ++y)
        for (uint32_t x = 0; x < N; ++x) {
            const uint32_t p = y * (N + 1) + x, q = p + 1, d = p + N + 1, e = d + 1;
            idx[n++] = p; idx[n++] = q; idx[n++] = e;
            idx[n++] = p; idx[n++] = e; idx[n++] = d;
        }
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, false);
    addAt(s, artfSceneAddMesh(s, v, (N + 1) * (N + 1), idx, n), white, 0, 0, 0, 1);
    drawInto(soft, s, b);
    bool whole = true;
    for (size_t i = 0; i < (size_t)W * H; ++i) whole = whole && b[i] >> 24 == 255;
    CHECK(whole);
    artfSceneFree(s);

    /* what falls between pixels' centers is not lost: this sliver holds a row of samples and no center */
    s = plainScene();
    white = whiteMaterial(s);
    artfSceneSetBackgroundAlpha(s, 0);
    const float thin = .002f;
    const ArtfVertex sliver[4] = {{-1, -thin, 0, 0, 0, 1, 0, 0, 0}, {1, -thin, 0, 0, 0, 1, 0, 0, 0}, {1, thin, 0, 0, 0, 1, 0, 0, 0}, {-1, thin, 0, 0, 0, 1, 0, 0, 0}};
    const uint32_t quad[6] = {0, 1, 2, 0, 2, 3};
    addAt(s, artfSceneAddMesh(s, sliver, 4, quad, 6), white, 0, .01024f, 0, 1);
    drawInto(hard, s, a);
    drawInto(soft, s, b);
    int touched = 0, touchedHard = 0;
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        touched += b[i] != 0;
        touchedHard += a[i] != 0;
    }
    CHECK(touched > 20);
    CHECK(touchedHard == 0);
    artfSceneFree(s);

    /* a new size, and a scene with layers */
    s = layeredScene();
    artfRendererResize(soft, W / 2, H / 2);
    CHECK(artfRendererDraw(soft, s));
    CHECK(artfRendererDraw(soft, s));
    CHECK(artfRendererReadPixels(soft, b));
    artfRendererResize(soft, W, H);
    drawInto(soft, s, b);
    artfSceneFree(s);

    artfFree(a);
    artfFree(b);
    artfRendererFree(hard);
    artfRendererFree(soft);
}

/* The two backends draw these the same. */
static void testLayersBothBackends(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* probe = artfRendererNew(&o);
    if (!probe) return;
    artfRendererFree(probe);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    for (int antialias = 0; antialias < 2; ++antialias)
        for (int clear = 0; clear < 2; ++clear) {
            o = options(ARTF_BACKEND_VULKAN, 0, false);
            o.antialias = antialias != 0;
            ArtfRenderer* gpu = artfRendererNew(&o);
            o.backend = ARTF_BACKEND_SOFTWARE;
            ArtfRenderer* cpu = artfRendererNew(&o);
            ArtfScene* s = layeredScene();
            if (clear) artfSceneSetBackgroundAlpha(s, 0);
            drawInto(gpu, s, a);
            drawInto(cpu, s, b);
            const double far = differentShare(a, b, 24);
            int alphaOff = 0;
            for (size_t i = 0; i < (size_t)W * H; ++i) alphaOff += abs(CHANNEL(a[i], 24) - CHANNEL(b[i], 24)) > 24;
            printf("layers, %s, %s: pixels that differ by more than 24 of 255: %.2f%%, in alpha %.2f%%\n", antialias ? "antialiased" : "hard edges", clear ? "no background" : "a background", far * 100, (double)alphaOff / (W * H) * 100);
            CHECK(far < .02);
            CHECK(alphaOff < W * H / 50);
            /* occlusion, on the GPU too, with samples or without */
            artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, true);
            drawInto(gpu, s, b);
            CHECK(differentShare(a, b, 0) < .0001);
            artfRendererFree(gpu);
            artfRendererFree(cpu);
            artfSceneFree(s);
        }
    artfFree(a);
    artfFree(b);
}

/* ---- pictures that must not change ----
 * The CPU's bytes are the same on every processor, so a picture can be held to a number: a change to any
 * of these is a change to what programs draw, and should be one that was meant. */

static uint64_t pictureOf(ArtfScene* s, bool antialias) {
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, false);
    artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, false);
    artfSceneSetFeature(s, ARTF_FEATURE_CONES, true);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    o.antialias = antialias;
    ArtfRenderer* r = artfRendererNew(&o);
    uint32_t* frame = artfMalloc((size_t)W * H * 4);
    drawInto(r, s, frame);
    const uint64_t h = hashOf(frame, (size_t)W * H);
    artfFree(frame);
    artfRendererFree(r);
    artfSceneFree(s);
    return h;
}

static void testPictures(void) {
    const uint64_t rich = pictureOf(richScene(), false), layered = pictureOf(layeredScene(), false), smooth = pictureOf(layeredScene(), true);
    ArtfScene* s = layeredScene();
    artfSceneSetBackgroundAlpha(s, 0);
    const uint64_t clear = pictureOf(s, true);
    CHECK(rich == 0x818E1B446A10E12Bull);
    CHECK(layered == 0x3A886E17A40AE14ull);
    CHECK(smooth == 0x8DB1290E706CF72Full);
    CHECK(clear == 0x3D52D43351D2EB2Full);
}

/* ---- what a program can get wrong, and what it can ask for ---- */

static double meanOf(const uint32_t* frame, int x, int y, int shift);
static void put32le(uint8_t* p, uint32_t v);
static ArtfMesh addStrip(ArtfScene* s);
static void bendStrip(ArtfScene* s, ArtfInstance i, float angle);
static void drawSamples(ArtfRenderer* r, ArtfScene* s, int samples, uint32_t* frame);

static int logged;
static void countLog(void* user, const char* message) { (void)message; ++*(int*)user; }

static void testHandles(void) {
    artfSetLog(countLog, &logged);
    ArtfScene* s = plainScene();
    ArtfMesh m = addSquare(s, 0, 0, 0, 0);
    ArtfInstance a = addAt(s, m, 0, 0, 0, 0, 1), b = addAt(s, m, 0, 1, 0, 0, 1);
    artfSceneRemoveInstance(s, a);
    const ArtfTransform t = artfTransformAt(9, 9, 9, 1);
    /* a handle that is gone does nothing, even when its place is taken */
    ArtfInstance c = addAt(s, m, 0, 2, 0, 0, 1);
    CHECK(c != a);
    const uint32_t before = artfSceneInstanceCount(s);
    logged = 0;
    artfInstanceSetTransform(s, a, &t);
    artfInstanceSetOpacity(s, a, .5f);
    artfInstanceSetMaterial(s, a, 0);
    artfSceneRemoveInstance(s, a);
    artfSceneRemoveInstance(s, 0x00FFFFFFu);
    artfInstanceSetTransform(s, 0, &t);
    CHECK(logged == 6);
    CHECK(artfSceneInstanceCount(s) == before);
    CHECK(s->translucentCount == 0);
    for (size_t i = 0; i < s->instances.len; ++i) CHECK(s->instances.items[i].m[3] != 9.f);
    artfInstanceSetTransform(s, b, &t);
    CHECK(logged == 6);
    artfSceneFree(s);
    artfSetLog(NULL, NULL);

    ArtfMaterialDesc d = artfMaterialDefaults();
    CHECK(d.baseColor.x == 1.f && d.specular == 1.f && d.roughness == .5f && d.alphaMode == ARTF_ALPHA_OPAQUE);
    ArtfRendererOptions o = artfRendererDefaults(W, H);
    CHECK(o.width == W && o.height == H && o.backend == ARTF_BACKEND_AUTO && !o.antialias);
}

/* Fog, film, spots: on both backends, the same way. */
static void testLook(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    const size_t mid = (size_t)(H / 2) * W + W / 2;
    const int margin = 4;

    /* fog: at distance 5 with density .2, air 1 keeps 1 / 2.5 of what the surface shows */
    ArtfScene* s = plainScene();
    ArtfMaterial white = whiteMaterial(s);
    addAt(s, addSquare(s, 0, 0, 0, 0), white, 0, 0, 0, 1);
    artfSceneSetFog(s, artfVec3(0, 0, 0), .2f);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[mid], 8), (int)(sqrtf(.4f) * 255.f), margin));
    ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 16, 0};
    artfSceneSetTrace(s, &rays);
    for (int i = 0; i < 16; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, a));
    CHECK(near_(CHANNEL(a[mid], 8), (int)(sqrtf(.4f) * 255.f), margin));
    artfSceneSetTrace(s, NULL);
    artfSceneSetFog(s, artfVec3(0, 0, 0), 0);

    /* film: light of 1, exposed at 1, comes to the curve's .8 */
    artfSceneSetExposure(s, 1.f);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[mid], 8), (int)(sqrtf(2.54f / 3.16f) * 255.f), margin));
    artfSceneSetTrace(s, &rays);
    for (int i = 0; i < 16; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, a));
    CHECK(near_(CHANNEL(a[mid], 8), (int)(sqrtf(2.54f / 3.16f) * 255.f), margin));
    artfSceneFree(s);

    /* a spot: full in its cone, nothing past it, and the same point light without one */
    s = plainScene();
    artfSceneSetAmbient(s, artfVec3(0, 0, 0), artfVec3(0, 0, 0));
    white = whiteMaterial(s);
    addAt(s, addSquare(s, 0, 0, 0, 0), white, 0, 0, 0, 2);
    ArtfLight l = artfSceneAddLight(s, artfVec3(0, 0, 3), artfVec3(1, 1, 1), 100.f);
    artfLightSetSpot(s, l, artfVec3(0, 0, -1), .1f, .2f);
    const size_t aside = (size_t)(H / 2) * W + W / 2 + 40;
    for (int traced = 0; traced < 2; ++traced) {
        if (traced) artfSceneSetTrace(s, &rays);
        for (int i = 0; i < 16; ++i) CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, a));
        CHECK(CHANNEL(a[mid], 8) > 230);
        CHECK(CHANNEL(a[aside], 8) == 0);
    }
    artfSceneSetTrace(s, NULL);
    artfLightSet(s, l, artfVec3(0, 0, 3), artfVec3(1, 1, 1), 100.f);
    drawInto(r, s, a);
    CHECK(CHANNEL(a[aside], 8) > 200);
    artfSceneFree(s);

    /* a soft sun: an edge that was hard goes through shades */
    s = plainScene();
    white = whiteMaterial(s);
    artfSceneSetAmbient(s, artfVec3(.1f, .1f, .1f), artfVec3(.1f, .1f, .1f));
    artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(.9f, .9f, .9f));
    setCamera(s, 0, 6, .01f, 0, 0, 0);
    const ArtfTransform ground = artfTransformAt(0, 0, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 30, 1), white, &ground);
    const ArtfTransform bar = artfTransformTrs(artfVec3(0, 2, 0), artfQuatAxisAngle(artfVec3(1, 0, 0), -1.5708f), artfVec3(1, 8, 1));
    artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), white, &bar);
    artfSceneSetSun(s, artfVec3(-.5f, -1, 0), artfVec3(.9f, .9f, .9f));
    int shades[2];
    for (int soft = 0; soft < 2; ++soft) {
        artfSceneSetSunSize(s, soft ? .15f : 0.f);
        ArtfTraceDesc many = {ARTF_TRACE_RAYS, 1, 48, 0};
        artfSceneSetTrace(s, &many);
        for (int i = 0; i < 48; ++i) CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, a));
        shades[soft] = 0;
        for (int x = 0; x < W; ++x) {
            const int v = CHANNEL(a[(size_t)(H - 8) * W + x], 8);
            shades[soft] += v > 110 && v < 215;
        }
    }
    CHECK(shades[1] > shades[0] + 6);
    artfSceneFree(s);

    /* glass bends: a ball between the eye and a wall, red to the left and blue to the right, turns it around */
    s = plainScene();
    artfSceneSetBackground(s, artfVec3(0, 0, 0));
    white = whiteMaterial(s);
    addAt(s, addSquare(s, 0xFFFF0000u, 0xFF0000FFu, 0xFF0000FFu, 0xFFFF0000u), white, 0, 0, -6, 12);
    ArtfMaterialDesc glass = artfMaterialDefaults();
    glass.alphaMode = ARTF_ALPHA_BLEND;
    glass.transparency = 1.f;
    glass.specular = 0;
    glass.roughness = 1.f;
    addAt(s, artfSceneAddSphere(s, 1, 48, 64), artfSceneAddMaterial(s, &glass), 0, 0, 0, 1);
    ArtfTraceDesc through = {ARTF_TRACE_RAYS, 1, 8, 0};
    artfSceneSetTrace(s, &through);
    for (int i = 0; i < 8; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, a));
    const int x = W / 2 - 15;
    CHECK(meanOf(a, x, H / 2, 16) > meanOf(a, x, H / 2, 0)); /* straight: more red on the left */
    glass.ior = 1.5f;
    artfMaterialSet(s, s->instances.items[1].material, &glass);
    for (int i = 0; i < 8; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, a));
    CHECK(meanOf(a, x, H / 2, 0) > meanOf(a, x, H / 2, 16));
    artfSceneFree(s);

    artfFree(a);
    artfRendererFree(r);
}

/* A renderer that goes while its work is in flight, many times, and a cache that survives. */
static void testLifetimes(void) {
    ArtfScene* s = layeredScene();
    for (int i = 0; i < 6; ++i) {
        ArtfRendererOptions o = options(i & 1 ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.antialias = i & 2;
        if (i == 3) { ArtfTraceDesc d = {ARTF_TRACE_PATHS, 1, 0, 0}; artfSceneSetTrace(s, &d); }
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        for (int k = 0; k < 3; ++k) CHECK(artfRendererDraw(r, s)); /* (nothing waited for) */
        artfRendererFree(r);
    }
    artfSceneFree(s);

    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    o.pipelineCache = "artful-test-pipelines.bin";
    remove(o.pipelineCache);
    for (int i = 0; i < 2; ++i) {
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) return;
        s = richScene();
        CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererDraw(r, s));
        ArtfFrameStats st;
        artfRendererStats(r, &st);
        CHECK(st.gpuMs > 0 && st.gpuMs < 1000);
        artfRendererFree(r);
        artfSceneFree(s);
        FILE* f = fopen(o.pipelineCache, "rb");
        CHECK(f != NULL);
        if (f) fclose(f);
    }
    remove(o.pipelineCache);
}

/* ---- what things are made of ---- */

/* A square with v going down the picture, as a file's textures have it. */
static ArtfMesh addPictureSquare(ArtfScene* s) {
    const ArtfVertex v[4] = {{-1, -1, 0, 0, 0, 1, 0, 1, 0}, {1, -1, 0, 0, 0, 1, 1, 1, 0}, {1, 1, 0, 0, 0, 1, 1, 0, 0}, {-1, 1, 0, 0, 0, 1, 0, 0, 0}};
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    return artfSceneAddMesh(s, v, 4, idx, 6);
}

static ArtfTexture oneColor(ArtfScene* s, uint32_t bgra) {
    static uint32_t px[4 * 4];
    for (int i = 0; i < 16; ++i) px[i] = bgra;
    return artfSceneAddTexture(s, px, 4, 4, 0);
}

/* Draws, and gives the middle's pixel: by triangles, and then by rays. */
static uint32_t middleOf(ArtfRenderer* r, ArtfScene* s, uint32_t* frame, bool traced) {
    const ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 8, 1};
    artfSceneSetTrace(s, traced ? &rays : NULL);
    for (int i = 0; i < 9; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, frame));
    return frame[(size_t)(H / 2) * W + W / 2];
}

static void testPhysical(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    for (int traced = 0; traced < 2; ++traced) {
        /* a metal in a white sky, seen head on, shows its own color */
        ArtfScene* s = plainScene();
        ArtfMaterialDesc d = artfMaterialDefaults();
        d.baseColor = artfVec3(1, .5f, .25f);
        d.metallic = 1.f;
        d.roughness = .2f;
        ArtfMaterial metal = artfSceneAddMaterial(s, &d);
        addAt(s, addSquare(s, 0, 0, 0, 0), metal, 0, 0, 0, 1);
        uint32_t p = middleOf(r, s, a, traced);
        CHECK(near_(CHANNEL(p, 16), 255, 3) && near_(CHANNEL(p, 8), (int)(sqrtf(.5f) * 255.f), 3) && near_(CHANNEL(p, 0), (int)(sqrtf(.25f) * 255.f), 3));
        /* and what is not metal, and black, reflects 4% of it */
        d.baseColor = artfVec3(0, 0, 0);
        d.metallic = 0;
        artfMaterialSet(s, metal, &d);
        p = middleOf(r, s, a, traced);
        CHECK(near_(CHANNEL(p, 8), (int)(sqrtf(.04f) * 255.f), 3));
        artfSceneFree(s);

        /* a highlight, on black: the smoother, the brighter its peak */
        int peaks[2];
        for (int smooth = 0; smooth < 2; ++smooth) {
            s = plainScene();
            artfSceneSetAmbient(s, artfVec3(0, 0, 0), artfVec3(0, 0, 0));
            artfSceneSetSun(s, artfVec3(0, 0, -1), artfVec3(1, 1, 1));
            d = artfMaterialDefaults();
            d.baseColor = artfVec3(0, 0, 0);
            d.roughness = smooth ? .2f : .7f;
            addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
            peaks[smooth] = CHANNEL(middleOf(r, s, a, traced), 8);
            artfSceneFree(s);
        }
        CHECK(peaks[1] > peaks[0] + 30);

        /* a normal map: a plain square, with the sun above it, tells up from down */
        int bent[3];
        const uint32_t maps[3] = {0xFF8080FFu, 0xFF80FFFFu, 0xFF8000FFu}; /* flat; +y, up the picture; -y */
        for (int i = 0; i < 3; ++i) {
            s = plainScene();
            artfSceneSetAmbient(s, artfVec3(0, 0, 0), artfVec3(0, 0, 0));
            artfSceneSetSun(s, artfVec3(0, -1, -.2f), artfVec3(1, 1, 1));
            d = artfMaterialDefaults();
            d.specular = 0;
            d.normalMap = oneColor(s, maps[i]);
            addAt(s, addPictureSquare(s), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
            bent[i] = CHANNEL(middleOf(r, s, a, traced), 8);
            artfSceneFree(s);
        }
        CHECK(bent[1] > bent[0] + 40);
        CHECK(bent[2] < bent[0]);

        /* a surface map: blue says metal, and green multiplies roughness */
        s = plainScene();
        d = artfMaterialDefaults();
        d.baseColor = artfVec3(1, .5f, .25f);
        d.metallic = 1.f;
        d.surfaceMap = oneColor(s, 0xFF000000u); /* (no blue: not metal) */
        addAt(s, addPictureSquare(s), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
        p = middleOf(r, s, a, traced);
        CHECK(CHANNEL(p, 16) > 240); /* (the same color, now diffuse) */
        artfSceneFree(s);

        /* an occlusion map keeps light from all around out, and not the sun's */
        s = plainScene();
        d = artfMaterialDefaults();
        d.specular = 0;
        d.occlusionMap = oneColor(s, 0xFF400000u); /* (a quarter open) */
        addAt(s, addPictureSquare(s), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
        p = middleOf(r, s, a, traced);
        CHECK(near_(CHANNEL(p, 8), (int)(sqrtf(.25f) * 255.f), 4));
        artfSceneFree(s);
    }
    artfFree(a);
    artfRendererFree(r);
}

/* ---- shadows ---- */

/* A floor, a ball over it and a wall of bars, as a test of what maps can get wrong. */
static ArtfScene* shadowScene(void) {
    ArtfScene* s = artfSceneNew();
    setCamera(s, 2, 5, 9, 0, 0, 0);
    artfSceneSetBackground(s, artfVec3(0, 0, 0));
    artfSceneSetAmbient(s, artfVec3(.1f, .1f, .1f), artfVec3(.1f, .1f, .1f));
    artfSceneSetSun(s, artfVec3(-.5f, -1, -.3f), artfVec3(.9f, .9f, .9f));
    ArtfMaterialDesc matte = {artfVec3(.8f, .8f, .8f), 0, 0, 1, artfVec3(0, 0, 0)};
    const ArtfMaterial m = artfSceneAddMaterial(s, &matte);
    ArtfTransform t = artfTransformAt(0, -1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 24, 1), m, &t);
    addAt(s, artfSceneAddSphere(s, 1, 32, 48), m, 0, .6f, 0, 1);
    addAt(s, artfSceneAddCube(s, 1.2f), m, -2.5f, 0, 1, 1);
    return s;
}

static void testShadows(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);

    /* the maps come close to what rays say; with none, nothing is dark */
    ArtfScene* s = shadowScene();
    drawInto(r, s, a);
    const ArtfShadowDesc desc = {1024, 40};
    artfSceneSetShadows(s, &desc);
    drawInto(r, s, b);
    int darker = 0;
    for (size_t i = 0; i < (size_t)W * H; ++i) darker += CHANNEL(b[i], 8) + 8 < CHANNEL(a[i], 8);
    CHECK(darker > 600);
    ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 32, 0};
    artfSceneSetTrace(s, &rays);
    drawSamples(r, s, 32, a);
    artfSceneSetTrace(s, NULL);
    double far = differentShare(a, b, 40);
    printf("the sun's shadows against rays, pixels that differ by more than 40 of 255: %.2f%%\n", far * 100);
    CHECK(far < .015);
    artfSceneFree(s);

    /* a spot, and a point light: what stands next to them throws the same shadow as a ray says */
    for (int kind = 0; kind < 2; ++kind) {
        s = shadowScene();
        artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(0, 0, 0));
        ArtfLight l = artfSceneAddLight(s, artfVec3(2.5f, 4, 1), artfVec3(2, 2, 2), 20.f);
        if (kind) artfLightSetSpot(s, l, artfVec3(-.45f, -1, -.2f), .5f, .8f);
        artfLightSetShadow(s, l, true);
        artfSceneSetShadows(s, &desc);
        drawInto(r, s, b);
        artfSceneSetTrace(s, &rays);
        drawSamples(r, s, 32, a);
        artfSceneSetTrace(s, NULL);
        far = differentShare(a, b, 40);
        printf("%s against rays, pixels that differ by more than 40 of 255: %.2f%%\n", kind ? "a spot" : "a point light", far * 100);
        CHECK(far < .02);
        artfSceneFree(s);
    }

    /* holes let light through: half a square throws half a shadow, as rays say */
    s = artfSceneNew();
    setCamera(s, 0, 8, .01f, 0, 0, 0);
    artfSceneSetBackground(s, artfVec3(0, 0, 0));
    artfSceneSetAmbient(s, artfVec3(0, 0, 0), artfVec3(0, 0, 0));
    artfSceneSetSun(s, artfVec3(-.8f, -1, 0), artfVec3(1, 1, 1));
    ArtfMaterialDesc matte = {artfVec3(1, 1, 1), 0, 0, 1, artfVec3(0, 0, 0)};
    ArtfTransform t = artfTransformAt(0, 0, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 30, 1), artfSceneAddMaterial(s, &matte), &t);
    static uint32_t half[64 * 64];
    for (int i = 0; i < 64 * 64; ++i) half[i] = i % 64 < 32 ? 0x00FFFFFFu : 0xFFFFFFFFu;
    ArtfMaterialDesc cut = {artfVec3(1, 1, 1), artfSceneAddTexture(s, half, 64, 64, 0), 0, 1, artfVec3(0, 0, 0), ARTF_ALPHA_MASK, 0, 0, true};
    const ArtfTransform flat = artfTransformTrs(artfVec3(0, 1, 0), artfQuatAxisAngle(artfVec3(1, 0, 0), -1.5708f), artfVec3(1.5f, 1.5f, 1));
    artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &cut), &flat);
    artfSceneSetShadows(s, &desc);
    drawInto(r, s, b);
    artfSceneSetTrace(s, &rays);
    drawSamples(r, s, 32, a);
    artfSceneSetTrace(s, NULL);
    far = differentShare(a, b, 40);
    printf("a shadow with holes against rays, pixels that differ by more than 40 of 255: %.2f%%\n", far * 100);
    CHECK(far < .01);
    artfSceneFree(s);

    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

static void testShadowsBothBackends(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (!gpu) return;
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    const ArtfShadowDesc desc = {1024, 40};
    for (int kind = 0; kind < 3; ++kind) {
        ArtfScene* s = shadowScene();
        if (kind) {
            ArtfLight l = artfSceneAddLight(s, artfVec3(2.5f, 4, 1), artfVec3(1, 1, 1), 20.f);
            if (kind == 2) artfLightSetSpot(s, l, artfVec3(-.45f, -1, -.2f), .5f, .8f);
            artfLightSetShadow(s, l, true);
        }
        artfSceneSetShadows(s, &desc);
        drawInto(gpu, s, a);
        drawInto(cpu, s, b);
        const double far = differentShare(a, b, 24);
        printf("shadows, %d: pixels that differ by more than 24 of 255: %.2f%%\n", kind, far * 100);
        CHECK(far < .02);
        artfSceneFree(s);
    }
    artfFree(a);
    artfFree(b);
    artfRendererFree(gpu);
    artfRendererFree(cpu);
}

static void testShadowBytes(void) {
    ArtfScene* s = shadowScene();
    const ArtfShadowDesc desc = {512, 40};
    artfSceneSetShadows(s, &desc);
    ArtfLight l = artfSceneAddLight(s, artfVec3(2.5f, 4, 1), artfVec3(1, 1, 1), 20.f);
    artfLightSetShadow(s, l, true);
    /* more than fits gets none, and says so */
    artfSetLog(countLog, &logged);
    logged = 0;
    for (int i = 0; i < 3; ++i) artfLightSetShadow(s, artfSceneAddLight(s, artfVec3((float)i, 3, 0), artfVec3(.2f, .2f, .2f), 9.f), true);
    uint32_t* frame = artfMalloc((size_t)W * H * 4);
    static const struct { int threads; ArtfSimd simd; } runs[] = {{1, ARTF_SIMD_AUTO}, {4, ARTF_SIMD_AUTO}, {1, ARTF_SIMD_PORTABLE}, {3, ARTF_SIMD_128}};
    uint64_t reference = 0;
    for (size_t i = 0; i < ARTF_COUNT(runs); ++i) {
        ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, runs[i].threads, false);
        o.simd = runs[i].simd;
        ArtfRenderer* r = artfRendererNew(&o);
        drawInto(r, s, frame);
        const uint64_t h = hashOf(frame, (size_t)W * H);
        if (!i) reference = h;
        CHECK(h == reference);
        artfRendererFree(r);
    }
    CHECK(logged > 0);
    artfSetLog(NULL, NULL);
    artfFree(frame);
    artfSceneFree(s);
}

/* ---- materials of your own ---- */

/* The program's own C function, as a material is meant to be written: stripes across u. */
static ArtfExpr stripes(ArtfGraph* g, float count) {
    ArtfExpr x = artfExprFract(g, artfExprMul(g, artfExprX(g, artfExprUv(g)), artfExprNumber(g, count)));
    return artfExprIf(g, artfExprSub(g, x, artfExprNumber(g, .5f)), artfExprConst(g, 1, 1, 1, 1), artfExprConst(g, 0, 0, 0, 1));
}

static ArtfMaterial graphMaterial(ArtfScene* s, ArtfGraph* g, ArtfAlphaMode mode) {
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), 0, 0, 1, artfVec3(0, 0, 0)};
    d.alphaMode = mode;
    d.graph = artfSceneAddGraph(s, g);
    artfGraphFree(g);
    return artfSceneAddMaterial(s, &d);
}

static void testGraphs(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    const size_t mid = (size_t)(H / 2) * W + W / 2;
    for (int traced = 0; traced < 2; ++traced) {
        /* a color by position on the surface: drawn, and traced, stripes where they should be */
        ArtfScene* s = plainScene();
        ArtfGraph* g = artfGraphNew();
        artfGraphSetOutput(g, ARTF_OUT_COLOR, stripes(g, 2.f));
        addAt(s, addSquare(s, 0, 0, 0, 0), graphMaterial(s, g, ARTF_ALPHA_OPAQUE), 0, 0, 0, 2);
        (void)middleOf(r, s, a, traced);
        /* the square spans 146 pixels: at a quarter of u it is white, at three eighths still, past a half black */
        const size_t row = (size_t)(H / 2) * W;
        CHECK(CHANNEL(a[row + W / 2 - 73 + 26], 8) == 0 && CHANNEL(a[row + W / 2 - 73 + 55], 8) > 240 && CHANNEL(a[row + W / 2 - 73 + 100], 8) == 0);
        artfSceneFree(s);

        /* a parameter: changes what is drawn, with no new shader */
        s = plainScene();
        g = artfGraphNew();
        artfGraphSetOutput(g, ARTF_OUT_COLOR, artfExprConst(g, 0, 0, 0, 1));
        artfGraphSetOutput(g, ARTF_OUT_EMISSIVE, artfExprParam(g, 1));
        ArtfMaterial m = graphMaterial(s, g, ARTF_ALPHA_OPAQUE);
        addAt(s, addSquare(s, 0, 0, 0, 0), m, 0, 0, 0, 1);
        artfMaterialSetParam(s, m, 1, .25f, 0, 0, 1);
        uint32_t p = middleOf(r, s, a, traced);
        CHECK(near_(CHANNEL(p, 16), 128, 3) && CHANNEL(p, 8) == 0);
        artfMaterialSetParam(s, m, 1, 0, 1, 0, 1);
        p = middleOf(r, s, a, traced);
        CHECK(CHANNEL(p, 16) == 0 && CHANNEL(p, 8) > 250);
        artfSceneFree(s);

        /* sine and cosine agree with the library's, near enough */
        s = plainScene();
        g = artfGraphNew();
        ArtfExpr e = artfExprSin(g, artfExprNumber(g, 2.f));
        ArtfExpr c = artfExprCos(g, artfExprNumber(g, 2.f));
        artfGraphSetOutput(g, ARTF_OUT_COLOR, artfExprConst(g, 0, 0, 0, 1));
        artfGraphSetOutput(g, ARTF_OUT_EMISSIVE, artfExprVec(g, e, artfExprMul(g, c, c), artfExprNumber(g, 0)));
        addAt(s, addSquare(s, 0, 0, 0, 0), graphMaterial(s, g, ARTF_ALPHA_OPAQUE), 0, 0, 0, 1);
        p = middleOf(r, s, a, traced);
        CHECK(near_(CHANNEL(p, 16), (int)(sqrtf(sinf(2.f)) * 255.f), 3) && near_(CHANNEL(p, 8), (int)(fabsf(cosf(2.f)) * 255.f), 3));
        artfSceneFree(s);

        /* alpha, roughness and metal, and a texture */
        s = plainScene();
        g = artfGraphNew();
        artfGraphSetOutput(g, ARTF_OUT_ALPHA, artfExprNumber(g, .5f));
        artfGraphSetOutput(g, ARTF_OUT_COLOR, artfExprTexture(g, oneColor(s, 0xFF808080u), artfExprUv(g)));
        addAt(s, addSquare(s, 0, 0, 0, 0), graphMaterial(s, g, ARTF_ALPHA_BLEND), 0, 0, 0, 1);
        const ArtfTraceDesc many = {ARTF_TRACE_RAYS, 1, 64, 1};
        artfSceneSetTrace(s, traced ? &many : NULL);
        for (int i = 0; i < (traced ? 64 : 3); ++i) CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, a));
        CHECK(near_((int)meanOf(a, W / 2, H / 2, 8), 64, 12));
        artfSceneFree(s);
        (void)mid;
    }
    artfFree(a);
    artfRendererFree(r);
}

/* One definition, on both backends and on all kernels. */
static void testGraphsBothBackends(void) {
    ArtfScene* s = shadowScene();
    ArtfGraph* g = artfGraphNew();
    ArtfExpr p = artfExprPosition(g);
    ArtfExpr wave = artfExprSin(g, artfExprAdd(g, artfExprMul(g, artfExprX(g, p), artfExprNumber(g, 3)), artfExprParam(g, 0)));
    artfGraphSetOutput(g, ARTF_OUT_COLOR, artfExprMix(g, artfExprConst(g, .1f, .2f, .8f, 1), artfExprConst(g, .9f, .7f, .1f, 1), artfExprAdd(g, artfExprMul(g, wave, artfExprNumber(g, .5f)), artfExprNumber(g, .5f))));
    artfGraphSetOutput(g, ARTF_OUT_ROUGHNESS, artfExprFract(g, artfExprZ(g, p)));
    ArtfMaterial m = graphMaterial(s, g, ARTF_ALPHA_OPAQUE);
    artfInstanceSetMaterial(s, 1, m);
    artfMaterialSetParam(s, m, 0, 1.f, 0, 0, 0);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    static const struct { int threads; ArtfSimd simd; } runs[] = {{1, ARTF_SIMD_AUTO}, {4, ARTF_SIMD_AUTO}, {1, ARTF_SIMD_PORTABLE}, {3, ARTF_SIMD_128}};
    uint64_t reference = 0;
    for (size_t i = 0; i < ARTF_COUNT(runs); ++i) {
        ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, runs[i].threads, false);
        o.simd = runs[i].simd;
        ArtfRenderer* r = artfRendererNew(&o);
        drawInto(r, s, b);
        const uint64_t h = hashOf(b, (size_t)W * H);
        if (!i) reference = h;
        CHECK(h == reference);
        artfRendererFree(r);
    }
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        drawInto(gpu, s, a);
        double far = differentShare(a, b, 24);
        printf("a material in C, pixels that differ by more than 24 of 255: %.2f%%\n", far * 100);
        CHECK(far < .02);
        /* a graph added later: new shaders, and the old ones go on being right */
        ArtfGraph* later = artfGraphNew();
        artfGraphSetOutput(later, ARTF_OUT_COLOR, artfExprConst(later, 1, 0, 0, 1));
        artfInstanceSetMaterial(s, 2, graphMaterial(s, later, ARTF_ALPHA_OPAQUE));
        drawInto(gpu, s, a);
        ArtfRendererOptions c = options(ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* cpu = artfRendererNew(&c);
        drawInto(cpu, s, b);
        CHECK(differentShare(a, b, 24) < .02);
        artfRendererFree(cpu);
        artfRendererFree(gpu);
    }
    artfFree(a);
    artfFree(b);
    artfSceneFree(s);
}

/* ---- blocks ---- */

/* A KTX2 file of one level, of that format, holding these blocks. */
static size_t makeKtx2(uint8_t* out, uint32_t format, uint32_t width, uint32_t height, const uint8_t* blocks, size_t bytes) {
    static const uint8_t mark[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, 13, 10, 26, 10};
    memset(out, 0, 104);
    memcpy(out, mark, 12);
    put32le(out + 12, format);
    put32le(out + 16, 1);
    put32le(out + 20, width);
    put32le(out + 24, height);
    put32le(out + 36, 1);
    put32le(out + 40, 1);
    put32le(out + 80, 104);
    put32le(out + 88, (uint32_t)bytes);
    put32le(out + 96, (uint32_t)bytes);
    memcpy(out + 104, blocks, bytes);
    return 104 + bytes;
}

static void testBlocks(void) {
    /* what is made and turned back stays close to what it was */
    enum { SIDE = 32 };
    static uint32_t px[SIDE * SIDE];
    for (int y = 0; y < SIDE; ++y)
        for (int x = 0; x < SIDE; ++x) px[y * SIDE + x] = (uint32_t)(255 - x * 4) << 24 | (uint32_t)(x * 8) << 16 | (uint32_t)(y * 8) << 8 | (uint32_t)((x + y) * 4);
    for (int kind = 0; kind < 4; ++kind) {
        ArtfScene* s = artfSceneNew();
        static uint32_t opaque[SIDE * SIDE];
        for (int i = 0; i < SIDE * SIDE; ++i) opaque[i] = px[i] | 0xFF000000u;
        const ArtfTexture id = artfSceneAddTexture(s, kind & 1 ? px : opaque, SIDE, SIDE, 0);
        const ArtfTextureData* t = &s->textures.items[id];
        uint32_t format, offsets[16];
        size_t size;
        uint8_t* blocks = artfTextureEncode(t, kind >= 2, &format, &size, offsets);
        CHECK(size < (size_t)SIDE * SIDE * 4 * 2 / 3 * 2);
        ArtfTextureData back;
        memset(&back, 0, sizeof back);
        back.width = t->width;
        back.height = t->height;
        back.mipCount = t->mipCount;
        back.format = format;
        back.blocks = blocks;
        memcpy(back.blockOffsets, offsets, sizeof offsets);
        artfTextureDecode(&back);
        int worst = 0;
        for (int i = 0; i < SIDE * SIDE; ++i)
            for (int shift = 0; shift < 32; shift += 8) worst = artfMaxi(worst, abs(CHANNEL(back.pixels[i], shift) - CHANNEL(t->pixels[i], shift)));
        printf("blocks %d: the worst a channel strayed: %d\n", kind, worst);
        CHECK(worst < 40);
        artfFree(back.pixels);
        artfFree(blocks);
        artfSceneFree(s);
    }

    /* a file: read, and what is wrong in it refused */
    static uint8_t ktx[104 + 4096];
    static uint8_t blocks[4096];
    uint32_t state = 7;
    for (int i = 0; i < 4096; ++i) {
        state = state * 1664525u + 1013904223u;
        blocks[i] = (uint8_t)(state >> 24);
    }
    artfSetLog(countLog, &logged);
    logged = 0;
    ArtfScene* s = artfSceneNew();
    const size_t size = makeKtx2(ktx, 137, 64, 64, blocks, 4096);
    CHECK(artfSceneAddTextureKtx2(s, ktx, size) != 0);
    CHECK(artfSceneAddTextureKtx2(s, ktx, 100) == 0);
    CHECK(artfSceneAddTextureKtx2(s, "not this", 8) == 0);
    for (size_t cut = 0; cut < 110; ++cut) artfSceneAddTextureKtx2(s, ktx, cut);
    for (size_t i = 12; i < 104; ++i) {
        static uint8_t bad[104 + 4096];
        memcpy(bad, ktx, size);
        bad[i] ^= 0xFF;
        artfSceneAddTextureKtx2(s, bad, size);
    }
    CHECK(logged > 2);
    artfSetLog(NULL, NULL);
    artfSceneFree(s);

    /* The GPU knows these formats better than this code: random blocks, each format it takes, drawn by both.
     * Random blocks reach every mode, so what is wrong in a decoder shows as noise that does not match. */
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (!gpu) return;
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    static const struct { uint32_t format, bytes; } formats[] = {{131, 8}, {137, 16}, {147, 8}, {151, 16}};
    for (size_t f = 0; f < ARTF_COUNT(formats); ++f) {
        s = plainScene();
        const size_t bytes = (size_t)(64 / 4) * (64 / 4) * formats[f].bytes;
        const size_t fileSize = makeKtx2(ktx, formats[f].format, 64, 64, blocks, bytes);
        ArtfMaterialDesc d = {artfVec3(1, 1, 1), artfSceneAddTextureKtx2(s, ktx, fileSize), 0, 1, artfVec3(0, 0, 0)};
        addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &d), 0, 0, 0, 2);
        drawInto(gpu, s, a);
        drawInto(cpu, s, b);
        const double far = differentShare(a, b, 12);
        printf("format %u: pixels that differ by more than 12 of 255: %.2f%%\n", formats[f].format, far * 100);
        CHECK(far < .01);
        artfSceneFree(s);
    }
    /* plain textures compressed as they go up look like themselves */
    artfRendererFree(gpu);
    o = options(ARTF_BACKEND_VULKAN, 0, false);
    o.compressTextures = true;
    gpu = artfRendererNew(&o);
    s = richScene();
    drawInto(gpu, s, a);
    drawInto(cpu, s, b);
    CHECK(differentShare(a, b, 24) < .02);
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(gpu);
    artfRendererFree(cpu);
}

/* ---- noise ---- */

/* How far a picture is from another, on average, in steps of 255. */
static double errorOf(const uint32_t* a, const uint32_t* b) {
    double sum = 0;
    for (size_t i = 0; i < (size_t)W * H; ++i)
        for (int shift = 0; shift < 24; shift += 8) sum += abs(CHANNEL(a[i], shift) - CHANNEL(b[i], shift));
    return sum / ((double)W * H * 3);
}

static void testDenoise(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    uint32_t* c = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = shadowScene();
    artfSceneSetSunSize(s, .15f); /* (a wide sun: shadows that are all noise) */
    /* the truth, from many samples, and four samples with and without the filter */
    const ArtfTraceDesc truth = {ARTF_TRACE_PATHS, 8, 512, 0, false};
    artfSceneSetTrace(s, &truth);
    for (int i = 0; i < 64; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, c));
    const ArtfTraceDesc noisy = {ARTF_TRACE_PATHS, 1, 4, 0, false};
    artfSceneSetTrace(s, &noisy);
    artfRendererFree(r); /* (what was gathered would go on being shown) */
    r = artfRendererNew(&o);
    for (int i = 0; i < 4; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, a));
    const ArtfTraceDesc clean = {ARTF_TRACE_PATHS, 1, 0, 0, true};
    artfSceneSetTrace(s, &clean);
    for (int i = 0; i < 4; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, b));
    const double before = errorOf(a, c), after = errorOf(b, c);
    printf("from 512 samples: four samples are %.2f away, four filtered %.2f\n", before, after);
    CHECK(after < before * .6);

    /* the eye moves, and the picture does not start over: a frame later it is still closer than noise is */
    for (int i = 0; i < 12; ++i) CHECK(artfRendererDraw(r, s));
    setCamera(s, 2.05f, 5, 9, 0, 0, 0);
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, b));
    artfSceneSetTrace(s, &truth);
    for (int i = 0; i < 64; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, c));
    const double moved = errorOf(b, c);
    printf("a frame after moving: %.2f away\n", moved);
    CHECK(moved < before * .6);
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfFree(c);
    artfRendererFree(r);
}

/* A light that changes over what stays: what was gathered must not go on being shown. */
static void testDenoiseChange(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* c = artfMalloc((size_t)W * H * 4);
    for (int kind = 0; kind < 2; ++kind) {
        ArtfScene* s = shadowScene();
        artfSceneSetSunSize(s, .05f);
        const ArtfVec3 sun = kind ? artfVec3(.7f, -1, .3f) : artfVec3(-.5f, -1, -.3f);
        const ArtfVec3 color = kind ? artfVec3(.9f, .9f, .9f) : artfVec3(0, 0, 0);
        artfSceneSetSun(s, sun, color);
        const ArtfTraceDesc truth = {ARTF_TRACE_PATHS, 8, 512, 0, false};
        artfSceneSetTrace(s, &truth);
        for (int i = 0; i < 64; ++i) CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, c));
        artfRendererFree(r);
        r = artfRendererNew(&o);
        artfSceneSetSun(s, artfVec3(-.5f, -1, -.3f), artfVec3(.9f, .9f, .9f));
        const ArtfTraceDesc clean = {ARTF_TRACE_PATHS, 1, 0, 0, true};
        artfSceneSetTrace(s, &clean);
        for (int i = 0; i < 120; ++i) CHECK(artfRendererDraw(r, s));
        artfSceneSetSun(s, sun, color);
        for (int i = 0; i < 8; ++i) CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, a));
        const double away = errorOf(a, c);
        printf("eight frames after the sun %s: %.2f away\n", kind ? "turned" : "went out", away);
        CHECK(away < 4);
        artfSceneFree(s);
    }
    artfFree(a);
    artfFree(c);
    artfRendererFree(r);
}

/* What moves keeps what it gathered: it goes with its object. */
static void testDenoiseMotion(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* c = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = shadowScene();
    artfSceneSetSunSize(s, .15f);
    ArtfMaterialDesc m = {artfVec3(.8f, .3f, .3f), 0, 0, 1, artfVec3(0, 0, 0)};
    ArtfTransform at = artfTransformAt(-2, 2, 2, 1);
    const ArtfInstance moving = artfSceneAddInstance(s, artfSceneAddCube(s, 1), artfSceneAddMaterial(s, &m), &at);
    const ArtfTraceDesc clean = {ARTF_TRACE_PATHS, 1, 0, 0, true};
    artfSceneSetTrace(s, &clean);
    for (int i = 0; i < 40; ++i) {
        at = artfTransformAt(-2.f + (float)i * .05f, 2, 2, 1);
        artfInstanceSetTransform(s, moving, &at);
        CHECK(artfRendererDraw(r, s));
    }
    CHECK(artfRendererReadPixels(r, a));
    artfRendererFree(r);
    r = artfRendererNew(&o);
    const ArtfTraceDesc truth = {ARTF_TRACE_PATHS, 8, 512, 0, false};
    artfSceneSetTrace(s, &truth);
    for (int i = 0; i < 64; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, c));
    const double away = errorOf(a, c);
    printf("after forty frames of motion: %.2f away\n", away);
    CHECK(away < 3);
    artfSceneFree(s);
    artfFree(a);
    artfFree(c);
    artfRendererFree(r);
}

/* Occlusion tried both ways: the frames do not change, and it goes back and forth only in its rounds. */
static void testOcclusionAuto(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = shadowScene();
    drawInto(r, s, a);
    artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION_AUTO, true);
    int changes = 0;
    bool was = artfSceneFeature(s, ARTF_FEATURE_OCCLUSION);
    for (int i = 0; i < 260; ++i) {
        CHECK(artfRendererDraw(r, s));
        changes += artfSceneFeature(s, ARTF_FEATURE_OCCLUSION) != was;
        was = artfSceneFeature(s, ARTF_FEATURE_OCCLUSION);
    }
    CHECK(changes >= 1 && changes <= 4);
    drawInto(r, s, b);
    CHECK(!memcmp(a, b, (size_t)W * H * 4));
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

/* A map that did not change stays as it is; one that did comes out as a new renderer draws it. */
static void testShadowCache(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = shadowScene();
    ArtfMaterialDesc m = {artfVec3(.8f, .8f, .8f), 0, 0, 1, artfVec3(0, 0, 0)};
    ArtfTransform at = artfTransformAt(2, 0, 2, 1);
    const ArtfInstance moving = artfSceneAddInstance(s, artfSceneAddCube(s, 1), artfSceneAddMaterial(s, &m), &at);
    for (int i = 0; i < 40; ++i) addAt(s, artfSceneAddCube(s, .3f), 0, (float)(i % 8) * 12.f - 40.f, 0, (float)(i / 8) * 12.f - 30.f, 1);
    const ArtfShadowDesc maps = {256, 0};
    artfSceneSetShadows(s, &maps);
    ArtfFrameStats stats;
    CHECK(artfRendererDraw(r, s));
    artfRendererStats(r, &stats);
    CHECK(stats.shadowMapsDrawn >= 1);
    drawInto(r, s, a);
    drawInto(r, s, b);
    artfRendererStats(r, &stats);
    CHECK(stats.shadowMapsDrawn == 0);
    CHECK(!memcmp(a, b, (size_t)W * H * 4));
    at = artfTransformAt(-1, 0, 3, 1);
    artfInstanceSetTransform(s, moving, &at);
    CHECK(artfRendererDraw(r, s));
    artfRendererStats(r, &stats);
    CHECK(stats.shadowMapsDrawn >= 1);
    drawInto(r, s, a);
    artfRendererFree(r);
    r = artfRendererNew(&o);
    drawInto(r, s, b);
    CHECK(!memcmp(a, b, (size_t)W * H * 4));
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

/* Between two levels both are drawn, each in its pixels: nothing is left open, and the picture is between the two. */
static void testLodFade(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true); /* (whatever ARTFUL_FEATURES says) */
    const ArtfMesh mesh = artfSceneAddSphere(s, 1, 64, 96);
    const ArtfMeshData* m = &s->meshes.items[mesh - 1];
    CHECK(m->lodCount >= 1);
    addAt(s, mesh, whiteMaterial(s), 0, 0, 0, 1);
    /* where the first level starts: a little past it, it is in its fade */
    ArtfFrameData frame;
    artfSceneFrame(s, W, H, &frame);
    const float at = m->lodErrors[0] * frame.lodScale * 1.07f + m->radius;
    setCamera(s, 0, 0, at, 0, 0, 0);
    drawInto(r, s, a);
    artfSceneSetFeature(s, ARTF_FEATURE_LOD_FADE, true);
    artfSceneFrame(s, W, H, &frame);
    uint32_t other;
    const uint32_t fade = artfMeshFade(s, mesh - 1, frame.lodScale, 1, at - m->radius, frame.look[3], &other);
    CHECK(fade > 60 && fade < 200 && other == mesh - 1);
    drawInto(r, s, b);
    /* the middle of the disc is covered, as it was */
    const size_t mid = (size_t)(H / 2) * W + W / 2;
    int open = 0;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x) open += b[mid + (size_t)(y * W) + (size_t)x] == 0xFF000000u;
    CHECK(open == 0);
    CHECK(differentShare(a, b, 40) < .01);
    artfSceneSetFeature(s, ARTF_FEATURE_LOD_FADE, false);
    drawInto(r, s, b);
    CHECK(!memcmp(a, b, (size_t)W * H * 4));
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

/* Meshlets dropped by the GPU change nothing that shows. */
static void testGpuMeshlets(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = shadowScene();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, false);
    const ArtfMesh big = artfSceneAddSphere(s, 2, 256, 384);
    for (int i = 0; i < 12; ++i) addAt(s, big, 0, (float)(i % 4) * 4.f - 6.f, 1.5f, (float)(i / 4) * -5.f + 3.f, 1);
    setCamera(s, 1, 2, 7, 3, 1, 0);
    drawInto(r, s, a);
    artfSceneSetFeature(s, ARTF_FEATURE_CONES, false);
    drawInto(r, s, b);
    CHECK(!memcmp(a, b, (size_t)W * H * 4));
    /* and with occlusion, whichever pass each thing comes in, and what a wall hides now and shows next */
    artfSceneSetFeature(s, ARTF_FEATURE_CONES, true);
    artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, true);
    ArtfTransform at = artfTransformAt(0, 1, 5, 1);
    const ArtfInstance wall = artfSceneAddInstance(s, artfSceneAddCube(s, 3), 0, &at);
    for (int i = 0; i < 6; ++i) {
        setCamera(s, 1 + (float)i, 2, 8, 3, 1, 0);
        at = artfTransformAt((float)i * 2.f - 4.f, 1, 5, 1);
        artfInstanceSetTransform(s, wall, &at);
        drawInto(r, s, a);
        artfSceneSetFeature(s, ARTF_FEATURE_CONES, false);
        drawInto(r, s, b);
        artfSceneSetFeature(s, ARTF_FEATURE_CONES, true);
        CHECK(!memcmp(a, b, (size_t)W * H * 4));
    }
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

/* Two layers that cut through each other: each is in front on its own side. */
static void testLayersSorted(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    const ArtfVertex v[8] = {{-2, -2, -1, 0, 0, 1, 0, 0, 0}, {2, -2, 1, 0, 0, 1, 0, 0, 0}, {2, 2, 1, 0, 0, 1, 0, 0, 0}, {-2, 2, -1, 0, 0, 1, 0, 0, 0},
                             {-2, -2, 1, 0, 0, 1, 0, 0, 0}, {2, -2, -1, 0, 0, 1, 0, 0, 0}, {2, 2, -1, 0, 0, 1, 0, 0, 0}, {-2, 2, 1, 0, 0, 1, 0, 0, 0}};
    const uint32_t first[6] = {0, 1, 2, 0, 2, 3}, second[6] = {4, 5, 6, 4, 6, 7};
    ArtfMaterialDesc red = artfMaterialDefaults(), blue = artfMaterialDefaults();
    red.baseColor = artfVec3(1, 0, 0);
    blue.baseColor = artfVec3(0, 0, 1);
    red.alphaMode = blue.alphaMode = ARTF_ALPHA_BLEND;
    red.transparency = blue.transparency = .25f;
    red.specular = blue.specular = 0;
    red.doubleSided = blue.doubleSided = true;
    const ArtfTransform at = artfTransformAt(0, 0, 0, 1);
    artfSceneAddInstance(s, artfSceneAddMesh(s, v, 8, first, 6), artfSceneAddMaterial(s, &red), &at);
    artfSceneAddInstance(s, artfSceneAddMesh(s, v, 8, second, 6), artfSceneAddMaterial(s, &blue), &at);
    artfSceneSetFeature(s, ARTF_FEATURE_LAYERS_SORTED, true);
    drawInto(r, s, a);
    /* on the right the first is nearer, on the left the second: more of what is in front shows */
    const uint32_t left = a[(size_t)(H / 2) * W + W / 2 - 30], right = a[(size_t)(H / 2) * W + W / 2 + 30];
    CHECK(CHANNEL(right, 16) > CHANNEL(right, 0) + 40);
    CHECK(CHANNEL(left, 0) > CHANNEL(left, 16) + 40);

    /* the same with four samples a pixel, and it agrees with what one sample gave, but for edges */
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfRendererOptions smooth = o;
    smooth.antialias = true;
    ArtfRenderer* many = artfRendererNew(&smooth);
    drawInto(many, s, b);
    const double apart = differentShare(a, b, 24);
    printf("sorted layers, with samples and without, pixels that differ by more than 24 of 255: %.2f%%\n", apart * 100);
    CHECK(apart < .02);
    artfRendererFree(many);
    artfFree(b);
    artfSceneFree(s);
    artfFree(a);
    artfRendererFree(r);
}

/* Two lamps apart, and something that moves by one of them: the other's maps stay, and what comes out is what a new
 * renderer draws. */
static void testShadowRegions(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    artfSceneSetAmbient(s, artfVec3(.1f, .1f, .1f), artfVec3(.1f, .1f, .1f));
    setCamera(s, 0, 6, 14, 0, 0, 0);
    ArtfMaterial white = whiteMaterial(s);
    ArtfTransform at = artfTransformAt(0, -1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 40, 1), white, &at);
    const ArtfMesh cube = artfSceneAddCube(s, 1);
    addAt(s, cube, white, 8, 0, 0, 1);
    at = artfTransformAt(-8, 0, 0, 1);
    const ArtfInstance moving = artfSceneAddInstance(s, cube, white, &at);
    const ArtfShadowDesc maps = {256, 0};
    artfSceneSetShadows(s, &maps);
    artfLightSetShadow(s, artfSceneAddLight(s, artfVec3(8, 3, 0), artfVec3(9, 9, 9), 6), true);
    artfLightSetShadow(s, artfSceneAddLight(s, artfVec3(-8, 3, 0), artfVec3(9, 9, 9), 6), true);
    ArtfFrameStats stats;
    CHECK(artfRendererDraw(r, s));
    artfRendererStats(r, &stats);
    CHECK(stats.shadowMapsDrawn == 12);
    CHECK(artfRendererDraw(r, s));
    at = artfTransformAt(-7, 0, 1, 1);
    artfInstanceSetTransform(s, moving, &at);
    CHECK(artfRendererDraw(r, s));
    artfRendererStats(r, &stats);
    CHECK(stats.shadowMapsDrawn >= 1 && stats.shadowMapsDrawn <= 6);
    drawInto(r, s, a);
    artfRendererFree(r);
    r = artfRendererNew(&o);
    drawInto(r, s, b);
    CHECK(!memcmp(a, b, (size_t)W * H * 4));
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

/* A layer behind what is solid, with four samples a pixel: at the edge it stays behind, on both backends. */
static void testLayersBehindSolid(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    o.antialias = true;
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (!gpu) return;
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    ArtfMaterialDesc red = artfMaterialDefaults(), solid = artfMaterialDefaults();
    red.baseColor = artfVec3(1, .05f, .05f);
    red.alphaMode = ARTF_ALPHA_BLEND;
    red.transparency = .2f;
    red.specular = solid.specular = 0;
    solid.baseColor = artfVec3(.1f, .2f, 1);
    addAt(s, artfSceneAddCube(s, 6), artfSceneAddMaterial(s, &red), 0, 0, -6, 1);
    addAt(s, artfSceneAddSphere(s, 1.5f, 32, 48), artfSceneAddMaterial(s, &solid), 0, 0, 0, 1);
    /* (the backends do not agree on every edge pixel in any case: this may add nothing to that) */
    drawInto(gpu, s, a);
    drawInto(cpu, s, b);
    const double whole = differentShare(a, b, 24);
    artfSceneSetFeature(s, ARTF_FEATURE_LAYERS_SORTED, true);
    drawInto(gpu, s, a);
    drawInto(cpu, s, b);
    const double apart = differentShare(a, b, 24);
    printf("a layer behind an edge, pixels that differ by more than 24 of 255: %.3f%%, and %.3f%% with layers whole\n", apart * 100, whole * 100);
    CHECK(apart <= whole * 1.1 + .0002);
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(cpu);
    artfRendererFree(gpu);
}

/* What bones bend keeps what it gathered, as what moves whole does. */
static void testDenoiseBones(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* c = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = shadowScene();
    artfSceneSetSunSize(s, .15f);
    ArtfMaterialDesc m = {artfVec3(.8f, .3f, .3f), 0, 0, 1, artfVec3(0, 0, 0)};
    m.doubleSided = true;
    ArtfTransform at = artfTransformTrs(artfVec3(0, -1, 3), artfQuatAxisAngle(artfVec3(0, 1, 0), 0), artfVec3(9, 4, 9));
    const ArtfInstance strip = artfSceneAddInstance(s, addStrip(s), artfSceneAddMaterial(s, &m), &at);
    const ArtfTraceDesc clean = {ARTF_TRACE_PATHS, 1, 0, 0, true};
    artfSceneSetTrace(s, &clean);
    for (int i = 0; i < 20; ++i) {
        /* (carried along by its bones, straight off its surface: nothing else would find it again) */
        ArtfTransform joints[2] = {artfTransformIdentity(), artfTransformIdentity()};
        joints[0].m[11] = joints[1].m[11] = (float)(i - 19) * .05f;
        artfInstanceSetJoints(s, strip, joints, 2);
        CHECK(artfRendererDraw(r, s));
    }
    CHECK(artfRendererReadPixels(r, a));
    /* where a ray lands on it, the guide knows how far back it was: .05 of the object, stretched nine times */
    if (backend == ARTF_BACKEND_SOFTWARE) {
        ArtfFrameData frame;
        ArtfTraceFrame traced;
        artfSceneFrame(s, W, H, &frame);
        artfSceneTraceFrame(s, &frame, W, H, 0, &traced);
        int found = 0;
        for (uint32_t y = 0; y < H; y += 8)
            for (uint32_t x = 0; x < W; x += 8) {
                float light[4] = {0, 0, 0, 0};
                ArtfTraceGuide guide;
                memset(&guide, 0, sizeof guide);
                artfTraceSample(s, &frame, &traced, x, y, 0, light, &guide);
                found += fabsf(guide.shift[2] + .45f) < .05f;
            }
        printf("pixels whose guide knows: %d\n", found);
        CHECK(found > 10);
    }
    artfRendererFree(r);
    r = artfRendererNew(&o);
    const ArtfTraceDesc truth = {ARTF_TRACE_PATHS, 8, 512, 0, false};
    artfSceneSetTrace(s, &truth);
    for (int i = 0; i < 64; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, c));
    const double away = errorOf(a, c);
    printf("after twenty frames of being carried by bones: %.2f away\n", away);
    CHECK(away < 3);
    artfSceneFree(s);
    artfFree(a);
    artfFree(c);
    artfRendererFree(r);
}

static void testDenoiseBytes(void) {
    ArtfScene* s = shadowScene();
    const ArtfTraceDesc clean = {ARTF_TRACE_RAYS, 1, 0, 0, true};
    artfSceneSetTrace(s, &clean);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    uint64_t reference = 0;
    for (int threads = 1; threads <= 4; threads += 3) {
        ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, threads, false);
        ArtfRenderer* r = artfRendererNew(&o);
        drawSamples(r, s, 6, a);
        const uint64_t h = hashOf(a, (size_t)W * H);
        if (threads == 1) reference = h;
        CHECK(h == reference);
        artfRendererFree(r);
    }
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        drawSamples(gpu, s, 6, b);
        const double far = differentShare(a, b, 24);
        printf("the filter on both backends, pixels that differ by more than 24 of 255: %.2f%%\n", far * 100);
        CHECK(far < .02);
        artfRendererFree(gpu);
    }
    artfFree(a);
    artfFree(b);
    artfSceneFree(s);
}

/* ---- what comes and goes ---- */

static void testRemoving(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    const size_t mid = (size_t)(H / 2) * W + W / 2;
    artfSetLog(countLog, &logged);
    ArtfScene* s = plainScene();
    ArtfMaterial white = whiteMaterial(s);
    /* in use, it stays; once nothing uses it, it goes; and nothing else is touched */
    ArtfMesh keep = addSquare(s, 0xFF0000FFu, 0xFF0000FFu, 0xFF0000FFu, 0xFF0000FFu);
    addAt(s, keep, white, 0, 0, -1, 2);
    ArtfMesh m = addSquare(s, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u);
    ArtfInstance i = addAt(s, m, white, 0, 0, 0, 1);
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFFFF0000u);
    logged = 0;
    CHECK(!artfSceneRemoveMesh(s, m));
    CHECK(logged == 1);
    artfSceneRemoveInstance(s, i);
    CHECK(artfSceneRemoveMesh(s, m));
    CHECK(!artfSceneRemoveMesh(s, m));
    CHECK(artfSceneAddInstance(s, m, white, NULL) == 0);
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFF0000FFu);

    /* many come and go, with what stays right; this is what a GPU's memory must take */
    ArtfSceneMemory before, after;
    artfSceneMemory(s, &before);
    for (int round = 0; round < 40; ++round) {
        ArtfMesh sphere = artfSceneAddSphere(s, 1, 8 + round % 5, 12);
        ArtfMesh green = addSquare(s, 0xFF00FF00u, 0xFF00FF00u, 0xFF00FF00u, 0xFF00FF00u);
        ArtfInstance one = addAt(s, sphere, white, 0, 3, 0, 1), two = addAt(s, green, white, 0, 0, 0, 1);
        CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, a));
        CHECK(a[mid] == 0xFF00FF00u);
        artfSceneRemoveInstance(s, one);
        artfSceneRemoveInstance(s, two);
        CHECK(artfSceneRemoveMesh(s, sphere));
        CHECK(artfSceneRemoveMesh(s, green));
    }
    artfSceneMemory(s, &after);
    CHECK(after.meshes == before.meshes);
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFF0000FFu);
    /* traced, as well */
    const ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 2, 0};
    artfSceneSetTrace(s, &rays);
    drawSamples(r, s, 2, a);
    CHECK(a[mid] == 0xFF0000FFu);
    artfSceneFree(s);

    /* a texture's place is used again, and what uses a texture keeps it */
    s = plainScene();
    ArtfTexture t = oneColor(s, 0xFFFF0000u);
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), t, 0, 1, artfVec3(0, 0, 0)};
    ArtfMaterial tm = artfSceneAddMaterial(s, &d);
    addAt(s, addSquare(s, 0, 0, 0, 0), tm, 0, 0, 0, 1);
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFFFF0000u);
    CHECK(!artfSceneRemoveTexture(s, t));
    d.texture = 0;
    artfMaterialSet(s, tm, &d);
    CHECK(artfSceneRemoveTexture(s, t));
    ArtfTexture again = oneColor(s, 0xFF00FF00u);
    CHECK(again == t);
    d.texture = again;
    artfMaterialSet(s, tm, &d);
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFF00FF00u);
    artfSceneFree(s);

    /* a bent instance's copy goes with it */
    s = plainScene();
    white = whiteMaterial(s);
    ArtfMesh strip = addStrip(s);
    ArtfInstance one = addAt(s, strip, white, 0, 0, 0, 1);
    CHECK(artfRendererDraw(r, s));
    CHECK(!artfSceneRemoveMesh(s, strip));
    artfSceneRemoveInstance(s, one);
    CHECK(s->deforming.len == 0);
    CHECK(artfRendererDraw(r, s));
    CHECK(artfSceneRemoveMesh(s, strip));
    artfSceneFree(s);
    artfSetLog(NULL, NULL);
    artfFree(a);
    artfRendererFree(r);
}

static void testPrepared(void) {
    /* what is prepared is what is added, with its levels */
    ArtfScene* a = artfSceneNew();
    ArtfScene* b = artfSceneNew();
    artfSceneSetFeature(a, ARTF_FEATURE_LOD, true);
    enum { N = 24 };
    static ArtfVertex v[(N + 1) * (N + 1)];
    static uint32_t idx[N * N * 6];
    uint32_t n = 0;
    for (int y = 0; y <= N; ++y)
        for (int x = 0; x <= N; ++x) {
            const ArtfVertex q = {(float)x, (float)y, 0, 0, 0, 1, 0, 0, 0};
            v[y * (N + 1) + x] = q;
        }
    for (uint32_t y = 0; y < N; ++y)
        for (uint32_t x = 0; x < N; ++x) {
            const uint32_t p = y * (N + 1) + x, q = p + 1, d = p + N + 1, e = d + 1;
            idx[n++] = p; idx[n++] = q; idx[n++] = e;
            idx[n++] = p; idx[n++] = e; idx[n++] = d;
        }
    const ArtfMesh direct = artfSceneAddMesh(a, v, (N + 1) * (N + 1), idx, n);
    const ArtfMesh made = artfSceneAddPreparedMesh(b, artfPrepareMesh(v, (N + 1) * (N + 1), idx, n, true));
    CHECK(direct && made);
    CHECK(a->meshes.items[direct - 1].lodCount == b->meshes.items[made - 1].lodCount);
    CHECK(b->meshes.items[made - 1].lodCount > 0);
    static uint32_t px[32 * 32];
    for (int i = 0; i < 32 * 32; ++i) px[i] = 0xFF204060u;
    const ArtfTexture t = artfSceneAddPreparedTexture(b, artfPrepareTexture(px, 32, 32, 0));
    CHECK(t && b->textures.items[t].mipCount == 6);
    artfSceneFree(a);
    artfSceneFree(b);
}

static int reports;
static void onResidency(void* user, ArtfTexture t, int mip) {
    ArtfScene* s = user;
    ++reports;
    artfTextureSetResidentMip(s, t, mip);
}

static void testResidency(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    enum { SIDE = 256 };
    static uint32_t px[SIDE * SIDE];
    for (int i = 0; i < SIDE * SIDE; ++i) px[i] = 0xFF4080C0u;
    ArtfScene* s = plainScene();
    const ArtfTexture t = artfSceneAddTexture(s, px, SIDE, SIDE, 0);
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), t, 0, 1, artfVec3(0, 0, 0)};
    addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
    ArtfSceneMemory m;
    artfSceneMemory(s, &m);
    CHECK(m.textures == m.residentTextures && m.textures > (size_t)SIDE * SIDE * 4);

    /* none: nothing happens by itself, and a call does */
    CHECK(artfRendererDraw(r, s));
    CHECK(s->textures.items[t].residentMip == 0);
    artfTextureSetResidentMip(s, t, 3);
    artfSceneMemory(s, &m);
    CHECK(m.residentTextures < m.textures / 32);
    drawInto(r, s, a);
    const size_t mid = (size_t)(H / 2) * W + W / 2;
    CHECK(near_(CHANNEL(a[mid], 8), 0x80, 2) && near_(CHANNEL(a[mid], 16), 0x40, 2));
    artfTextureSetResidentMip(s, t, 0);

    /* assisted: Artful says, and the program does */
    ArtfResidencyDesc desc = {ARTF_RESIDENCY_ASSISTED, 4096, 1, onResidency, s};
    reports = 0;
    artfSceneSetResidency(s, &desc);
    drawInto(r, s, a);
    CHECK(reports > 0);
    artfSceneMemory(s, &m);
    CHECK(m.residentTextures <= 4096);
    artfTextureSetResidentMip(s, t, 0);

    /* automatic: within its budget, and what is far needs less */
    desc.mode = ARTF_RESIDENCY_AUTOMATIC;
    desc.report = NULL;
    artfSceneSetResidency(s, &desc);
    drawInto(r, s, a);
    artfSceneMemory(s, &m);
    CHECK(m.residentTextures <= 4096);
    CHECK(near_(CHANNEL(a[mid], 8), 0x80, 2));
    desc.budget = 0;
    artfSceneSetResidency(s, &desc);
    setCamera(s, 0, 0, 200, 0, 0, 0);
    drawInto(r, s, a);
    CHECK(s->textures.items[t].residentMip >= 3);
    setCamera(s, 0, 0, 2, 0, 0, 0);
    drawInto(r, s, a);
    CHECK(s->textures.items[t].residentMip == 0);
    artfSceneFree(s);
    artfFree(a);
    artfRendererFree(r);
}

/* Blocks of noise, turned into pixels, hashed. The hashes are of what was checked, block by block, against a decoder of
 * someone else's: 65,536 blocks of BC7 alike, and 46,000 of ASTC within one of 255 (it rounds a step sooner). */
static void testBlockDecoders(void) {
    uint32_t state = 12345;
    uint64_t bc7 = 1469598103934665603ull, astc = bc7;
    uint32_t out[144];
    int drawn = 0;
    for (int i = 0; i < 200000; ++i) {
        uint8_t b[16];
        for (int k = 0; k < 16; ++k) {
            state = state * 1664525u + 1013904223u;
            b[k] = (uint8_t)(state >> 24);
        }
        artfDecodeBc7(b, out);
        for (int k = 0; k < 16; ++k) bc7 = (bc7 ^ out[k]) * 1099511628211ull;
        artfDecodeAstc(b, 6, 6, out);
        if (out[0] != 0xFFFF00FFu) ++drawn;
        for (int k = 0; k < 36; ++k) astc = (astc ^ out[k]) * 1099511628211ull;
    }
    printf("blocks: bc7 %016llx, astc %016llx, %d of ASTC's are legal\n", (unsigned long long)bc7, (unsigned long long)astc, drawn);
    CHECK(bc7 == 0x173e39b598bebd9dull);
    CHECK(astc == 0x6bdc1b46124e7596ull);
}

/* A texture whose mipmaps come when they are needed: each a color of its own, to see which is drawn. */
static int mipReads;
static bool readMip(void* user, int mip, uint32_t* pixels) {
    (void)user;
    ++mipReads;
    const int side = artfMaxi(64 >> mip, 1);
    for (int i = 0; i < side * side; ++i) pixels[i] = 0xFF000000u | (uint32_t)(mip * 40) << 8 | 0x10;
    return true;
}

static void testStreamedTexture(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    mipReads = 0;
    const ArtfTexture t = artfSceneAddTextureStreamed(s, 64, 64, 0, 3, readMip, NULL);
    CHECK(t && mipReads == 4); /* (eight, four, two and one pixels) */
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), t, 0, 1, artfVec3(0, 0, 0)};
    addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
    ArtfSceneMemory m;
    artfSceneMemory(s, &m);
    const size_t few = m.residentTextures;
    CHECK(few < 64 * 64);
    drawInto(r, s, a);
    const size_t mid = (size_t)(H / 2) * W + W / 2;
    CHECK(near_(CHANNEL(a[mid], 8), 120, 2));

    /* more of it: the rest is read, what was there is not */
    artfTextureSetResidentMip(s, t, 0);
    CHECK(mipReads == 7);
    drawInto(r, s, a);
    CHECK(CHANNEL(a[mid], 8) < 120);
    artfSceneMemory(s, &m);
    CHECK(m.residentTextures > few);

    /* less: the memory goes */
    artfTextureSetResidentMip(s, t, 5);
    artfSceneMemory(s, &m);
    CHECK(m.residentTextures < few);
    drawInto(r, s, a);
    CHECK(near_(CHANNEL(a[mid], 8), 200, 2));
    artfSceneFree(s);
    artfFree(a);
    artfRendererFree(r);
}

/* What a material in C reads keeps its detail, however little the material itself would need. */
static void testResidencyGraph(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    enum { SIDE = 256 };
    static uint32_t px[SIDE * SIDE];
    for (int y = 0; y < SIDE; ++y)
        for (int x = 0; x < SIDE; ++x) px[y * SIDE + x] = (x / 4 + y / 4) & 1 ? 0xFFFFFFFFu : 0xFF000000u;
    ArtfScene* s = plainScene();
    const ArtfTexture t = artfSceneAddTexture(s, px, SIDE, SIDE, 0);
    ArtfGraph* g = artfGraphNew();
    artfGraphSetOutput(g, ARTF_OUT_COLOR, artfExprTexture(g, t, artfExprUv(g)));
    ArtfMaterialDesc d = artfMaterialDefaults();
    d.graph = artfSceneAddGraph(s, g);
    artfGraphFree(g);
    addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
    ArtfResidencyDesc desc = {ARTF_RESIDENCY_AUTOMATIC, 0, 1, NULL, NULL};
    artfSceneSetResidency(s, &desc);
    for (int i = 0; i < 3; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(s->textures.items[t].residentMip == 0);
    artfSceneFree(s);
    artfRendererFree(r);
}

/* A device lost in the middle of it all: false, and false after that; the renderer can be freed; and the scene
 * goes to a new one, which draws what the first did. */
static void testLostDevice(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = shadowScene();
    drawInto(r, s, a);
    artfVulkanSubmitsLeft = 2;
    CHECK(artfRendererDraw(r, s) && artfRendererDraw(r, s));
    CHECK(!artfRendererDraw(r, s));
    artfVulkanSubmitsLeft = -1;
    CHECK(!artfRendererDraw(r, s));
    CHECK(!artfRendererReadPixels(r, b));
    artfRendererFree(r);
    r = artfRendererNew(&o);
    CHECK(r != NULL);
    drawInto(r, s, b);
    CHECK(!memcmp(a, b, (size_t)W * H * 4));
    artfRendererFree(r);

    /* the same, while tracing */
    r = artfRendererNew(&o);
    const ArtfTraceDesc clean = {ARTF_TRACE_PATHS, 1, 0, 0, true};
    artfSceneSetTrace(s, &clean);
    CHECK(artfRendererDraw(r, s));
    artfVulkanSubmitsLeft = 0;
    CHECK(!artfRendererDraw(r, s));
    artfVulkanSubmitsLeft = -1;
    artfRendererFree(r);
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
}

#ifdef _WIN32
__declspec(dllimport) void* __stdcall CreateWindowExA(unsigned long, const char*, const char*, unsigned long, int, int, int, int, void*, void*, void*, void*);
__declspec(dllimport) int __stdcall DestroyWindow(void*);
/* A driver that refuses a swapchain, first and on a resize: the window is free again for whatever comes next. */
static void testRefusedSwapchain(void) {
    void* window = CreateWindowExA(0, "STATIC", "artful", 0xCF0000u, 0, 0, W, H, NULL, NULL, NULL, NULL);
    if (!window) return;
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    o.surface.kind = ARTF_SURFACE_WIN32;
    o.surface.a = window;
    ArtfScene* s = shadowScene();
    artfVulkanRefuseSwapchain = true;
    ArtfRenderer* r = artfRendererNew(&o);
    artfVulkanRefuseSwapchain = false;
    CHECK(r == NULL || !artfRendererDraw(r, s));
    artfRendererFree(r);
    r = artfRendererNew(&o);
    if (r) {
        CHECK(artfRendererDraw(r, s));
        artfVulkanRefuseSwapchain = true;
        artfRendererResize(r, W / 2, H / 2);
        CHECK(!artfRendererDraw(r, s));
        artfVulkanRefuseSwapchain = false;
        CHECK(!artfRendererDraw(r, s));
        artfRendererFree(r);
        r = artfRendererNew(&o);
        CHECK(r != NULL && artfRendererDraw(r, s));
        artfRendererFree(r);
    }
    artfSceneFree(s);
    DestroyWindow(window);
}
#else
static void testRefusedSwapchain(void) {}
#endif

/* A place that was left is taken again, and what is drawn from it is what is there now. */
static void testMeshPlaces(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    ArtfMaterial white = whiteMaterial(s);
    ArtfMesh mesh = artfSceneAddSphere(s, 1, 16, 24);
    ArtfTransform at = artfTransformAt(0, 0, 0, 1);
    ArtfInstance i = artfSceneAddInstance(s, mesh, white, &at);
    drawInto(r, s, a);
    const size_t places = s->meshes.len;
    for (int round = 0; round < 6; ++round) {
        artfSceneRemoveInstance(s, i);
        CHECK(artfSceneRemoveMesh(s, mesh));
        mesh = artfSceneAddCube(s, 1.2f);
        i = artfSceneAddInstance(s, mesh, white, &at);
        drawInto(r, s, b);
        CHECK(differentShare(a, b, 24) > .001);
        artfSceneRemoveInstance(s, i);
        CHECK(artfSceneRemoveMesh(s, mesh));
        mesh = artfSceneAddSphere(s, 1, 16, 24);
        i = artfSceneAddInstance(s, mesh, white, &at);
        drawInto(r, s, b);
        CHECK(differentShare(a, b, 24) < .001);
    }
    CHECK(s->meshes.len <= places + 1);
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

static void testGltfSparse(void) {
    static const char text[] = "{\"asset\":{\"version\":\"2.0\"},%s\"buffers\":[{\"byteLength\":44,\"uri\":\"data:application/octet-stream;base64,AAABAAIAAAAAAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAAAAAACAPwAAAAA=\"}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":6},{\"buffer\":0,\"byteOffset\":8,\"byteLength\":36}],\"accessors\":[{\"componentType\":5126,\"type\":\"VEC3\",\"count\":3,\"max\":[1,1,0],\"min\":[0,0,0],\"sparse\":{\"count\":3,\"indices\":{\"bufferView\":0,\"componentType\":5123},\"values\":{\"bufferView\":1}}}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    char file[1200];
    int size = snprintf(file, sizeof file, text, "");
    ArtfScene* s = artfSceneNew();
    CHECK(artfSceneLoadGltfMemory(s, file, (size_t)size, NULL, NULL) == 1);
    /* the three points came from the changes alone: a triangle with an area */
    CHECK(s->meshes.len >= 1 && s->meshes.items[0].vertexCount == 3 && s->meshes.items[0].radius > .4f);
    artfSceneFree(s);
    s = artfSceneNew();
    size = snprintf(file, sizeof file, text, "\"extensionsRequired\":[\"KHR_nothing_here\"],");
    CHECK(artfSceneLoadGltfMemory(s, file, (size_t)size, NULL, NULL) == 0);
    size = snprintf(file, sizeof file, text, "\"extensionsRequired\":[\"KHR_mesh_quantization\"],");
    CHECK(artfSceneLoadGltfMemory(s, file, (size_t)size, NULL, NULL) == 1);
    artfSceneFree(s);
}

/* KHR_lights_punctual, cameras, KHR_materials_emissive_strength and KHR_texture_transform. */
static void testGltfExtensions(void) {
    static const char file[] =
        "{\"asset\":{\"version\":\"2.0\"},\"extensionsRequired\":[\"KHR_texture_transform\"],\"extensions\":{\"KHR_ligh"
        "ts_punctual\":{\"lights\":[{\"type\":\"point\",\"intensity\":20,\"range\":10,\"color\":[1,0.5,0.25]},{\"type\""
        ":\"directional\",\"intensity\":3},{\"type\":\"spot\",\"intensity\":5,\"spot\":{\"innerConeAngle\":0.2,\"outerC"
        "oneAngle\":0.5}}]}},\"buffers\":[{\"byteLength\":84,\"uri\":\"data:application/octet-stream;base64,AAAAAAAAAAA"
        "AAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAAAAAAAAAAAAAA/AAAAAAAAAAAAAAA/\"}],\"bu"
        "fferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},{\"buffer\":0,\"byteOffset\":36,\"byteLength\":"
        "24},{\"buffer\":0,\"byteOffset\":60,\"byteLength\":24}],\"accessors\":[{\"bufferView\":0,\"componentType\":512"
        "6,\"type\":\"VEC3\",\"count\":3,\"max\":[1,1,0],\"min\":[0,0,0]},{\"bufferView\":1,\"componentType\":5126,\"ty"
        "pe\":\"VEC2\",\"count\":3},{\"bufferView\":2,\"componentType\":5126,\"type\":\"VEC2\",\"count\":3}],\"material"
        "s\":[{\"emissiveFactor\":[1,1,1],\"extensions\":{\"KHR_materials_emissive_strength\":{\"emissiveStrength\":4}}"
        ",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0,\"extensions\":{\"KHR_texture_transform\":{\"offs"
        "et\":[0.5,0],\"scale\":[2,2],\"texCoord\":1}}}}}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0"
        ",\"TEXCOORD_0\":1,\"TEXCOORD_1\":2},\"material\":0}]}],\"cameras\":[{\"type\":\"perspective\",\"perspective\":"
        "{\"yfov\":0.7,\"znear\":0.05}}],\"nodes\":[{\"mesh\":0},{\"translation\":[0,2,0],\"extensions\":{\"KHR_lights_"
        "punctual\":{\"light\":0}}},{\"extensions\":{\"KHR_lights_punctual\":{\"light\":1}}},{\"translation\":[1,1,1],\""
        "extensions\":{\"KHR_lights_punctual\":{\"light\":2}}},{\"translation\":[0,0,5],\"camera\":0}],\"scenes\":[{\"n"
        "odes\":[0,1,2,3,4]}],\"scene\":0}";
    ArtfScene* s = artfSceneNew();
    const ArtfModel m = artfSceneLoadModelMemory(s, file, sizeof file - 1, NULL, NULL);
    CHECK(m && artfModelInstanceCount(s, m) == 1);
    /* the second set of coordinates, moved by half and doubled: (0.5, 0) and (0, 0.5) become (1.5, 0) and (0.5, 1) */
    float most = 0, least = 9;
    const ArtfMeshData* mesh = &s->meshes.items[0];
    for (uint32_t i = 0; i < mesh->vertexCount; ++i) {
        most = artfMaxf(most, artfHalfToFloat(mesh->vertices[i].u));
        least = artfMinf(least, artfHalfToFloat(mesh->vertices[i].u));
    }
    CHECK(most == 1.5f && least == .5f);
    CHECK(s->materials.items[s->materials.len - 1].emissive[0] == 4.f);
    /* the point light, its range kept; the sun, down -z; the spot, a cone */
    CHECK(s->lightOn[0] && s->lights[0].position[1] == 2.f && s->lights[0].radius == 10.f);
    CHECK(s->lights[0].color[0] > s->lights[0].color[1] && s->lights[0].color[1] > s->lights[0].color[2]);
    CHECK(s->sunColor.x == 3.f && s->toSun.z > .99f);
    CHECK(s->lightOn[1] && s->lights[1].spotCut > .87f && s->lights[1].spotCut < .88f);
    ArtfCamera eye;
    CHECK(artfModelCameraCount(s, m) == 1 && artfModelCamera(s, m, 0, &eye));
    CHECK(eye.position.z == 5.f && eye.target.z == 4.f && eye.up.y == 1.f && eye.fovY == .7f);
    CHECK(!artfModelCamera(s, m, 1, &eye));
    /* lights and cameras go with the model */
    const ArtfTransform moved = artfTransformAt(3, 0, 0, 1);
    artfModelSetTransform(s, m, &moved);
    CHECK(s->lights[0].position[0] == 3.f && artfModelCamera(s, m, 0, &eye) && eye.position.x == 3.f);
    artfSceneFree(s);
}

/* ---- EXT_meshopt_compression: encoders as meshoptimizer's streams are laid out, to check the decoders against ---- */

typedef struct Bytes { uint8_t* p; size_t n, cap; } Bytes;
static void bytePut(Bytes* b, uint8_t v) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->p = realloc(b->p, b->cap);
    }
    b->p[b->n++] = v;
}
static void vbytePut(Bytes* b, uint32_t v) {
    do {
        const uint8_t group = (uint8_t)(v & 127);
        v >>= 7;
        bytePut(b, (uint8_t)(group | (v ? 128 : 0)));
    } while (v);
}

/* Vertices: each group of sixteen bytes in the fewest bits that hold it, as the encoder chooses. */
static void encodeVertices(Bytes* out, const uint8_t* v, size_t count, size_t stride) {
    bytePut(out, 0xA0);
    uint8_t last[256];
    memcpy(last, v, stride);
    size_t block = 8192 / stride & ~(size_t)15;
    if (block > 256) block = 256;
    for (size_t first = 0; first < count; first += block) {
        const size_t n = count - first < block ? count - first : block, aligned = (n + 15) & ~(size_t)15;
        for (size_t k = 0; k < stride; ++k) {
            uint8_t deltas[256] = {0};
            uint8_t p = last[k];
            for (size_t i = 0; i < n; ++i) {
                const uint8_t now = v[(first + i) * stride + k], d = (uint8_t)(now - p);
                deltas[i] = (uint8_t)((d << 1) ^ (uint8_t)((int8_t)d >> 7)); /* zig-zag */
                p = now;
            }
            last[k] = p;
            const size_t groups = aligned / 16, headerAt = out->n;
            for (size_t h = 0; h < (groups + 3) / 4; ++h) bytePut(out, 0);
            for (size_t g = 0; g < groups; ++g) {
                const uint8_t* d = deltas + g * 16;
                int best = 3;
                size_t bestSize = 16;
                for (int bits = 0; bits < 3; ++bits) {
                    const int width = bits == 0 ? 0 : bits == 1 ? 2 : 4, all = (1 << width) - 1;
                    size_t size = (size_t)(16 * width / 8);
                    bool fits = true;
                    for (int i = 0; i < 16; ++i) {
                        if (width == 0) fits &= d[i] == 0;
                        else size += d[i] >= all;
                    }
                    if (fits && size < bestSize) { best = bits; bestSize = size; }
                }
                out->p[headerAt + g / 4] |= (uint8_t)(best << (g % 4 * 2));
                if (best == 3) {
                    for (int i = 0; i < 16; ++i) bytePut(out, d[i]);
                } else if (best > 0) {
                    const int width = best == 1 ? 2 : 4, all = (1 << width) - 1, each = 8 / width;
                    for (int i = 0; i < 16; i += each) {
                        uint8_t byte = 0;
                        for (int j = 0; j < each; ++j) byte = (uint8_t)(byte << width | (d[i + j] >= all ? all : d[i + j]));
                        bytePut(out, byte);
                    }
                    for (int i = 0; i < 16; ++i)
                        if (d[i] >= all) bytePut(out, d[i]);
                }
            }
        }
    }
    /* the first vertex, at the end */
    const size_t tail = stride < 32 ? 32 : stride;
    for (size_t i = 0; i < tail - stride; ++i) bytePut(out, 0);
    for (size_t i = 0; i < stride; ++i) bytePut(out, v[i]);
}

/* Triangles: an edge met lately when there is one, a new vertex or one met lately or a free one; else three free. */
static void encodeTriangles(Bytes* out, const uint16_t* t, size_t count) {
    uint32_t edges[16][2], vertices[16], edge = 0, vertex = 0, next = 0, last = 0;
    memset(edges, 0xFF, sizeof edges);
    memset(vertices, 0xFF, sizeof vertices);
    Bytes codes = {0}, data = {0};
#define PUSH_V(x, adv) (vertices[vertex] = (x), vertex = (vertex + (adv)) & 15)
#define PUSH_E(x, y) (edges[edge][0] = (x), edges[edge][1] = (y), edge = (edge + 1) & 15)
    for (size_t i = 0; i < count; i += 3) {
        const uint32_t a = t[i], b = t[i + 1], c = t[i + 2];
        int fe = -1;
        for (int k = 0; k < 16 && fe < 0; ++k)
            if (edges[(edge - 1 - k) & 15][0] == a && edges[(edge - 1 - k) & 15][1] == b) fe = k;
        if (fe >= 0) {
            int fec = -1;
            if (c == next) fec = 0;
            for (int k = 1; k < 13 && fec < 0; ++k)
                if (vertices[(vertex - 1 - k) & 15] == c) fec = k;
            if (fec < 0) fec = c == last - 1 ? 13 : c == last + 1 ? 14 : 15;
            bytePut(&codes, (uint8_t)(fe << 4 | fec));
            if (fec == 0) ++next;
            if (fec == 15) {
                const int32_t d = (int32_t)(c - last);
                vbytePut(&data, (((uint32_t)d << 1) ^ (d < 0 ? 0xFFFFFFFFu : 0u)));
            }
            if (fec >= 13) last = c;
            PUSH_V(c, fec == 0 || fec >= 13);
            PUSH_E(c, b);
            PUSH_E(a, c);
        } else {
            /* three free: 0xff, and a byte of fifteens */
            bytePut(&codes, 0xFF);
            bytePut(&data, 0xFF);
            const uint32_t abc[3] = {a, b, c};
            for (int k = 0; k < 3; ++k) {
                const int32_t d = (int32_t)(abc[k] - last);
                vbytePut(&data, (((uint32_t)d << 1) ^ (d < 0 ? 0xFFFFFFFFu : 0u)));
                last = abc[k];
            }
            PUSH_V(a, 1);
            PUSH_V(b, 1);
            PUSH_V(c, 1);
            PUSH_E(b, a);
            PUSH_E(c, b);
            PUSH_E(a, c);
        }
    }
#undef PUSH_V
#undef PUSH_E
    bytePut(out, 0xE1);
    for (size_t i = 0; i < codes.n; ++i) bytePut(out, codes.p[i]);
    for (size_t i = 0; i < data.n; ++i) bytePut(out, data.p[i]);
    for (int i = 0; i < 16; ++i) bytePut(out, 0); /* (the table: not used here) */
    free(codes.p);
    free(data.p);
}

static void encodeIndexSequence(Bytes* out, const uint16_t* t, size_t count) {
    bytePut(out, 0xD1);
    uint32_t last[2] = {0, 0};
    for (size_t i = 0; i < count; ++i) {
        /* (alternating between the two lasts, to use both) */
        const uint32_t which = i & 1;
        const int32_t d = (int32_t)(t[i] - last[which]);
        last[which] = t[i];
        vbytePut(out, ((((uint32_t)d << 1) ^ (d < 0 ? 0xFFFFFFFFu : 0u))) << 1 | which);
    }
    for (int i = 0; i < 4; ++i) bytePut(out, 0);
}

static void base64Put(char* out, const uint8_t* p, size_t n) {
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out[o++] = digits[v >> 18 & 63];
        out[o++] = digits[v >> 12 & 63];
        out[o++] = i + 1 < n ? digits[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? digits[v & 63] : '=';
    }
    out[o] = 0;
}

/* A hill of 9 x 9 vertices, as a plain file and as one compressed with EXT_meshopt_compression (triangles in one
 * primitive, the same indices as a sequence in another): both draw the same. Then the filters, alone. And the
 * materials' extensions, and an orthographic camera. */
static void testGltfMeshopt(void) {
    enum { SIDE = 9, VERTS = SIDE * SIDE, TRIS = (SIDE - 1) * (SIDE - 1) * 2 };
    static float positions[VERTS * 3];
    static uint16_t indices[TRIS * 3];
    for (int z = 0; z < SIDE; ++z)
        for (int x = 0; x < SIDE; ++x) {
            float* p = positions + (z * SIDE + x) * 3;
            p[0] = (float)x * .25f - 1.f;
            p[1] = (float)((x - 4) * (x - 4) + (z - 4) * (z - 4)) * -.02f;
            p[2] = (float)z * .25f - 1.f;
        }
    int n = 0;
    for (int z = 0; z + 1 < SIDE; ++z)
        for (int x = 0; x + 1 < SIDE; ++x) {
            const uint16_t a = (uint16_t)(z * SIDE + x), b = (uint16_t)(a + SIDE);
            /* (the second starts on an edge of the first) */
            indices[n++] = a; indices[n++] = b; indices[n++] = (uint16_t)(b + 1);
            indices[n++] = a; indices[n++] = (uint16_t)(b + 1); indices[n++] = (uint16_t)(a + 1);
        }
    Bytes v = {0}, tri = {0}, seq = {0};
    encodeVertices(&v, (const uint8_t*)positions, VERTS, 12);
    encodeTriangles(&tri, indices, TRIS * 3);
    encodeIndexSequence(&seq, indices, TRIS * 3);
    /* the decoders, straight */
    static float gotPositions[VERTS * 3];
    static uint16_t gotTriangles[TRIS * 3], gotSequence[TRIS * 3];
    CHECK(artfMeshoptDecodeVertices((uint8_t*)gotPositions, VERTS, 12, v.p, v.n) == 0 && !memcmp(gotPositions, positions, sizeof positions));
    CHECK(artfMeshoptDecodeTriangles((uint8_t*)gotTriangles, TRIS * 3, 2, tri.p, tri.n) == 0 && !memcmp(gotTriangles, indices, sizeof indices));
    CHECK(artfMeshoptDecodeIndices((uint8_t*)gotSequence, TRIS * 3, 2, seq.p, seq.n) == 0 && !memcmp(gotSequence, indices, sizeof indices));
    CHECK(artfMeshoptDecodeVertices((uint8_t*)gotPositions, VERTS, 12, v.p, v.n - 1) != 0); /* (cut short: refused) */
    /* both files: plain, and with the compressed views (their fallback buffer holds nothing) */
    const size_t plainSize = sizeof positions + sizeof indices;
    uint8_t* plain = malloc(plainSize);
    memcpy(plain, positions, sizeof positions);
    memcpy(plain + sizeof positions, indices, sizeof indices);
    const size_t packedSize = v.n + tri.n + seq.n;
    uint8_t* packed = malloc(packedSize + 8);
    memcpy(packed, v.p, v.n);
    memcpy(packed + v.n, tri.p, tri.n);
    memcpy(packed + v.n + tri.n, seq.p, seq.n);
    char* b64Plain = malloc(plainSize * 2 + 8);
    char* b64Packed = malloc(packedSize * 2 + 8);
    base64Put(b64Plain, plain, plainSize);
    base64Put(b64Packed, packed, packedSize);
    char* json = malloc(plainSize * 4 + packedSize * 4 + 8192);
    const char* mesh = "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1},{\"attributes\":{\"POSITION\":0},\"indices\":2}]}],"
                       "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0";
    const char* accessors = "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"type\":\"VEC3\",\"count\":81,\"max\":[1,0,1],\"min\":[-1,-0.64,-1]},"
                            "{\"bufferView\":1,\"componentType\":5123,\"type\":\"SCALAR\",\"count\":384},{\"bufferView\":2,\"componentType\":5123,\"type\":\"SCALAR\",\"count\":384}]";
    ArtfScene* scenes[2];
    for (int k = 0; k < 2; ++k) {
        if (!k)
            sprintf(json, "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":%u,\"uri\":\"data:application/octet-stream;base64,%s\"}],"
                          "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":972,\"byteStride\":12},{\"buffer\":0,\"byteOffset\":972,\"byteLength\":768},"
                          "{\"buffer\":0,\"byteOffset\":972,\"byteLength\":768}],%s,%s}",
                    (unsigned)plainSize, b64Plain, accessors, mesh);
        else
            sprintf(json, "{\"asset\":{\"version\":\"2.0\"},\"extensionsRequired\":[\"EXT_meshopt_compression\"],\"extensionsUsed\":[\"EXT_meshopt_compression\"],"
                          "\"buffers\":[{\"byteLength\":%u,\"uri\":\"data:application/octet-stream;base64,%s\"},{\"byteLength\":1740,\"extensions\":{\"EXT_meshopt_compression\":{\"fallback\":true}}}],"
                          "\"bufferViews\":[{\"buffer\":1,\"byteOffset\":0,\"byteLength\":972,\"byteStride\":12,\"extensions\":{\"EXT_meshopt_compression\":{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%u,\"byteStride\":12,\"count\":81,\"mode\":\"ATTRIBUTES\"}}},"
                          "{\"buffer\":1,\"byteOffset\":972,\"byteLength\":768,\"extensions\":{\"EXT_meshopt_compression\":{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"byteStride\":2,\"count\":384,\"mode\":\"TRIANGLES\"}}},"
                          "{\"buffer\":1,\"byteOffset\":972,\"byteLength\":768,\"extensions\":{\"EXT_meshopt_compression\":{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u,\"byteStride\":2,\"count\":384,\"mode\":\"INDICES\"}}}],%s,%s}",
                    (unsigned)packedSize, b64Packed, (unsigned)v.n, (unsigned)v.n, (unsigned)tri.n, (unsigned)(v.n + tri.n), (unsigned)seq.n, accessors, mesh);
        scenes[k] = plainScene();
        setCamera(scenes[k], 0, 2, 2.5f, 0, -.3f, 0);
        artfSceneSetSun(scenes[k], artfVec3(-.4f, -1, -.2f), artfVec3(1, 1, 1));
        artfSceneSetAmbient(scenes[k], artfVec3(.1f, .1f, .1f), artfVec3(.1f, .1f, .1f));
        CHECK(artfSceneLoadModelMemory(scenes[k], json, strlen(json), NULL, NULL) != 0);
    }
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    drawInto(cpu, scenes[0], a);
    drawInto(cpu, scenes[1], b);
    CHECK(differentShare(a, b, 0) == 0 && meanOf(a, W / 2, H / 2, 8) > 20);
    artfRendererFree(cpu);
    artfSceneFree(scenes[0]);
    artfSceneFree(scenes[1]);
    artfFree(a);
    artfFree(b);
    free(json); free(b64Plain); free(b64Packed); free(plain); free(packed); free(v.p); free(tri.p); free(seq.p);

    /* the filters: an octahedron's normal, a quaternion, a float from a mantissa and an exponent */
    int8_t oct[4] = {0, 127, 127, 0}; /* (up: u 0, v 1, and the one length) */
    artfMeshoptFilter((uint8_t*)oct, 1, 4, 1);
    CHECK(oct[0] == 0 && oct[1] == 127 && oct[2] == 0);
    int16_t quat[4] = {0, 0, 0, 3 | (32767 & ~3)}; /* (the identity: w left out, as index 3 says; x, y, z nothing) */
    quat[3] = (int16_t)(32767 & ~3) | 3;
    artfMeshoptFilter((uint8_t*)quat, 1, 8, 2);
    CHECK(quat[3] == 32767 && quat[0] == 0 && quat[1] == 0 && quat[2] == 0);
    uint32_t e = (uint32_t)(-2 & 255) << 24 | 6u; /* 6 * 2^-2 */
    artfMeshoptFilter((uint8_t*)&e, 1, 4, 3);
    float ef;
    memcpy(&ef, &e, 4);
    CHECK(ef == 1.5f);

    /* materials: lit by nothing, an index of 1.2, light that goes through; and an orthographic camera */
    static const float corners[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    char b64[64];
    base64Put(b64, (const uint8_t*)corners, sizeof corners);
    char file[2048];
    snprintf(file, sizeof file,
             "{\"asset\":{\"version\":\"2.0\"},\"extensionsUsed\":[\"KHR_materials_unlit\",\"KHR_materials_ior\",\"KHR_materials_transmission\"],"
             "\"buffers\":[{\"byteLength\":36,\"uri\":\"data:application/octet-stream;base64,%s\"}],\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],"
             "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"type\":\"VEC3\",\"count\":3,\"max\":[1,1,0],\"min\":[0,0,0]}],"
             "\"materials\":[{\"extensions\":{\"KHR_materials_unlit\":{}}},{\"extensions\":{\"KHR_materials_ior\":{\"ior\":1.2}}},"
             "{\"extensions\":{\"KHR_materials_transmission\":{\"transmissionFactor\":0.8},\"KHR_materials_ior\":{\"ior\":1.33}}}],"
             "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0},{\"attributes\":{\"POSITION\":0},\"material\":1},"
             "{\"attributes\":{\"POSITION\":0},\"material\":2}]}],"
             "\"cameras\":[{\"type\":\"orthographic\",\"orthographic\":{\"xmag\":2,\"ymag\":1.5,\"znear\":0.1,\"zfar\":50}}],"
             "\"nodes\":[{\"mesh\":0},{\"translation\":[0,0,5],\"camera\":0}],\"scenes\":[{\"nodes\":[0,1]}],\"scene\":0}",
             b64);
    ArtfScene* s = artfSceneNew();
    const ArtfModel m = artfSceneLoadModelMemory(s, file, strlen(file), NULL, NULL);
    CHECK(m != 0 && s->materials.len >= 3);
    const ArtfMaterialData* made = &s->materials.items[s->materials.len - 3];
    CHECK(made[0].flags & ARTF_MATERIAL_UNLIT);
    CHECK(fabsf(made[1].specular - .2066f) < .001f && !(made[1].flags & ARTF_MATERIAL_UNLIT));
    CHECK(made[2].flags & ARTF_MATERIAL_BLEND && fabsf(made[2].opacity - .2f) < 1e-5f && fabsf(made[2].ior - 1.33f) < 1e-5f);
    ArtfCamera eye;
    CHECK(artfModelCameraCount(s, m) == 1 && artfModelCamera(s, m, 0, &eye));
    /* orthographic, twice ymag high, from where the camera is, as far as zfar */
    CHECK(eye.orthoHeight == 3.f && fabsf(eye.near_ - .1f) < 1e-6f && eye.far_ == 50.f);
    CHECK(fabsf(eye.position.z - 5.f) < 1e-4f && eye.target.z < eye.position.z);
    artfSceneFree(s);
}

/* Light from all around, from a picture: the maps, a Radiance file, and both backends drawing the same. */
static void testEnvironment(void) {
    /* a picture lit above and dark below, with a brighter patch where -z is */
    enum { PW = 64, PH = 32 };
    float* picture = artfMalloc(PW * PH * 12);
    for (int y = 0; y < PH; ++y)
        for (int x = 0; x < PW; ++x) {
            float* p = picture + 3 * (y * PW + x);
            const float v = y < PH / 2 ? 1.f : 0.f;
            p[0] = p[1] = p[2] = v;
            if (y >= PH / 2 - 4 && y < PH / 2 && x >= PW / 2 - 2 && x < PW / 2 + 2) p[0] = 50.f;
        }
    ArtfScene* s = artfSceneNew();
    CHECK(artfSceneSetEnvironment(s, picture, PW, PH, 1.f, true));
    float c[3];
    artfEnvSample(s->environment, ARTF_ENV_DIFFUSE, 0, 1, 0, c);
    CHECK(fabsf(c[1] - 1.f) < .02f);
    artfEnvSample(s->environment, ARTF_ENV_DIFFUSE, 0, -1, 0, c);
    CHECK(c[1] < .02f);
    artfEnvSample(s->environment, ARTF_ENV_DIFFUSE, 1, 0, 0, c);
    CHECK(fabsf(c[1] - .5f) < .05f);
    /* the patch is toward -z, a little above the horizon: sharp in the first map, spread in the rough ones */
    float sharp[3], away[3], rough[3];
    artfEnvSample(s->environment, 0, 0, .1f, -1, sharp);
    artfEnvSample(s->environment, 0, 0, .1f, 1, away);
    artfEnvSample(s->environment, 4, 0, .1f, -1, rough);
    CHECK(sharp[0] > 20.f && away[0] < 1.5f && rough[0] > 1.f && rough[0] < sharp[0]);
    /* the border repeats what is across the fold: no seam below */
    float left[3], right[3];
    artfEnvSample(s->environment, 2, -.01f, -1, .3f, left);
    artfEnvSample(s->environment, 2, .01f, -1, .3f, right);
    CHECK(fabsf(left[1] - right[1]) < .02f);

    /* the same scene, on both backends */
    const ArtfMesh ball = artfSceneAddSphere(s, 1, 32, 48);
    ArtfMaterialDesc metal = artfMaterialDefaults();
    metal.baseColor = artfVec3(.9f, .9f, .9f);
    metal.metallic = 1;
    metal.roughness = .3f;
    artfSceneAddInstance(s, ball, artfSceneAddMaterial(s, &metal), NULL);
    artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(0, 0, 0));
    const ArtfCamera eye = {{0, .5f, 3.5f}, {0, 0, 0}, {0, 1, 0}, .9f, .1f};
    artfSceneSetCamera(s, &eye);
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    drawInto(cpu, s, a);
    /* the background is the picture: dark below, lit above */
    CHECK((a[(H - 1) * W] & 255) < 20 && (a[0] & 255) > 200);
    o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 24);
        printf("light from all around, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .01);
        artfRendererFree(gpu);
    }
    /* without it, the ball shows sky and ground instead */
    artfSceneSetEnvironment(s, NULL, 0, 0, 0, false);
    drawInto(cpu, s, b);
    CHECK(differentShare(a, b, 24) > .2);
    artfRendererFree(cpu);
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfFree(picture);

    /* a Radiance file, flat and in runs: 1, 2 and 0.5 */
    static const uint8_t flat[] = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n\x80\x80\x80\x81\x80\x80\x80\x82";
    int fw = 0, fh = 0;
    float* got = artfImageDecodeHdr(flat, sizeof flat - 1, &fw, &fh);
    CHECK(got && fw == 2 && fh == 1 && got[0] == 1.f && got[3] == 2.f);
    artfFree(got);
    uint8_t runs[64 + 8 * 4];
    const char head[] = "#?RADIANCE\n\n-Y 1 +X 8\n";
    size_t at = sizeof head - 1;
    memcpy(runs, head, at);
    runs[at++] = 2; runs[at++] = 2; runs[at++] = 0; runs[at++] = 8;
    for (int ch = 0; ch < 4; ++ch) { runs[at++] = 128 + 8; runs[at++] = ch == 3 ? 128 : 128; }
    got = artfImageDecodeHdr(runs, at, &fw, &fh);
    CHECK(got && fw == 8 && got[21] == .5f);
    artfFree(got);
    CHECK(!artfImageDecodeHdr(runs, at - 3, &fw, &fh));
    CHECK(!artfImageDecodeHdr("not one", 7, &fw, &fh));
}

/* A frame in floats: the same picture as before with nothing after it; each effect the same on both backends; lights
 * past the frame's own; light that bounced. */
static ArtfScene* bleedScene(void) {
    ArtfScene* s = plainScene();
    const ArtfMaterial white = whiteMaterial(s);
    artfSceneSetSun(s, artfVec3(-.6f, -1, -.3f), artfVec3(.5f, .5f, .5f));
    artfSceneSetAmbient(s, artfVec3(.05f, .05f, .05f), artfVec3(.05f, .05f, .05f));
    setCamera(s, 0, 3, 7, 0, 0, 0);
    ArtfMaterialDesc redWall = {artfVec3(1, .02f, .02f), 0, 0, 1, artfVec3(0, 0, 0)};
    ArtfTransform t = artfTransformAt(0, -1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 40, 1), white, &t);
    const ArtfTransform wall = artfTransformTrs(artfVec3(2.2f, 1, 0), artfQuatAxisAngle(artfVec3(0, 1, 0), -1.5708f), artfVec3(4, 2, 1));
    artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &redWall), &wall);
    const ArtfMesh ball = artfSceneAddSphere(s, .8f, 24, 32);
    ArtfMaterialDesc mirrorDesc = artfMaterialDefaults();
    mirrorDesc.baseColor = artfVec3(.9f, .9f, .9f);
    mirrorDesc.metallic = 1;
    mirrorDesc.roughness = .1f;
    addAt(s, ball, artfSceneAddMaterial(s, &mirrorDesc), -1, -.2f, 0, 1);
    return s;
}

static void testFloats(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* c = artfMalloc(W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* plain = artfRendererNew(&o);
        o.hdr = true;
        ArtfRenderer* floats = artfRendererNew(&o);
        if (!plain || !floats) {
            artfRendererFree(plain);
            artfRendererFree(floats);
            continue;
        }
        ArtfScene* s = bleedScene();
        drawInto(plain, s, a);
        drawInto(floats, s, b);
        /* nothing after it: the picture as it was, to a level */
        int worst = 0;
        for (int i = 0; i < W * H; ++i)
            for (int k = 0; k < 24; k += 8) worst = artfMaxi(worst, abs((int)(a[i] >> k & 255) - (int)(b[i] >> k & 255)));
        printf("%s, a frame in floats against one in bytes: %d at most\n", backend ? "GPU" : "CPU", worst);
        CHECK(worst <= 1);
        /* each effect changes the picture, and the same way every time */
        const ArtfPostDesc each[6] = {{.bloom = .5f, .bloomThreshold = .1f}, {.occlusion = 1, .occlusionRadius = .8f}, {.reflections = 1}, {.focus = 3, .aperture = 10},
                                      {.volumetric = 1, .scattering = .5f}, {.temporal = true}};
        for (int e = 0; e < 6; ++e) {
            artfSceneSetPost(s, &each[e]);
            if (e == 4) artfSceneSetFog(s, artfVec3(.5f, .5f, .55f), .05f);
            drawInto(floats, s, c);
            const double changed = differentShare(b, c, 2);
            if (changed <= (e == 5 || e == 2 ? .001 : .01)) printf("effect %d changed %.4f of the pixels\n", e, changed);
            CHECK(changed > (e == 5 || e == 2 ? .001 : .01));
            if (e < 5) {
                drawInto(floats, s, a);
                CHECK(differentShare(a, c, 0) == 0);
            }
            if (e == 4) artfSceneSetFog(s, artfVec3(0, 0, 0), 0);
        }
        artfSceneSetPost(s, NULL);
        /* light that bounced: the floor by the red wall turns red */
        const int fx = W / 2 + 28, fy = H / 2 + 34;
        drawInto(plain, s, a);
        const ArtfIndirectDesc probes = {{8, 4, 8}, 64, .5f, 1, {-4, -1.5f, -3}, {4, 3, 3}};
        artfSceneSetIndirect(s, &probes);
        for (int k = 0; k < 6; ++k) drawInto(plain, s, b);
        const double before = meanOf(a, fx, fy, 16) - meanOf(a, fx, fy, 8), after = meanOf(b, fx, fy, 16) - meanOf(b, fx, fy, 8);
        printf("%s, light that bounced: red over green by the wall, %.1f without, %.1f with\n", backend ? "GPU" : "CPU", before, after);
        CHECK(after > before + 3);
        artfSceneFree(s);
        artfRendererFree(plain);
        artfRendererFree(floats);
    }
    /* the backends agree, on each effect and on light that bounced */
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    o.hdr = true;
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        o.backend = ARTF_BACKEND_SOFTWARE;
        ArtfRenderer* cpu = artfRendererNew(&o);
        const ArtfPostDesc all = {.bloom = .2f, .occlusion = 1, .reflections = 1, .focus = 6, .aperture = 6};
        ArtfScene* s = bleedScene();
        artfSceneSetPost(s, &all);
        const ArtfIndirectDesc probes = {{8, 4, 8}, 64, .5f, 1, {-4, -1.5f, -3}, {4, 3, 3}};
        artfSceneSetIndirect(s, &probes);
        drawInto(cpu, s, a);
        artfSceneSetIndirect(s, &probes); /* (the probes start over, so both see the same steps) */
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 16);
        printf("all effects and probes, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .03);
        /* lights past the frame's own */
        artfSceneSetPost(s, NULL);
        artfSceneSetIndirect(s, NULL);
        artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(0, 0, 0));
        uint32_t st = 5;
        for (int i = 0; i < 100; ++i) {
            st = st * 747796405u + 2891336453u;
            const float x = (float)(st >> 8 & 255) / 255.f * 8.f - 4.f, z = (float)(st >> 16 & 255) / 255.f * 6.f - 3.f;
            CHECK(artfSceneAddLight(s, artfVec3(x, -.6f, z), artfVec3((float)(i % 3 == 0), (float)(i % 3 == 1), (float)(i % 3 == 2)), 1.5f) != 0);
        }
        drawInto(cpu, s, a);
        drawInto(gpu, s, b);
        CHECK(meanOf(a, W / 2, H * 3 / 4, 16) > 30);
        const double lightsApart = differentShare(a, b, 16);
        printf("100 lights past the frame's own, the GPU against the CPU: %.2f%% of pixels apart\n", lightsApart * 100);
        CHECK(lightsApart < .02);
        artfSceneFree(s);
        artfRendererFree(cpu);
        artfRendererFree(gpu);
    }
    artfFree(a);
    artfFree(b);
    artfFree(c);
}

/* The look: log2 and 2^x as the metering has them; each setting changes the picture the same way every time, on both
 * backends, and they agree; auto exposure brings a dim scene and a bright one together. */
static double frameMean(const uint32_t* f) {
    double sum = 0;
    for (int i = 0; i < W * H; ++i) sum += CHANNEL(f[i], 0) + CHANNEL(f[i], 8) + CHANNEL(f[i], 16);
    return sum / (3.0 * W * H);
}

static void testGrading(void) {
    double worstLog = 0, worstExp = 0;
    for (float x = 1.f / 8192.f; x < 8192.f; x *= 1.0137f) worstLog = fmax(worstLog, fabs(artfLog2Fast(x) - log2(x)));
    for (float x = -30.f; x < 30.f; x += .0173f) worstExp = fmax(worstExp, fabs(artfExp2Fast(x) / exp2(x) - 1));
    printf("log2 within %.2g, 2^x within %.2g of its value\n", worstLog, worstExp);
    CHECK(worstLog < 5e-5 && worstExp < 2e-5);

    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* c = artfMalloc(W * H * 4);
    const ArtfPostDesc each[9] = {{.toneMap = ARTF_TONE_NEUTRAL}, {.toneMap = ARTF_TONE_REINHARD}, {.toneMap = ARTF_TONE_NONE},
                                  {.temperature = .8f}, {.tint = -.6f}, {.saturation = -.7f}, {.contrast = .5f}, {.vignette = 1}, {.chromatic = 6}};
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.hdr = true;
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        ArtfScene* s = bleedScene();
        artfSceneSetSun(s, artfVec3(-.6f, -1, -.3f), artfVec3(3, 3, 3));
        artfSceneSetExposure(s, 1);
        drawInto(r, s, b);
        for (int e = 0; e < 9; ++e) {
            artfSceneSetPost(s, &each[e]);
            drawInto(r, s, c);
            const double changed = differentShare(b, c, 2);
            if (changed <= .01) printf("%s, look %d changed %.4f of the pixels\n", backend ? "GPU" : "CPU", e, changed);
            CHECK(changed > .01);
            drawInto(r, s, a);
            CHECK(differentShare(a, c, 0) == 0);
        }
        /* nothing asked: the same bytes as before there was a look */
        const ArtfPostDesc none = {0};
        artfSceneSetPost(s, &none);
        drawInto(r, s, c);
        CHECK(differentShare(b, c, 0) == 0);
        /* auto exposure: the sun eight times as bright, nearly the same picture */
        const ArtfPostDesc meter = {.autoExposure = true, .exposureSpeed = 1};
        artfSceneSetPost(s, &meter);
        artfSceneSetSun(s, artfVec3(-.6f, -1, -.3f), artfVec3(.5f, .5f, .5f));
        artfSceneSetAmbient(s, artfVec3(.02f, .02f, .02f), artfVec3(.02f, .02f, .02f));
        drawInto(r, s, a);
        artfSceneSetSun(s, artfVec3(-.6f, -1, -.3f), artfVec3(4, 4, 4));
        artfSceneSetAmbient(s, artfVec3(.16f, .16f, .16f), artfVec3(.16f, .16f, .16f));
        drawInto(r, s, b);
        const double dim = frameMean(a), bright = frameMean(b);
        artfSceneSetPost(s, NULL);
        artfSceneSetSun(s, artfVec3(-.6f, -1, -.3f), artfVec3(.5f, .5f, .5f));
        artfSceneSetAmbient(s, artfVec3(.02f, .02f, .02f), artfVec3(.02f, .02f, .02f));
        drawInto(r, s, c);
        printf("%s, auto exposure: a sun 8 times as bright shows %.1f against %.1f; the dim one %.1f without it\n", backend ? "GPU" : "CPU", bright, dim, frameMean(c));
        CHECK(fabs(bright - dim) < 8 && frameMean(c) < dim - 30);
        artfSceneSetSun(s, artfVec3(-.6f, -1, -.3f), artfVec3(4, 4, 4));
        artfSceneSetAmbient(s, artfVec3(.16f, .16f, .16f), artfVec3(.16f, .16f, .16f));
        /* and slowly: a twentieth of the way a frame */
        const ArtfPostDesc slow = {.autoExposure = true};
        artfSceneSetPost(s, &slow);
        drawInto(r, s, a); /* (from what it had: a state goes on while the renderer meters) */
        artfSceneSetSun(s, artfVec3(-.6f, -1, -.3f), artfVec3(.5f, .5f, .5f));
        artfSceneSetAmbient(s, artfVec3(.02f, .02f, .02f), artfVec3(.02f, .02f, .02f));
        drawInto(r, s, b);
        const double first = frameMean(b);
        for (int k = 0; k < 60; ++k) drawInto(r, s, b);
        printf("%s, auto exposure going up: %.1f, then %.1f after 60 frames\n", backend ? "GPU" : "CPU", first, frameMean(b));
        CHECK(frameMean(b) > first + 10);
        artfSceneFree(s);
        artfRendererFree(r);
    }
    /* the backends agree, all of it at once */
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    o.hdr = true;
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        o.backend = ARTF_BACKEND_SOFTWARE;
        ArtfRenderer* cpu = artfRendererNew(&o);
        for (int curve = 0; curve < 4; ++curve) {
            ArtfScene* s = bleedScene();
            artfSceneSetSun(s, artfVec3(-.6f, -1, -.3f), artfVec3(3, 3, 3));
            const ArtfPostDesc all = {.bloom = .1f, .toneMap = (ArtfToneMap)curve, .autoExposure = true, .exposureSpeed = 1, .temperature = .4f,
                                      .tint = .2f, .saturation = .3f, .contrast = .2f, .vignette = .6f, .chromatic = 3};
            artfSceneSetPost(s, &all);
            drawInto(cpu, s, a);
            drawInto(gpu, s, b);
            const double apart = differentShare(a, b, 16);
            printf("the look with curve %d, the GPU against the CPU: %.2f%% of pixels apart\n", curve, apart * 100);
            CHECK(apart < .02);
            artfSceneFree(s);
        }
        artfRendererFree(cpu);
        artfRendererFree(gpu);
    }
    artfFree(a);
    artfFree(b);
    artfFree(c);
}

/* A clear coat and sheen: each changes the picture, where it should; the two backends agree, drawing and tracing; and
 * glTF's extensions reach them. */
static ArtfScene* coatScene(float coat, float sheen) {
    ArtfScene* s = plainScene();
    artfSceneSetSun(s, artfVec3(-.5f, -1, -.6f), artfVec3(2, 2, 2));
    artfSceneSetAmbient(s, artfVec3(.2f, .25f, .3f), artfVec3(.05f, .05f, .05f));
    setCamera(s, 0, .5f, 4, 0, 0, 0);
    ArtfMaterialDesc d = artfMaterialDefaults();
    d.baseColor = artfVec3(.5f, .05f, .05f);
    d.roughness = .8f;
    d.clearcoat = coat;
    d.clearcoatRoughness = .1f;
    d.sheen = artfVec3(sheen, sheen, sheen);
    d.sheenRoughness = .5f;
    addAt(s, artfSceneAddSphere(s, 1.2f, 48, 64), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
    return s;
}

static void testCoat(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        ArtfScene* plain = coatScene(0, 0);
        ArtfScene* coated = coatScene(1, 0);
        ArtfScene* velvet = coatScene(0, 1);
        drawInto(r, plain, a);
        drawInto(r, coated, b);
        /* a sharp highlight where there was a dull one */
        int brightest[2] = {0, 0};
        for (int i = 0; i < W * H; ++i) {
            brightest[0] = artfMaxi(brightest[0], CHANNEL(a[i], 8));
            brightest[1] = artfMaxi(brightest[1], CHANNEL(b[i], 8));
        }
        printf("%s, a clear coat: its highlight's green %d, %d without\n", backend ? "GPU" : "CPU", brightest[1], brightest[0]);
        CHECK(brightest[1] > brightest[0] + 60 && differentShare(a, b, 2) > .05);
        /* sheen: the rim, not the middle */
        drawInto(r, velvet, b);
        const int rimX = W / 2 + (int)(H * .28f), midX = W / 2;
        const double rim = meanOf(b, rimX, H / 2, 16) - meanOf(a, rimX, H / 2, 16), middle = meanOf(b, midX, H / 2, 16) - meanOf(a, midX, H / 2, 16);
        printf("%s, sheen: the rim %.1f brighter, the middle %.1f\n", backend ? "GPU" : "CPU", rim, middle);
        CHECK(rim > 8 && rim > middle * 2);
        artfSceneFree(plain);
        artfSceneFree(coated);
        artfSceneFree(velvet);
        artfRendererFree(r);
    }
    /* both at once, on both backends: drawn, and traced */
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        o.backend = ARTF_BACKEND_SOFTWARE;
        ArtfRenderer* cpu = artfRendererNew(&o);
        ArtfScene* s = coatScene(.8f, .6f);
        drawInto(cpu, s, a);
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 24);
        printf("a clear coat and sheen, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .005);
        ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 32, 0};
        artfSceneSetTrace(s, &rays);
        for (int k = 0; k < 32; ++k) drawInto(cpu, s, a);
        for (int k = 0; k < 32; ++k) drawInto(gpu, s, b);
        const double tracedApart = differentShare(a, b, 24);
        printf("a clear coat and sheen traced, the GPU against the CPU: %.2f%% of pixels apart\n", tracedApart * 100);
        CHECK(tracedApart < .01);
        artfSceneFree(s);
        artfRendererFree(cpu);
        artfRendererFree(gpu);
    }
    artfFree(a);
    artfFree(b);
    /* from glTF */
    static const char file[] =
        "{\"asset\":{\"version\":\"2.0\"},\"extensionsRequired\":[\"KHR_materials_clearcoat\",\"KHR_materials_sheen\"],"
        "\"buffers\":[{\"byteLength\":36,\"uri\":\"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"type\":\"VEC3\","
        "\"count\":3,\"max\":[1,1,0],\"min\":[0,0,0]}],\"materials\":[{\"extensions\":{\"KHR_materials_clearcoat\":{\"clearcoatFactor\":0.75,"
        "\"clearcoatRoughnessFactor\":0.25},\"KHR_materials_sheen\":{\"sheenColorFactor\":[0.5,0.25,1]}}}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}]}";
    ArtfScene* s = artfSceneNew();
    CHECK(artfSceneLoadModelMemory(s, file, sizeof file - 1, NULL, NULL) != 0);
    const ArtfMaterialMore* more = &s->materialsMore.items[s->materialsMore.len - 1];
    CHECK(s->materialsMore.len == s->materials.len && more->coat == .75f && more->coatRoughness == .25f);
    CHECK(more->sheen[0] == .5f && more->sheen[1] == .25f && more->sheen[2] == 1.f && s->layeredMaterials == 1);
    ArtfMaterialDesc none = artfMaterialDefaults();
    artfMaterialSet(s, (ArtfMaterial)(s->materials.len - 1), &none);
    CHECK(s->layeredMaterials == 0);
    artfSceneFree(s);
}

/* A sky from the sun: blue overhead by day, red toward a low sun, the sun reddened and then gone; the same on both
 * backends. Fog by height: thinner above. */
static void testSky(void) {
    ArtfScene* s = artfSceneNew();
    const ArtfSkyDesc day = {{3, 3, 3}, 0, 0, {0, 0, 0}, 128, false, false};
    float up[3], side[3];
    artfSceneSetSun(s, artfVec3(-.2f, -1, -.3f), artfVec3(3, 3, 3));
    CHECK(artfSceneSetSky(s, &day));
    artfEnvSample(s->environment, 0, 0, 1, 0, up);
    artfEnvSample(s->environment, 0, 1, .02f, 0, side);
    printf("a sky at noon: overhead %.3f %.3f %.3f, at the horizon %.3f %.3f %.3f; the sun %.2f %.2f %.2f\n", up[0], up[1], up[2], side[0], side[1], side[2],
           s->sunColor.x, s->sunColor.y, s->sunColor.z);
    CHECK(up[2] > up[1] && up[1] > up[0] && up[2] > .05f && side[1] > up[1] && s->sunColor.x > 2.5f && s->sunColor.z < s->sunColor.x);
    /* the sun low: toward it, red over blue; and the sun itself reddened */
    artfSceneSetSun(s, artfVec3(0, -.03f, 1), artfVec3(3, 3, 3));
    CHECK(artfSceneSetSky(s, &day));
    float toward[3];
    artfEnvSample(s->environment, 0, 0, .03f, -1, toward);
    printf("a sky at sunset: toward the sun %.3f %.3f %.3f; the sun %.3f %.3f %.3f\n", toward[0], toward[1], toward[2], s->sunColor.x, s->sunColor.y, s->sunColor.z);
    CHECK(toward[0] > toward[2] && s->sunColor.x > s->sunColor.z * 3);
    /* below the horizon: no sun */
    artfSceneSetSun(s, artfVec3(0, .3f, 1), artfVec3(3, 3, 3));
    CHECK(artfSceneSetSky(s, &day));
    CHECK(s->sunColor.x == 0 && s->sunColor.y == 0 && s->sunColor.z == 0);
    /* the same bytes, made again */
    const size_t size = ARTF_ENV_FLOATS * sizeof(float);
    float* first = artfMalloc(size);
    memcpy(first, s->environment, size);
    CHECK(artfSceneSetSky(s, &day));
    CHECK(memcmp(first, s->environment, size) == 0);
    artfFree(first);
    CHECK(artfSceneSetSky(s, NULL) && !s->environment);
    artfSceneFree(s);

    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    o.hdr = true;
    ArtfRenderer* gpu = artfRendererNew(&o);
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    s = bleedScene();
    artfSceneSetSun(s, artfVec3(-.6f, -.4f, -.3f), artfVec3(3, 3, 3));
    CHECK(artfSceneSetSky(s, &day));
    artfSceneSetExposure(s, 1);
    drawInto(cpu, s, a);
    /* the sky shows behind the scene: blue at the top */
    CHECK(CHANNEL(a[W * 4 + W / 2], 0) > CHANNEL(a[W * 4 + W / 2], 16));
    if (gpu) {
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 24);
        printf("a sky, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .005);
    }
    /* fog by height, in fog that light goes through: thinner above, on both backends alike */
    const ArtfPostDesc fog = {.volumetric = 1, .scattering = .3f};
    artfSceneSetPost(s, &fog);
    artfSceneSetFog(s, artfVec3(.5f, .5f, .55f), .08f);
    drawInto(cpu, s, b);
    artfSceneSetFogHeight(s, -1, 2);
    drawInto(cpu, s, a);
    printf("fog by height: %.1f%% of pixels changed\n", differentShare(a, b, 8) * 100);
    CHECK(differentShare(a, b, 8) > .05);
    if (gpu) {
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 24);
        printf("fog by height, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .01);
    }
    artfSceneFree(s);
    artfRendererFree(cpu);
    artfRendererFree(gpu);
    artfFree(a);
    artfFree(b);
}

/* A light with a size: its highlight on something smooth is wider, the same on both backends; traced, its shadow is
 * softer. */
static ArtfScene* bulbScene(float size) {
    ArtfScene* s = plainScene();
    artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(0, 0, 0));
    artfSceneSetAmbient(s, artfVec3(.02f, .02f, .02f), artfVec3(.02f, .02f, .02f));
    setCamera(s, 0, 2.5f, 5, 0, 0, 0);
    ArtfMaterialDesc smooth = artfMaterialDefaults();
    smooth.baseColor = artfVec3(.2f, .2f, .25f);
    smooth.roughness = .08f;
    const ArtfTransform floor = artfTransformAt(0, -1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 20, 1), artfSceneAddMaterial(s, &smooth), &floor);
    addAt(s, artfSceneAddCube(s, .6f), whiteMaterial(s), 0, .1f, 0, 1);
    const ArtfLight bulb = artfSceneAddLight(s, artfVec3(0, 1.6f, -.6f), artfVec3(1.5f, 1.5f, 1.5f), 12);
    artfLightSetSize(s, bulb, size);
    return s;
}

static int brightCount(const uint32_t* f, int over) {
    int n = 0;
    for (int i = 0; i < W * H; ++i) n += CHANNEL(f[i], 8) > over;
    return n;
}
static int greyCount(const uint32_t* f) {
    int n = 0;
    for (int i = 0; i < W * H; ++i) n += CHANNEL(f[i], 8) > 25 && CHANNEL(f[i], 8) < 90;
    return n;
}

static void testSizedLights(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        ArtfScene* point = bulbScene(0);
        ArtfScene* bulb = bulbScene(.5f);
        drawInto(r, point, a);
        drawInto(r, bulb, b);
        printf("%s, a light with a size: %d bright pixels, %d from a point\n", backend ? "GPU" : "CPU", brightCount(b, 150), brightCount(a, 150));
        CHECK(brightCount(b, 150) > brightCount(a, 150) * 2);
        artfSceneFree(point);
        artfSceneFree(bulb);
        artfRendererFree(r);
    }
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    ArtfScene* s = bulbScene(.5f);
    if (gpu) {
        drawInto(cpu, s, a);
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 24);
        printf("a light with a size, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .005);
    }
    /* traced: the cube's shadow has a soft edge */
    ArtfScene* point = bulbScene(0);
    const ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 64, 0};
    artfSceneSetTrace(s, &rays);
    artfSceneSetTrace(point, &rays);
    for (int k = 0; k < 64; ++k) drawInto(cpu, point, a);
    for (int k = 0; k < 64; ++k) drawInto(cpu, s, b);
    printf("traced, a light with a size: %d pixels between light and shadow, %d from a point\n", greyCount(b), greyCount(a));
    CHECK(greyCount(b) > greyCount(a) + 200);
    if (gpu) {
        for (int k = 0; k < 64; ++k) drawInto(gpu, s, a);
        const double apart = differentShare(a, b, 24);
        printf("traced, a light with a size, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .02);
    }
    /* removed, it is a point again when its place is taken */
    artfSceneRemoveLight(point, 1);
    CHECK(point->lightSize[0] == 0);
    artfSceneFree(point);
    artfSceneFree(s);
    artfRendererFree(cpu);
    artfRendererFree(gpu);
    artfFree(a);
    artfFree(b);
}

/* What moves, followed: with nothing moving, motion blur changes no byte; a sphere going sideways is blurred along its
 * way and not across it; temporal antialiasing follows it, so it leaves no trail; both backends alike. */
static ArtfScene* movingScene(ArtfInstance* ball) {
    ArtfScene* s = plainScene();
    artfSceneSetSun(s, artfVec3(-.3f, -1, -.4f), artfVec3(1.5f, 1.5f, 1.5f));
    setCamera(s, 0, 0, 6, 0, 0, 0);
    ArtfMaterialDesc red = artfMaterialDefaults();
    red.baseColor = artfVec3(.9f, .1f, .1f);
    *ball = addAt(s, artfSceneAddSphere(s, .8f, 32, 48), artfSceneAddMaterial(s, &red), -1.2f, 0, 0, 1);
    return s;
}

/* One frame, and what it shows: drawInto draws three, and what moved has stopped by the last. */
static void drawOnce(ArtfRenderer* r, ArtfScene* s, uint32_t* frame) {
    CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, frame));
}

static void placeBall(ArtfScene* s, ArtfInstance ball, float x) {
    const ArtfTransform at = artfTransformAt(x, 0, 0, 1);
    artfInstanceSetTransform(s, ball, &at);
}

/* The mean difference of a channel, over the frame. */
static double meanApart(const uint32_t* a, const uint32_t* b) {
    double sum = 0;
    for (int i = 0; i < W * H; ++i) sum += abs(CHANNEL(a[i], 16) - CHANNEL(b[i], 16));
    return sum / (W * H);
}

static void testMotion(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* c = artfMalloc(W * H * 4);
    uint32_t* blurred[2] = {artfMalloc(W * H * 4), artfMalloc(W * H * 4)};
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.hdr = true;
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        ArtfInstance ball;
        ArtfScene* s = movingScene(&ball);
        /* still: not a byte changes */
        drawInto(r, s, a);
        const ArtfPostDesc blur = {.motionBlur = .5f};
        artfSceneSetPost(s, &blur);
        drawInto(r, s, b);
        drawInto(r, s, b);
        CHECK(differentShare(a, b, 0) == 0);
        /* going sideways: blurred along its way; above and below it, nothing changes */
        placeBall(s, ball, -.6f);
        drawOnce(r, s, blurred[backend]);
        artfSceneSetPost(s, NULL);
        drawInto(r, s, a);
        const double changed = differentShare(a, blurred[backend], 8);
        int across = 0;
        for (int y = 0; y < H / 5; ++y)
            for (int x = 0; x < W; ++x) across += a[y * W + x] != blurred[backend][y * W + x];
        printf("%s, motion blur: %.2f%% of pixels blurred, %d across its way\n", backend ? "GPU" : "CPU", changed * 100, across);
        CHECK(changed > .003 && changed < .1 && across == 0);
        /* temporal antialiasing, the sphere going on: what it shows against the sphere drawn still there */
        const ArtfPostDesc temporal = {.temporal = true};
        artfSceneSetPost(s, &temporal);
        for (int k = 0; k < 12; ++k) {
            placeBall(s, ball, -.9f + (float)k * .15f);
            drawOnce(r, s, b);
        }
        artfSceneSetPost(s, NULL);
        drawInto(r, s, c);
        const double trail = meanApart(b, c);
        printf("%s, temporal antialiasing on a moving sphere: %.2f of 255 from it drawn still, on average\n", backend ? "GPU" : "CPU", trail);
        CHECK(trail < 1.5);
        artfSceneFree(s);
        artfRendererFree(r);
    }
    if (blurred[1][0] || blurred[1][W * H / 2]) {
        const double apart = differentShare(blurred[0], blurred[1], 24);
        printf("motion blur, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .005);
    }
    artfFree(a);
    artfFree(b);
    artfFree(c);
    artfFree(blurred[0]);
    artfFree(blurred[1]);
}

/* Soft shadows, drawn: as wide as their light, for the sun and a light with a size; sharp ones as they were; both
 * backends alike. */
static void testSoftShadows(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* c[2] = {artfMalloc(W * H * 4), artfMalloc(W * H * 4)};
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        for (int sun = 0; sun < 2; ++sun) {
            ArtfScene* s = bulbScene(.5f);
            if (sun) {
                artfSceneRemoveLight(s, 1);
                artfSceneSetSun(s, artfVec3(.3f, -1, .4f), artfVec3(1.5f, 1.5f, 1.5f));
                artfSceneSetSunSize(s, .08f);
            } else {
                artfLightSetShadow(s, 1, true);
            }
            const ArtfShadowDesc hard = {1024, 20, false}, soft = {1024, 20, true};
            artfSceneSetShadows(s, &hard);
            drawInto(r, s, a);
            artfSceneSetShadows(s, &soft);
            drawInto(r, s, b);
            printf("%s, soft shadows from %s: %d pixels between light and shadow, %d sharp\n", backend ? "GPU" : "CPU", sun ? "the sun" : "a light", greyCount(b), greyCount(a));
            CHECK(greyCount(b) > greyCount(a) + 100);
            if (sun) memcpy(c[backend], b, W * H * 4);
            /* with no size, sharp as they were */
            if (sun) artfSceneSetSunSize(s, 0);
            else artfLightSetSize(s, 1, 0);
            drawInto(r, s, b);
            artfSceneSetShadows(s, &hard);
            drawInto(r, s, a);
            CHECK(differentShare(a, b, 0) == 0);
            artfSceneFree(s);
        }
        artfRendererFree(r);
    }
    if (c[1][0] || c[1][W * H / 2]) {
        const double apart = differentShare(c[0], c[1], 24);
        printf("soft shadows, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .01);
    }
    artfFree(a);
    artfFree(b);
    artfFree(c[0]);
    artfFree(c[1]);
}

/* Light that bounced, in what a surface reflects: a metal floor, which takes no light of its own, by a red wall turns
 * red once probes are on; both backends alike. */
static void testProbeReflections(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b[2] = {artfMalloc(W * H * 4), artfMalloc(W * H * 4)};
    const int fx = W / 2 + 28, fy = H / 2 + 34;
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        ArtfScene* s = bleedScene();
        ArtfMaterialDesc metal = artfMaterialDefaults();
        metal.baseColor = artfVec3(.9f, .9f, .9f);
        metal.metallic = 1;
        metal.roughness = .35f;
        artfMaterialSet(s, 1, &metal);
        drawInto(r, s, a);
        const ArtfIndirectDesc probes = {{8, 4, 8}, 64, .5f, 1, {-4, -1.5f, -3}, {4, 3, 3}};
        artfSceneSetIndirect(s, &probes);
        for (int k = 0; k < 4; ++k) drawInto(r, s, b[backend]);
        const double before = meanOf(a, fx, fy, 16) - meanOf(a, fx, fy, 8), after = meanOf(b[backend], fx, fy, 16) - meanOf(b[backend], fx, fy, 8);
        printf("%s, light that bounced in a metal floor: red over green %.1f without probes, %.1f with\n", backend ? "GPU" : "CPU", before, after);
        CHECK(after > before + 3);
        artfSceneFree(s);
        artfRendererFree(r);
    }
    if (b[1][0] || b[1][W * H / 2]) {
        const double apart = differentShare(b[0], b[1], 24);
        printf("light that bounced in a metal floor, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .02);
    }
    artfFree(a);
    artfFree(b[0]);
    artfFree(b[1]);
}

/* What is too small to see is not drawn: a far ball goes, a near one stays, on both backends alike. */
static void testCullSize(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b[2] = {artfMalloc(W * H * 4), artfMalloc(W * H * 4)};
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        ArtfScene* s = plainScene();
        const ArtfMesh ball = artfSceneAddSphere(s, .3f, 16, 24);
        addAt(s, ball, whiteMaterial(s), -.6f, 0, 2, 1);   /* near: tens of pixels */
        addAt(s, ball, whiteMaterial(s), 3, 0, -40, 1);    /* far: a few */
        drawInto(r, s, a);
        artfSceneSetCullSize(s, 12);
        drawInto(r, s, b[backend]);
        int lit[2] = {0, 0}, litLeft = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const bool was = CHANNEL(a[y * W + x], 8) > 40, is = CHANNEL(b[backend][y * W + x], 8) > 40;
                lit[0] += was;
                lit[1] += is;
                litLeft += is && x < W / 2;
            }
        printf("%s, too small to draw: %d pixels lit, %d before\n", backend ? "GPU" : "CPU", lit[1], lit[0]);
        CHECK(lit[1] < lit[0] && lit[1] == litLeft && litLeft > 200);
        artfSceneSetCullSize(s, 0);
        drawInto(r, s, b[backend]);
        CHECK(differentShare(a, b[backend], 0) == 0);
        artfSceneSetCullSize(s, 12);
        drawInto(r, s, b[backend]);
        artfSceneFree(s);
        artfRendererFree(r);
    }
    if (b[1][0] || b[1][W * H / 2]) CHECK(differentShare(b[0], b[1], 24) < .002);
    artfFree(a);
    artfFree(b[0]);
    artfFree(b[1]);
}

/* Particles: a burst gives that many; none goes through the ground they bounce on; turbulence moves them, and spin
 * turns them; the same steps give the same places. */
static void particlePlaces(const ArtfScene* s, float* lowest, double* sumX, double* sumTurn) {
    *lowest = 1e30f;
    *sumX = 0;
    *sumTurn = 0;
    for (size_t i = 0; i < s->instances.len; ++i) {
        const ArtfInstanceData* d = &s->instances.items[i];
        if (!(d->flags & ARTF_INSTANCE_PARTICLE) || d->m[0] == 0 && d->m[2] == 0 && d->m[8] == 0) continue;
        *lowest = artfMinf(*lowest, d->m[7]);
        *sumX += d->m[3];
        *sumTurn += fabs(d->m[4]); /* (the quad's x leaning off the eye's right) */
    }
}

static void testParticleForces(void) {
    for (int variant = 0; variant < 4; ++variant) {
        ArtfScene* s = plainScene();
        setCamera(s, 0, 1, 8, 0, 0, 0);
        ArtfEmitterDesc d;
        memset(&d, 0, sizeof d);
        d.velocity = artfVec3(0, 2, 0);
        d.spread = .8f;
        d.gravity = artfVec3(0, -9.8f, 0);
        d.life = 3;
        d.size = .1f;
        d.sizeEnd = .1f;
        d.color = d.colorEnd = artfVec3(1, 1, 1);
        d.opacity = 1;
        d.most = 200;
        d.ground = true;
        d.bounce = .5f;
        d.friction = .2f;
        if (variant == 1) d.turbulence = 6;
        if (variant == 2) d.spin = 3;
        const ArtfEmitter e = artfSceneAddEmitter(s, &d);
        artfEmitterBurst(s, e, 120);
        artfEmitterStep(s, e, 1.f / 60.f);
        CHECK(artfEmitterAlive(s, e) == 120);
        for (int k = 0; k < 120; ++k) artfEmitterStep(s, e, 1.f / 60.f);
        float lowest;
        double sumX, sumTurn;
        particlePlaces(s, &lowest, &sumX, &sumTurn);
        static double plainX, plainTurn;
        if (variant == 0) {
            plainX = sumX;
            plainTurn = sumTurn;
            printf("particles on the ground: the lowest at %.4f\n", lowest);
            CHECK(lowest >= -1e-4f && plainTurn < 1e-3);
        }
        if (variant == 1) CHECK(fabs(sumX - plainX) > 1);
        if (variant == 2) CHECK(sumTurn > 1); /* (what turns draws more numbers: it goes its own ways) */
        if (variant == 3) CHECK(sumX == plainX);
        artfSceneFree(s);
    }
}

/* Layers of a terrain, by rules and by a map, the same on both backends; things scattered over it, where the ground is,
 * as many as asked, the same each time, and not where it is steep. */
static void testTerrainLayers(void) {
    static float heights[65 * 65];
    for (int z = 0; z < 65; ++z)
        for (int x = 0; x < 65; ++x) {
            const float dx = (float)(x - 32) / 32.f, dz = (float)(z - 32) / 32.f, r2 = dx * dx + dz * dz;
            heights[z * 65 + x] = r2 < 1 ? (1.f - r2) * (1.f - r2) : 0.f;
        }
    uint32_t* a[2] = {artfMalloc(W * H * 4), artfMalloc(W * H * 4)};
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        ArtfScene* s = plainScene();
        artfSceneSetSun(s, artfVec3(-.3f, -1, -.4f), artfVec3(1, 1, 1));
        artfSceneSetAmbient(s, artfVec3(.4f, .4f, .4f), artfVec3(.2f, .2f, .2f));
        setCamera(s, 0, 4, 9, 0, .8f, 0);
        const ArtfMesh hill = artfSceneAddTerrain(s, heights, 65, 65, .15f, 3.f, 1.f);
        ArtfTerrainLayersDesc layers;
        memset(&layers, 0, sizeof layers);
        layers.count = 3;
        layers.layers[0].color = artfVec3(.1f, .5f, .1f);  /* grass */
        layers.layers[1].color = artfVec3(.4f, .4f, .4f);  /* rock, where it is steep */
        layers.layers[2].color = artfVec3(1, 1, 1);        /* snow, high */
        layers.steep = .75f;
        layers.high = 2.4f;
        const ArtfMaterial ground = artfSceneAddTerrainMaterial(s, &layers);
        CHECK(ground != 0);
        artfSceneAddInstance(s, hill, ground, NULL);
        drawInto(r, s, a[backend]);
        /* the top is snow, the foot grass: green over red there, and none at the top */
        uint32_t top = 0, foot = 0;
        int topY = H, footY = 0;
        for (int y = 0; y < H; ++y) {
            const uint32_t p = a[backend][y * W + W / 2];
            if (CHANNEL(p, 8) > 20 && y < topY) { topY = y; top = a[backend][(y + 3) * W + W / 2]; }
        }
        /* (the foot: up from the bottom, past the terrain's near edge) */
        for (footY = H - 1; footY > 0 && CHANNEL(a[backend][footY * W + W / 2], 8) < 20; --footY) {}
        footY -= 4;
        foot = a[backend][footY * W + W / 2];
        printf("%s, terrain layers: at the top %d %d %d, at the foot %d %d %d\n", backend ? "GPU" : "CPU", CHANNEL(top, 16), CHANNEL(top, 8), CHANNEL(top, 0), CHANNEL(foot, 16), CHANNEL(foot, 8), CHANNEL(foot, 0));
        CHECK(CHANNEL(foot, 8) > CHANNEL(foot, 16) + 30 && abs(CHANNEL(top, 8) - CHANNEL(top, 16)) < 20 && CHANNEL(top, 16) > CHANNEL(foot, 16) + 60);
        /* by a map: red on the left half, green on the right */
        const uint32_t map[8] = {0x00FF0000u, 0x00FF0000u, 0x00FF0000u, 0x00FF0000u, 0x0000FF00u, 0x0000FF00u, 0x0000FF00u, 0x0000FF00u};
        layers.weights = artfSceneAddTexture(s, map, 8, 1, 0);
        layers.count = 2;
        const ArtfMaterial painted = artfSceneAddTerrainMaterial(s, &layers);
        artfInstanceSetMaterial(s, 1, painted);
        uint32_t* b = artfMalloc(W * H * 4);
        drawInto(r, s, b);
        const uint32_t left = b[footY * W + W / 2 - W / 6], right = b[footY * W + W / 2 + W / 6];
        printf("%s, terrain painted: on the left %d %d %d, on the right %d %d %d\n", backend ? "GPU" : "CPU", CHANNEL(left, 16), CHANNEL(left, 8), CHANNEL(left, 0), CHANNEL(right, 16), CHANNEL(right, 8), CHANNEL(right, 0));
        CHECK(CHANNEL(left, 8) > CHANNEL(left, 16) + 30 && abs(CHANNEL(right, 8) - CHANNEL(right, 16)) < 12);
        artfFree(b);
        artfSceneFree(s);
        artfRendererFree(r);
    }
    if (a[1][0] || a[1][W * H / 2]) {
        const double apart = differentShare(a[0], a[1], 24);
        printf("terrain layers, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .01);
    }
    artfFree(a[0]);
    artfFree(a[1]);
    /* scattered: as many as the area asks, on the ground, the same each time */
    ArtfScene* s = artfSceneNew();
    const ArtfMesh rock = artfSceneAddSphere(s, .1f, 8, 12);
    ArtfScatterDesc d;
    memset(&d, 0, sizeof d);
    d.heights = heights;
    d.width = d.depth = 65;
    d.cell = .15f;
    d.height = 3.f;
    d.mesh = rock;
    d.material = 0;
    d.density = 10;
    d.seed = 7;
    ArtfInstance placed[2048];
    const uint32_t n = artfSceneScatter(s, &d, placed, 2048);
    const float area = 64 * .15f * 64 * .15f;
    CHECK(n == (uint32_t)(area * 10));
    float worst = 0;
    for (uint32_t i = 0; i < artfMini((int)n, 2048); ++i) {
        const ArtfInstanceData* inst = &s->instances.items[i];
        const float fx = (inst->m[3] + 32 * .15f) / .15f, fz = (inst->m[11] + 32 * .15f) / .15f;
        const int x = artfMini((int)fx, 63), z = artfMini((int)fz, 63);
        const float tx = fx - (float)x, tz = fz - (float)z;
        const float h0 = heights[z * 65 + x] + (heights[z * 65 + x + 1] - heights[z * 65 + x]) * tx;
        const float h1 = heights[(z + 1) * 65 + x] + (heights[(z + 1) * 65 + x + 1] - heights[(z + 1) * 65 + x]) * tx;
        worst = artfMaxf(worst, fabsf(inst->m[7] - (h0 + (h1 - h0) * tz) * 3.f));
    }
    CHECK(worst < 1e-4f);
    ArtfScene* again = artfSceneNew();
    artfSceneAddSphere(again, .1f, 8, 12);
    CHECK(artfSceneScatter(again, &d, NULL, 0) == n && memcmp(again->instances.items, s->instances.items, n * sizeof(ArtfInstanceData)) == 0);
    d.steepest = .9f;
    const uint32_t flatOnly = artfSceneScatter(again, &d, NULL, 0);
    printf("scattered over a terrain: %u, %u where it is flat enough\n", n, flatOnly);
    CHECK(flatOnly < n && flatOnly > n / 4);
    artfSceneFree(again);
    artfSceneFree(s);
}

/* Rays through the ray tracing hardware, where the GPU has some: the same picture as through the trees, rays and paths,
 * with what has one side and two, and after something moves. */
static void sawHardware(void* user, const char* message) {
    if (strstr(message, "ray tracing hardware")) *(bool*)user = true;
}

static void testTraceHardware(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    bool hardware = false;
    artfSetLog(sawHardware, &hardware);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (!gpu) {
        artfSetLog(NULL, NULL);
        return;
    }
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    for (int mode = 0; mode < 2; ++mode) {
        ArtfScene* s = bleedScene();
        ArtfMaterialDesc twoSided = artfMaterialDefaults();
        twoSided.baseColor = artfVec3(.2f, .4f, .9f);
        twoSided.doubleSided = true;
        const ArtfTransform card = artfTransformTrs(artfVec3(-2.4f, .2f, -.5f), artfQuatAxisAngle(artfVec3(0, 1, 0), 2.6f), artfVec3(1, 1, 1));
        const ArtfInstance back = artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &twoSided), &card);
        ArtfTraceDesc d = {mode ? ARTF_TRACE_PATHS : ARTF_TRACE_RAYS, 4, 64, 0, false, false};
        for (int moved = 0; moved < 2; ++moved) {
            if (moved) {
                const ArtfTransform there = artfTransformTrs(artfVec3(-2.f, .6f, -.2f), artfQuatAxisAngle(artfVec3(0, 1, 0), 2.2f), artfVec3(1, 1, 1));
                artfInstanceSetTransform(s, back, &there);
            }
            d.hardware = false;
            artfSceneSetTrace(s, &d);
            for (int k = 0; k < 16; ++k) drawInto(gpu, s, a);
            d.hardware = true;
            artfSceneSetTrace(s, &d);
            for (int k = 0; k < 16; ++k) drawInto(gpu, s, b);
            const double apart = differentShare(a, b, 24);
            printf("%s%s through the ray tracing hardware against the trees: %.2f%% of pixels apart\n", mode ? "paths" : "rays", moved ? ", moved," : "", apart * 100);
            CHECK(apart < .01);
        }
        artfSceneFree(s);
    }
    artfSetLog(NULL, NULL);
    printf(hardware ? "(rays went through the ray tracing hardware)\n" : "(no ray tracing hardware here: the trees were held to themselves)\n");
    artfFree(a);
    artfFree(b);
    artfRendererFree(gpu);
}

/* The sky, as Unreal's: light scattered more than once makes twilight and the horizon brighter; the sun drawn is bright
 * where it is and nowhere else, and gives no light of its own; clouds are there, the same bytes each time, and the
 * wind moves them; their shadow, when asked, dims the sun. */
static float skyMean(const ArtfScene* s, float y, int channel) {
    float sum = 0;
    for (int k = 0; k < 64; ++k) {
        const float a = (float)k * 6.2831853f / 64.f, across = sqrtf(1.f - y * y);
        float c[3];
        artfEnvSample(s->environment, 0, across * artfSinFast(a), y, across * artfCosFast(a), c);
        sum += c[channel];
    }
    return sum / 64.f;
}

static void testSkyMore(void) {
    ArtfScene* s = artfSceneNew();
    ArtfSkyDesc d;
    memset(&d, 0, sizeof d);
    d.width = 128;
    /* twilight: the sun under the horizon */
    artfSceneSetSun(s, artfVec3(0, .05f, 1), artfVec3(3, 3, 3));
    CHECK(artfSceneSetSky(s, &d));
    const float once = skyMean(s, .3f, 2), onceLow = skyMean(s, .05f, 2);
    d.multiple = 1;
    double start = artfNowMs();
    CHECK(artfSceneSetSky(s, &d));
    const double took = artfNowMs() - start;
    const float many = skyMean(s, .3f, 2), manyLow = skyMean(s, .05f, 2);
    printf("twilight's blue, scattered once %.5f (at the horizon %.5f), more than once %.5f (%.5f); made in %.1f ms\n", once, onceLow, many, manyLow, took);
    CHECK(many > once * 1.3f && manyLow > onceLow * 1.2f);
    /* by day too, a little */
    artfSceneSetSun(s, artfVec3(-.3f, -.6f, -1), artfVec3(3, 3, 3));
    d.multiple = 0;
    CHECK(artfSceneSetSky(s, &d));
    const float day = skyMean(s, .1f, 1);
    const ArtfVec3 sunOnce = s->sunColor;
    d.multiple = 1;
    CHECK(artfSceneSetSky(s, &d));
    printf("the horizon by day, green: %.4f once, %.4f more than once\n", day, skyMean(s, .1f, 1));
    CHECK(skyMean(s, .1f, 1) > day * 1.05f);
    /* the sun's color, from the table, is what the air leaves of it (by a march of 20000 steps: 2.664, 2.275, 1.770);
     * the 8 steps of light scattered once miss some of the haze near the ground */
    printf("the sun through the air: %.3f %.3f %.3f from the table, %.3f %.3f %.3f in 8 steps\n", s->sunColor.x, s->sunColor.y, s->sunColor.z, sunOnce.x, sunOnce.y, sunOnce.z);
    CHECK(fabsf(s->sunColor.x - 2.664f) < .01f && fabsf(s->sunColor.y - 2.275f) < .01f && fabsf(s->sunColor.z - 1.770f) < .01f);
    /* the sun drawn: bright toward it, nowhere else, and the sun's light the same */
    const ArtfVec3 sunPlain = s->sunColor;
    float toward[3], beside[3], besidePlain[3];
    const ArtfVec3 to = artfVec3Normalize(artfVec3(.3f, .6f, 1));
    const ArtfVec3 off = artfVec3Normalize(artfVec3(.5f, .6f, 1));
    artfEnvSample(s->environment, 0, off.x, off.y, off.z, besidePlain);
    d.sunDisc = 4;
    CHECK(artfSceneSetSky(s, &d));
    artfEnvSample(s->environment, 0, to.x, to.y, to.z, toward);
    artfEnvSample(s->environment, 0, off.x, off.y, off.z, beside);
    printf("the sun drawn: %.2f toward it, %.4f beside it (%.4f without)\n", toward[0], beside[0], besidePlain[0]);
    CHECK(toward[0] > 8 && fabsf(beside[0] - besidePlain[0]) < .01f * besidePlain[0] + 1e-4f);
    CHECK(s->sunColor.x == sunPlain.x && s->sunColor.y == sunPlain.y && s->sunColor.z == sunPlain.z);
    /* clouds: white and grey where they are, the same bytes made again; the wind moves them */
    d.sunDisc = 0;
    CHECK(artfSceneSetSky(s, &d));
    const size_t size = ARTF_ENV_FLOATS * sizeof(float);
    float* clear = artfMalloc(size);
    float* first = artfMalloc(size);
    memcpy(clear, s->environment, size);
    d.clouds.coverage = .6f;
    start = artfNowMs();
    CHECK(artfSceneSetSky(s, &d));
    printf("a sky with clouds made in %.1f ms\n", artfNowMs() - start);
    memcpy(first, s->environment, size);
    int cloudy = 0, looked = 0;
    for (int k = 0; k < 256; ++k) {
        const float a = (float)k * .37f, y = .15f + .8f * (float)(k % 16) / 16.f, across = sqrtf(1.f - y * y);
        float c[3], was[3];
        artfEnvSample(first, 0, across * artfSinFast(a), y, across * artfCosFast(a), c);
        artfEnvSample(clear, 0, across * artfSinFast(a), y, across * artfCosFast(a), was);
        /* (a cloud: less blue than red and green than the sky was, or brighter) */
        cloudy += c[2] / (c[0] + 1e-6f) < was[2] / (was[0] + 1e-6f) * .8f;
        ++looked;
    }
    printf("clouds over %d of %d places in the sky\n", cloudy, looked);
    CHECK(cloudy > looked / 8 && cloudy < looked);
    CHECK(artfSceneSetSky(s, &d));
    CHECK(memcmp(first, s->environment, size) == 0);
    d.clouds.wind = artfVec3(3000, 0, 0);
    CHECK(artfSceneSetSky(s, &d));
    CHECK(memcmp(first, s->environment, size) != 0);
    /* their shadow: the sun under thick cloud is dimmer; without it, as it was */
    const ArtfVec3 unshaded = s->sunColor;
    d.clouds.coverage = 1;
    d.clouds.density = 4;
    d.clouds.shadow = true;
    CHECK(artfSceneSetSky(s, &d));
    printf("the sun under clouds: %.3f, %.3f without their shadow\n", s->sunColor.y, unshaded.y);
    CHECK(s->sunColor.y < unshaded.y * .5f && unshaded.y == sunPlain.y);
    artfFree(clear);
    artfFree(first);
    artfSceneFree(s);
}

/* The look, as Unreal's: a histogram leaves out a small bright light that the mean follows; the exposure falls at its
 * own speed; a look-up table that changes nothing changes nothing, and one that swaps red and blue swaps them; lift,
 * gamma, gain, the wheels, grain and flare each change the picture; grain is new each frame; .cube files are read; the
 * backends agree; and none of it asked is the same bytes as before. */
static ArtfScene* lampScene(bool lamp) {
    ArtfScene* s = bleedScene();
    artfSceneSetSun(s, artfVec3(-.6f, -1, -.3f), artfVec3(2, 2, 2));
    artfSceneSetAmbient(s, artfVec3(.1f, .1f, .1f), artfVec3(.1f, .1f, .1f));
    artfSceneSetExposure(s, 1);
    if (lamp) {
        ArtfMaterialDesc m = artfMaterialDefaults();
        m.baseColor = artfVec3(0, 0, 0);
        m.emissive = artfVec3(3000, 3000, 3000);
        addAt(s, artfSceneAddSphere(s, 1.f, 16, 24), artfSceneAddMaterial(s, &m), 0, 1.2f, 0, 1);
    }
    return s;
}

/* The mean of the left quarter of a frame: away from the lamp. */
static double leftMean(const uint32_t* f) {
    double sum = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W / 4; ++x) sum += CHANNEL(f[y * W + x], 8);
    return sum / (H * (W / 4));
}

static float* swapLut(int n, bool swap) {
    float* lut = artfMalloc((size_t)n * n * n * 12);
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                float* o = lut + 3 * (((size_t)b * n + g) * n + r);
                o[0] = (float)(swap ? b : r) / (float)(n - 1);
                o[1] = (float)g / (float)(n - 1);
                o[2] = (float)(swap ? r : b) / (float)(n - 1);
            }
    return lut;
}

static void testLookMore(void) {
    /* .cube files */
    static const char cube[] = "# a table\nTITLE \"swap\"\nLUT_3D_SIZE 2\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 1 1 1\n"
                               "0 0 0\n0 0 1\n0 1 0\n0 1 1\n1 0 0\n1 0 1\n1 1 0\n1 1 1\n";
    int side = 0;
    float* read = artfLutDecodeCube(cube, sizeof cube - 1, &side);
    float* known = swapLut(2, true);
    CHECK(read && side == 2 && !memcmp(read, known, 8 * 12));
    artfFree(read);
    artfFree(known);
    CHECK(!artfLutDecodeCube("LUT_3D_SIZE 2\n0 0 0\n", 20, &side));
    CHECK(!artfLutDecodeCube("LUT_1D_SIZE 2\n0 0 0\n1 1 1\n", 26, &side));

    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* c = artfMalloc(W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.hdr = true;
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        const char* name = backend ? "GPU" : "CPU";
        /* a histogram: a lamp in the middle moves the mean, not what is left between the shares */
        ArtfScene* dark = lampScene(false);
        ArtfScene* lit = lampScene(true);
        const ArtfPostDesc mean = {.autoExposure = true, .exposureSpeed = 1};
        ArtfPostDesc histogram = mean;
        histogram.exposureLow = .1f;
        histogram.exposureHigh = .1f;
        artfSceneSetPost(dark, &mean);
        artfSceneSetPost(lit, &mean);
        drawInto(r, dark, a);
        drawInto(r, lit, b);
        const double byMean = leftMean(a) - leftMean(b);
        artfSceneSetPost(dark, &histogram);
        artfSceneSetPost(lit, &histogram);
        drawInto(r, dark, a);
        drawInto(r, lit, b);
        const double byHistogram = fabs(leftMean(a) - leftMean(b));
        printf("%s, a bright lamp darkens the rest by %.1f metered by the mean, %.1f by a histogram\n", name, byMean, byHistogram);
        CHECK(byMean > 2 && byHistogram < byMean * .3);
        /* down at its own speed: from dark to bright at once, back up slowly */
        ArtfPostDesc speeds = histogram;
        speeds.exposureSpeed = .05f;
        speeds.exposureSpeedDown = 1;
        artfSceneSetPost(dark, &speeds);
        artfSceneSetSun(dark, artfVec3(-.6f, -1, -.3f), artfVec3(.2f, .2f, .2f));
        drawInto(r, dark, a);
        for (int k = 0; k < 20; ++k) drawInto(r, dark, a);
        artfSceneSetSun(dark, artfVec3(-.6f, -1, -.3f), artfVec3(2, 2, 2));
        CHECK(artfRendererDraw(r, dark));
        CHECK(artfRendererReadPixels(r, b));
        artfSceneSetPost(dark, &histogram);
        drawInto(r, dark, c);
        printf("%s, the exposure falls at once: %.1f, as metered %.1f\n", name, leftMean(b), leftMean(c));
        CHECK(fabs(leftMean(b) - leftMean(c)) < 3);
        artfSceneFree(dark);
        artfSceneFree(lit);
        /* the look-up tables */
        ArtfScene* s = lampScene(false);
        artfSceneSetPost(s, NULL);
        drawInto(r, s, a);
        float* identity = swapLut(17, false);
        float* swapped = swapLut(17, true);
        ArtfPostDesc look = {.lut = identity, .lutSize = 17};
        artfSceneSetPost(s, &look);
        drawInto(r, s, b);
        printf("%s, a look-up table that changes nothing: %.4f%% of pixels apart\n", name, differentShare(a, b, 1) * 100);
        CHECK(differentShare(a, b, 1) == 0);
        look.lut = swapped;
        artfSceneSetPost(s, &look);
        drawInto(r, s, b);
        int worst = 0;
        for (int i = 0; i < W * H; ++i) {
            worst = artfMaxi(worst, abs(CHANNEL(a[i], 16) - CHANNEL(b[i], 0)));
            worst = artfMaxi(worst, abs(CHANNEL(a[i], 0) - CHANNEL(b[i], 16)));
            worst = artfMaxi(worst, abs(CHANNEL(a[i], 8) - CHANNEL(b[i], 8)));
        }
        printf("%s, a look-up table that swaps red and blue: within %d\n", name, worst);
        CHECK(worst <= 1);
        artfFree(identity);
        artfFree(swapped);
        /* each of the rest changes the picture, the same way each time (grain: each frame) */
        const ArtfPostDesc each[8] = {{.lift = {.1f, 0, 0}}, {.gamma = {1.5f, 1.5f, 1.5f}}, {.gain = {1, .7f, 1}}, {.shadows = {1, 1, 1.6f}},
                                      {.midtones = {1.3f, 1, 1}}, {.highlights = {.6f, .8f, 1}}, {.bloom = .1f, .flare = 1}, {.grain = .5f}};
        for (int e = 0; e < 8; ++e) {
            ArtfPostDesc plain = {0};
            plain.bloom = each[e].bloom;
            artfSceneSetPost(s, &plain);
            drawInto(r, s, a);
            artfSceneSetPost(s, &each[e]);
            drawInto(r, s, b);
            const double changed = differentShare(a, b, 2);
            if (changed <= .01) printf("%s, look %d changed %.4f of the pixels\n", name, e, changed);
            CHECK(changed > .01);
            drawInto(r, s, c);
            CHECK((differentShare(b, c, 0) == 0) == (e != 7));
        }
        /* none of it: the same bytes */
        artfSceneSetPost(s, NULL);
        drawInto(r, s, a);
        const ArtfPostDesc none = {.lutAmount = .5f, .lutSize = 17};
        artfSceneSetPost(s, &none);
        drawInto(r, s, b);
        CHECK(differentShare(a, b, 0) == 0);
        artfSceneFree(s);
        artfRendererFree(r);
    }
    /* the backends agree, all of it at once */
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    o.hdr = true;
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        o.backend = ARTF_BACKEND_SOFTWARE;
        ArtfRenderer* cpu = artfRendererNew(&o);
        float* swapped = swapLut(9, true);
        ArtfScene* s = lampScene(true);
        const ArtfPostDesc all = {.bloom = .1f, .autoExposure = true, .exposureSpeed = .5f, .exposureSpeedDown = 1, .exposureLow = .1f, .exposureHigh = .05f,
                                  .lift = {.03f, 0, .02f}, .gamma = {1.2f, 1.1f, 1}, .gain = {1, .95f, .9f}, .shadows = {.9f, 1, 1.2f}, .highlights = {1.1f, 1, .9f},
                                  .lut = swapped, .lutSize = 9, .lutAmount = .7f, .grain = .3f, .flare = .5f};
        artfSceneSetPost(s, &all);
        drawInto(cpu, s, a);
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 16);
        printf("the look's histogram, grading, table, grain and flare, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .02);
        artfSceneFree(s);
        artfFree(swapped);
        artfRendererFree(cpu);
        artfRendererFree(gpu);
    }
    artfFree(a);
    artfFree(b);
    artfFree(c);
}

/* ---- views and what passes take ---- */

/* What a view shows of a count (artfViewHeat), as a frame holds it: with floats, its square root. */
static void heatBytes(uint32_t count, bool floats, int* out) {
    float c[3];
    artfViewHeat(count, c);
    for (int k = 0; k < 3; ++k) out[k] = (int)((floats ? sqrtf(c[k]) : c[k]) * 255.f + .5f);
}

static bool rgbNear(uint32_t p, const int* rgb, int margin) {
    return near_(CHANNEL(p, 16), rgb[0], margin) && near_(CHANNEL(p, 8), rgb[1], margin) && near_(CHANNEL(p, 0), rgb[2], margin);
}

/* Lighter levels, a layer, lights past the frame's sixteen and something that moves: what the views tell apart. */
static ArtfScene* viewScene(ArtfInstance* moving) {
    ArtfScene* s = artfSceneNew();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true);
    ArtfMaterialDesc floorDesc = {artfVec3(.6f, .6f, .6f), 0, .2f, .5f, artfVec3(0, 0, 0)};
    ArtfTransform t = artfTransformAt(0, -1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 40, 8), artfSceneAddMaterial(s, &floorDesc), &t);
    ArtfMaterialDesc red = {artfVec3(.8f, .15f, .1f), 0, .7f, .3f, artfVec3(0, 0, 0)};
    red.metallic = .6f;
    ArtfMaterialDesc glow = {artfVec3(.1f, .3f, .2f), 0, 0, 1, artfVec3(.05f, .4f, .1f)};
    const ArtfMaterial redMaterial = artfSceneAddMaterial(s, &red), glowMaterial = artfSceneAddMaterial(s, &glow);
    const ArtfMesh sphere = artfSceneAddSphere(s, 1, 48, 64);
    for (int i = 0; i < 5; ++i) addAt(s, sphere, i & 1 ? glowMaterial : redMaterial, -3.f + (float)i * 1.6f, 0, -(float)i * 6.f, 1);
    *moving = addAt(s, artfSceneAddTorus(s, .7f, .25f, 48, 24), glowMaterial, 1.5f, .3f, 2.f, 1);
    artfInstanceSetOpacity(s, addAt(s, artfSceneAddCube(s, 1.2f), redMaterial, -1.2f, 0, 2.2f, 1), .5f);
    for (int i = 0; i < 24; ++i) artfSceneAddLight(s, artfVec3(-6.f + (float)i * .5f, -.5f, 1.f - (float)(i % 5) * 3.f), artfVec3(.4f, .3f, .2f), 2.5f);
    setCamera(s, 0, 1.5f, 7, 0, 0, -2);
    return s;
}

/* What the kernels do after the frame (grading, temporal antialiasing, upscaling, with something that moves): the same
 * bytes on every instruction set, with any number of threads. */
static void testPostKernels(void) {
    uint32_t* frame = artfMalloc(W * H * 4);
    float* swapped = swapLut(9, true);
    static const struct { int threads; ArtfSimd simd; } runs[] = {{1, ARTF_SIMD_AUTO}, {4, ARTF_SIMD_AUTO}, {1, ARTF_SIMD_PORTABLE}, {3, ARTF_SIMD_128}};
    for (int scaled = 0; scaled < 2; ++scaled) {
        uint64_t reference = 0;
        for (size_t i = 0; i < ARTF_COUNT(runs); ++i) {
            ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, runs[i].threads, false);
            o.simd = runs[i].simd;
            o.hdr = true;
            o.renderScale = scaled ? .6f : 0;
            ArtfRenderer* r = artfRendererNew(&o);
            ArtfInstance moving;
            ArtfScene* s = viewScene(&moving);
            const ArtfPostDesc all = {.bloom = .1f, .temporal = true, .lift = {.03f, 0, .02f}, .gamma = {1.2f, 1.1f, .9f}, .gain = {1, .95f, .9f},
                                      .shadows = {.9f, 1, 1.2f}, .highlights = {1.1f, 1, .9f}, .lut = swapped, .lutSize = 9, .lutAmount = .7f, .grain = .3f, .flare = .5f};
            artfSceneSetPost(s, &all);
            for (int f = 0; f < 4; ++f) {
                const ArtfTransform t = artfTransformAt(1.5f + .1f * (float)f, .3f, 2.f, 1);
                artfInstanceSetTransform(s, moving, &t);
                CHECK(artfRendererDraw(r, s));
            }
            CHECK(artfRendererReadPixels(r, frame));
            const uint64_t h = hashOf(frame, (size_t)W * H);
            if (!i) reference = h;
            CHECK(h == reference);
            artfSceneFree(s);
            artfRendererFree(r);
        }
    }
    artfFree(swapped);
    artfFree(frame);
}

/* Each view (ArtfView) drawn: what was asked for comes out, on each backend and with a frame in floats; the two
 * backends agree; and a view put back leaves the picture as it was, to the byte. */
/* Vulkan's pixel shaders of another variant, made off the frame: a view asked for after the first frame comes a few
 * frames later, and after artfRendererWarm the frame is the one that waiting for them makes. */
static void testShadersAhead(void) {
    ArtfRenderer* r = artfRendererNew(&(ArtfRendererOptions){.backend = ARTF_BACKEND_VULKAN, .width = W, .height = H});
    if (!r) return;
    ArtfScene* s = plainScene();
    ArtfMaterialDesc d = {artfVec3(.36f, .64f, .04f), 0, 0, .6f, artfVec3(.36f, 0, 0)};
    addAt(s, addSquare(s, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
    artfSceneAddLight(s, artfVec3(0, 0, 1), artfVec3(1, 1, 1), 10.f);
    uint32_t* waited = artfMalloc(W * H * 4);
    uint32_t* ahead = artfMalloc(W * H * 4);
    drawInto(r, s, waited);
    artfRendererSetView(r, ARTF_VIEW_NORMALS);
    drawInto(r, s, waited); /* (artfVulkanShadersWait: made before the frame) */
    artfRendererSetView(r, ARTF_VIEW_LIT);
    drawInto(r, s, ahead);
    artfVulkanShadersWait = false;
    artfRendererSetView(r, ARTF_VIEW_NORMALS);
    double longest = 0;
    for (int i = 0; i < 3; ++i) {
        const double t0 = artfNowMs();
        CHECK(artfRendererDraw(r, s));
        const double took = artfNowMs() - t0;
        if (took > longest) longest = took;
    }
    artfRendererWarm(r);
    drawInto(r, s, ahead);
    printf("pixel shaders made off the frame: the longest frame after asking for a view %.2f ms\n", longest);
    CHECK(!memcmp(waited, ahead, W * H * 4));
    artfVulkanShadersWait = true;
    artfFree(waited);
    artfFree(ahead);
    artfSceneFree(s);
    artfRendererFree(r);
}

static void testViews(void) {
    uint32_t* frame = artfMalloc(W * H * 4);
    uint32_t* frames[2][2][ARTF_VIEW_COUNT];
    memset(frames, 0, sizeof frames);
    CHECK(!strcmp(artfViewName(ARTF_VIEW_BASE_COLOR), "base-color") && !artfViewName(ARTF_VIEW_COUNT));
    ArtfView named = ARTF_VIEW_LIT;
    CHECK(artfViewFromName("overdraw", &named) && named == ARTF_VIEW_OVERDRAW && !artfViewFromName("lit up", &named));
    for (int backend = 0; backend < 2; ++backend)
        for (int floats = 0; floats < 2; ++floats) {
            ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
            o.hdr = floats;
            ArtfRenderer* r = artfRendererNew(&o);
            if (!r) continue;
            /* a square facing the eye at 5, with values that are whole bytes as they show */
            ArtfScene* s = plainScene();
            ArtfMaterialDesc d = {artfVec3(.36f, .64f, .04f), 0, 0, .6f, artfVec3(.36f, 0, 0)};
            d.metallic = .2f;
            const ArtfInstance square = addAt(s, addSquare(s, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu), artfSceneAddMaterial(s, &d), 0, 0, 0, 1);
            for (int i = 0; i < 3; ++i) artfSceneAddLight(s, artfVec3((float)i - 1.f, 0, 1), artfVec3(1, 1, 1), 10.f);
            artfSceneAddLight(s, artfVec3(0, 0, -30), artfVec3(1, 1, 1), 5.f); /* (out of reach) */
            float c[3];
            int colors[ARTF_VIEW_COUNT][3];
            const int base[3] = {153, 204, 51};
            memcpy(colors[ARTF_VIEW_BASE_COLOR], base, sizeof base);
            memcpy(colors[ARTF_VIEW_AMBIENT], base, sizeof base); /* (a sky and ground of 1: what all around gives is the color) */
            const int normal[3] = {128, 128, 255}, rough[3] = {153, 153, 153}, metal[3] = {51, 51, 51}, own[3] = {153, 0, 0}, white[3] = {255, 255, 255}, depth[3] = {170, 170, 170}, still[3] = {128, 128, 128};
            memcpy(colors[ARTF_VIEW_NORMALS], normal, sizeof normal);
            memcpy(colors[ARTF_VIEW_ROUGHNESS], rough, sizeof rough);
            memcpy(colors[ARTF_VIEW_METALLIC], metal, sizeof metal);
            memcpy(colors[ARTF_VIEW_EMISSIVE], own, sizeof own);
            memcpy(colors[ARTF_VIEW_LOD], white, sizeof white);
            memcpy(colors[ARTF_VIEW_DEPTH], depth, sizeof depth); /* (10 / (10 + 5)) */
            memcpy(colors[ARTF_VIEW_MOTION], still, sizeof still);
            artfViewColor(0, c);
            for (int k = 0; k < 3; ++k) colors[ARTF_VIEW_INSTANCES][k] = colors[ARTF_VIEW_MESHLETS][k] = (int)(c[k] * 255.f + .5f);
            heatBytes(1, floats, colors[ARTF_VIEW_OVERDRAW]);
            artfViewHeat(3, c);
            for (int k = 0; k < 3; ++k) colors[ARTF_VIEW_LIGHTS][k] = (int)(c[k] * 255.f + .5f);
            const int black[3] = {0, 0, 0};
            for (int v = ARTF_VIEW_BASE_COLOR; v < ARTF_VIEW_COUNT; ++v) {
                artfRendererSetView(r, (ArtfView)v);
                CHECK(artfRendererView(r) == (ArtfView)v);
                drawInto(r, s, frame);
                const uint32_t at = frame[(H / 2) * W + W / 2 + 12], behind = frame[2 * W + 2];
                const bool good = rgbNear(at, colors[v], 1) && rgbNear(behind, black, 0);
                if (!good) printf("%s%s, view %s: %d %d %d, behind %06x\n", backend ? "GPU" : "CPU", floats ? " in floats" : "", artfViewName((ArtfView)v), CHANNEL(at, 16), CHANNEL(at, 8), CHANNEL(at, 0), behind & 0xFFFFFF);
                CHECK(good);
            }
            /* motion: the square goes right, so red is past grey; up and down, nothing */
            artfRendererSetView(r, ARTF_VIEW_MOTION);
            const ArtfTransform right = artfTransformAt(.1f, 0, 0, 1);
            artfInstanceSetTransform(s, square, &right);
            drawOnce(r, s, frame);
            const uint32_t went = frame[(H / 2) * W + W / 2 + 12];
            printf("%s%s, a square going right 3.7 pixels: red %d, green %d\n", backend ? "GPU" : "CPU", floats ? " in floats" : "", CHANNEL(went, 16), CHANNEL(went, 8));
            CHECK(CHANNEL(went, 16) > 145 && CHANNEL(went, 16) < 170 && near_(CHANNEL(went, 8), 128, 1));
            artfSceneFree(s);

            /* a fuller scene, each view, for the two backends to agree on */
            ArtfInstance moving;
            s = viewScene(&moving);
            for (int v = 0; v < ARTF_VIEW_COUNT; ++v) {
                artfRendererSetView(r, (ArtfView)v);
                drawInto(r, s, frame);
                const ArtfTransform t = artfTransformAt(1.6f, .3f, 2.f, 1);
                if (v == ARTF_VIEW_MOTION) {
                    artfInstanceSetTransform(s, moving, &t);
                    drawOnce(r, s, frame);
                }
                frames[backend][floats][v] = artfMalloc(W * H * 4);
                memcpy(frames[backend][floats][v], frame, W * H * 4);
            }
            /* back to the picture: what it was, to the byte, as if no view had been */
            artfRendererSetView(r, ARTF_VIEW_LIT);
            drawInto(r, s, frame);
            artfSceneFree(s);
            artfRendererFree(r);
            r = artfRendererNew(&o);
            s = viewScene(&moving);
            const ArtfTransform t = artfTransformAt(1.6f, .3f, 2.f, 1);
            artfInstanceSetTransform(s, moving, &t);
            uint32_t* fresh = artfMalloc(W * H * 4);
            drawInto(r, s, fresh);
            CHECK(!memcmp(frame, fresh, W * H * 4));
            artfFree(fresh);
            artfSceneFree(s);
            artfRendererFree(r);
        }
    for (int floats = 0; floats < 2; ++floats) {
        if (!frames[1][floats][0]) break;
        for (int v = 1; v < ARTF_VIEW_COUNT; ++v) {
            const double apart = differentShare(frames[0][floats][v], frames[1][floats][v], 24);
            if (apart > .002) printf("the view %s%s, the GPU against the CPU: %.3f%% of pixels apart\n", artfViewName((ArtfView)v), floats ? " in floats" : "", apart * 100);
            CHECK(apart < .01);
        }
        /* (and not all the same picture; the spheres far away are lighter levels) */
        CHECK(differentShare(frames[0][floats][ARTF_VIEW_INSTANCES], frames[0][floats][ARTF_VIEW_MESHLETS], 24) > .01);
        int lighter = 0;
        for (int i = 0; i < W * H; ++i) {
            const uint32_t p = frames[0][floats][ARTF_VIEW_LOD][i];
            lighter += CHANNEL(p, 16) == 64 && CHANNEL(p, 0) == 64 && CHANNEL(p, 8) == 255;
        }
        CHECK(lighter > 0);
    }
    for (int i = 0; i < 2 * 2 * ARTF_VIEW_COUNT; ++i) artfFree((&frames[0][0][0])[i]);
    artfFree(frame);
}

/* What each pass took: listed, in order, adding up to the frame. */
static void testPassTimes(void) {
    uint32_t* frame = artfMalloc(W * H * 4);
    for (int backend = 0; backend < 2; ++backend)
        for (int floats = 0; floats < 2; ++floats) {
            ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
            o.hdr = floats;
            ArtfRenderer* r = artfRendererNew(&o);
            if (!r) continue;
            ArtfScene* s = richScene();
            const ArtfShadowDesc shadows = {512, 0, false};
            artfSceneSetShadows(s, &shadows);
            const ArtfPostDesc post = {.bloom = .5f, .occlusion = 1};
            artfSceneSetPost(s, &post);
            drawInto(r, s, frame);
            ArtfFrameStats st;
            artfRendererStats(r, &st);
            double sum = 0;
            bool named = st.passCount > 0 && st.passCount <= ARTF_PASS_TIMES, scene = false, bloom = false;
            for (uint32_t k = 0; k < st.passCount; ++k) {
                named = named && st.passes[k].name && st.passes[k].ms >= 0;
                if (!st.passes[k].name) continue;
                sum += st.passes[k].ms;
                scene |= !strcmp(st.passes[k].name, backend ? "scene" : "tiles");
                bloom |= !strcmp(st.passes[k].name, "bloom");
            }
            const double whole = backend ? st.gpuMs : st.cpuMs;
            printf("%s%s: %u passes, %.3f ms of %.3f\n", backend ? "GPU" : "CPU", floats ? " in floats" : "", st.passCount, sum, whole);
            CHECK(named && scene && bloom == (bool)floats);
            CHECK(whole > 0 && sum <= whole * 1.001 + .001 && sum >= whole * .9 - .05);
            artfSceneFree(s);
            artfRendererFree(r);
        }
    artfFree(frame);
}

/* ---- an orthographic camera ---- */

/* A scene for it: a floor, things on it that shine and that do not, the sun with shadows, a lamp. */
static ArtfScene* orthoScene(void) {
    ArtfScene* s = artfSceneNew();
    static uint32_t px[64 * 64];
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) px[y * 64 + x] = ((x / 8 + y / 8) & 1) ? 0xFFD0C8B0u : 0xFF404858u;
    ArtfMaterialDesc floorDesc = artfMaterialDefaults();
    floorDesc.texture = artfSceneAddTexture(s, px, 64, 64, 0);
    floorDesc.roughness = .25f;
    ArtfTransform t = artfTransformAt(0, -1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 30, 8), artfSceneAddMaterial(s, &floorDesc), &t);
    ArtfMaterialDesc metal = artfMaterialDefaults();
    metal.baseColor = artfVec3(.9f, .7f, .3f);
    metal.metallic = 1;
    metal.roughness = .15f;
    ArtfMaterialDesc red = artfMaterialDefaults();
    red.baseColor = artfVec3(.8f, .15f, .1f);
    red.roughness = .4f;
    t = artfTransformAt(-1.8f, 0, 0, 1);
    artfSceneAddInstance(s, artfSceneAddSphere(s, 1, 32, 48), artfSceneAddMaterial(s, &metal), &t);
    t = artfTransformTrs(artfVec3(.4f, 0, .3f), artfQuatAxisAngle(artfVec3(1, 0, .3f), 1.f), artfVec3(1, 1, 1));
    artfSceneAddInstance(s, artfSceneAddTorus(s, 1, .3f, 48, 24), artfSceneAddMaterial(s, &red), &t);
    t = artfTransformTrs(artfVec3(2, 0, -.5f), artfQuatAxisAngle(artfVec3(0, 1, 0), .5f), artfVec3(1, 1, 1));
    artfSceneAddInstance(s, artfSceneAddCube(s, 1.4f), 0, &t);
    artfSceneSetSun(s, artfVec3(-.4f, -1, -.3f), artfVec3(2, 1.9f, 1.7f));
    artfSceneAddLight(s, artfVec3(0, 1.5f, 2.5f), artfVec3(.2f, .5f, 1.f), 6.f);
    const ArtfShadowDesc shadows = {512, 20, false};
    artfSceneSetShadows(s, &shadows);
    const ArtfCamera eye = {artfVec3(1, 3, 7), artfVec3(0, 0, 0), artfVec3(0, 1, 0), .9f, .1f};
    artfSceneSetCamera(s, &eye);
    return s;
}

/* That scene drawn as drawn, through all that comes after a frame in floats, with fog that light goes through, and
 * traced: by a camera, or the scene's (NULL). */
#define ORTHO_SCENARIOS 4
static bool orthoPortable; /* the CPU's kernels as plain loops, on three threads */
static void orthoFrame(ArtfBackend backend, int k, const ArtfCamera* camera, uint32_t* frame, bool* drawn) {
    ArtfRendererOptions o = options(backend, orthoPortable ? 3 : 0, orthoPortable);
    o.hdr = k == 1 || k == 2;
    ArtfRenderer* r = artfRendererNew(&o);
    *drawn = r != NULL;
    if (!r) return;
    ArtfScene* s = orthoScene();
    if (camera) artfSceneSetCamera(s, camera);
    if (k == 1) {
        const ArtfPostDesc all = {.bloom = .2f, .occlusion = 1, .reflections = 1, .temporal = true, .focus = 6, .aperture = 6};
        artfSceneSetPost(s, &all);
    } else if (k == 2) {
        const ArtfPostDesc air = {.volumetric = 1, .scattering = .5f};
        artfSceneSetPost(s, &air);
        artfSceneSetFog(s, artfVec3(.5f, .5f, .55f), .05f);
    } else if (k == 3) {
        const ArtfTraceDesc rays = {ARTF_TRACE_RAYS, camera ? 8 : 2, 0, 0, false, false};
        artfSceneSetTrace(s, &rays);
    }
    for (int i = 0; i < (k == 3 && camera ? 8 : 3); ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, frame));
    artfSceneFree(s);
    artfRendererFree(r);
}

/* How many pixels are lit (green above 40), and their mean column and row. */
static int litPixels(const uint32_t* p, float* meanX, float* meanY) {
    int n = 0;
    double sx = 0, sy = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (CHANNEL(p[y * W + x], 8) > 40) {
                ++n;
                sx += x;
                sy += y;
            }
    if (meanX) *meanX = n ? (float)(sx / n) : 0;
    if (meanY) *meanY = n ? (float)(sy / n) : 0;
    return n;
}

/* An orthographic camera: perspective frames are what they were, to the byte; all rays go one way, so things keep their
 * size however far, and moving the eye back changes nothing; occlusion changes no pixel; and the backends agree on
 * all that comes after a frame and on tracing. */
static void testOrthographic(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* c = artfMalloc(W * H * 4);
    bool drawn = false, gpuDrawn = false;
    /* perspective, as before there was this: the hashes of the frames the commit before it drew */
    static const uint64_t before[ORTHO_SCENARIOS] = {0xE6A9A9FAD7DF5E12ull, 0x14711124B9342182ull, 0x5669C35191A4208Full, 0x8FAD2B4B2C45B748ull};
    for (int k = 0; k < ORTHO_SCENARIOS; ++k) {
        orthoFrame(ARTF_BACKEND_SOFTWARE, k, NULL, a, &drawn);
        CHECK(hashOf(a, W * H) == before[k]);
    }

    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        /* a cube 2 high, in a view 4 high: half the frame's height, from 10 away or 30 */
        ArtfScene* s = plainScene();
        addAt(s, artfSceneAddCube(s, 2), whiteMaterial(s), 0, 0, 0, 1);
        ArtfCamera eye = {artfVec3(0, 0, 10), artfVec3(0, 0, 0), artfVec3(0, 1, 0), 1.f, .1f, 4.f, 0};
        artfSceneSetCamera(s, &eye);
        drawInto(r, s, a);
        int rows = 0;
        for (int y = 0; y < H; ++y) rows += CHANNEL(a[y * W + W / 2], 8) > 40;
        printf("%s, orthographic: a cube 2 high in a view 4 high takes %d of %d rows\n", backend ? "GPU" : "CPU", rows, H);
        CHECK(abs(rows - H / 2) <= 1);
        eye.position.z = 30;
        artfSceneSetCamera(s, &eye);
        drawInto(r, s, b);
        CHECK(differentShare(a, b, 0) == 0);
        artfSceneFree(s);
        /* two balls, one far behind the other: the same size */
        s = plainScene();
        const ArtfMesh ball = artfSceneAddSphere(s, 1, 24, 32);
        const ArtfInstance near = addAt(s, ball, whiteMaterial(s), -2.f, 0, 0, 1);
        const ArtfInstance far = addAt(s, ball, whiteMaterial(s), 2.f, 0, -40, 1);
        eye.position.z = 10;
        eye.orthoHeight = 8;
        artfSceneSetCamera(s, &eye);
        artfInstanceSetOpacity(s, far, 0);
        drawInto(r, s, a);
        artfInstanceSetOpacity(s, far, 1);
        artfInstanceSetOpacity(s, near, 0);
        drawInto(r, s, b);
        float ax, ay, bx, by;
        const int nearPixels = litPixels(a, &ax, &ay), farPixels = litPixels(b, &bx, &by);
        printf("%s, orthographic: a ball 10 away takes %d pixels, one 50 away %d\n", backend ? "GPU" : "CPU", nearPixels, farPixels);
        CHECK(nearPixels > 1000 && abs(nearPixels - farPixels) <= nearPixels / 200 && fabsf(ay - by) < .5f && fabsf(bx - ax - 100.f) < .5f);
        artfSceneFree(s);
        /* what is hidden is not drawn, and no pixel changes for it; lighter meshes as far as the view is wide */
        s = orthoScene();
        const ArtfCamera iso = {artfVec3(6, 5, 6), artfVec3(0, 0, 0), artfVec3(0, 1, 0), 1.f, .1f, 6.f, 0};
        artfSceneSetCamera(s, &iso);
        ArtfTransform wall = artfTransformTrs(artfVec3(1.5f, 0, 1.5f), artfQuatAxisAngle(artfVec3(0, 1, 0), .785f), artfVec3(6, 6, .2f));
        artfSceneAddInstance(s, artfSceneAddCube(s, 1), 0, &wall);
        const ArtfMesh small = artfSceneAddSphere(s, .2f, 8, 12);
        for (int i = 0; i < 64; ++i) addAt(s, small, 0, -1.f - (float)(i % 8) * .3f, -.5f, -1.f - (float)(i / 8) * .3f, 1);
        drawInto(r, s, a);
        artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, true);
        for (int i = 0; i < 3; ++i) drawInto(r, s, b);
        ArtfFrameStats st;
        artfRendererStats(r, &st);
        if (!backend) printf("CPU, orthographic with occlusion: %u instances drawn of %u\n", st.instancesDrawn, artfSceneInstanceCount(s));
        CHECK(differentShare(a, b, 0) == 0);
        artfSceneFree(s);
        artfRendererFree(r);
    }

    /* lighter meshes as the view widens, by how big things are in it and not by how far */
    {
        ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        ArtfScene* s = plainScene();
        const ArtfMesh ball = artfSceneAddSphere(s, 1, 64, 96);
        addAt(s, ball, whiteMaterial(s), 0, 0, 0, 1);
        uint32_t binned[2];
        for (int wide = 0; wide < 2; ++wide) {
            const ArtfCamera eye = {artfVec3(0, 0, 5), artfVec3(0, 0, 0), artfVec3(0, 1, 0), 1.f, .1f, wide ? 200.f : 3.f, 0};
            artfSceneSetCamera(s, &eye);
            drawInto(r, s, a);
            ArtfFrameStats st;
            artfRendererStats(r, &st);
            binned[wide] = st.trianglesBinned;
        }
        printf("orthographic, levels of detail: a ball in a view 3 high bins %u triangles, in one 200 high %u\n", binned[0], binned[1]);
        CHECK(binned[1] * 16 < binned[0]);
        artfSceneFree(s);
        artfRendererFree(r);
    }

    /* the backends agree with four samples a pixel */
    {
        ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
        o.antialias = true;
        ArtfRenderer* gpu = artfRendererNew(&o);
        if (gpu) {
            o.backend = ARTF_BACKEND_SOFTWARE;
            ArtfRenderer* cpu = artfRendererNew(&o);
            ArtfScene* s = orthoScene();
            const ArtfCamera iso = {artfVec3(6, 5, 6), artfVec3(0, 0, 0), artfVec3(0, 1, 0), 1.f, .1f, 7.f, 0};
            artfSceneSetCamera(s, &iso);
            drawInto(cpu, s, a);
            drawInto(gpu, s, b);
            const double apart = differentShare(a, b, 24);
            printf("orthographic, four samples a pixel, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
            CHECK(apart < .005);
            artfSceneFree(s);
            artfRendererFree(cpu);
            artfRendererFree(gpu);
        }
    }

    /* the backends agree, on what is drawn, all that comes after, fog that light goes through, and rays */
    const ArtfCamera iso = {artfVec3(6, 5, 6), artfVec3(0, 0, 0), artfVec3(0, 1, 0), 1.f, .1f, 7.f, 0};
    static const double most[ORTHO_SCENARIOS] = {.003, .03, .01, .02};
    for (int k = 0; k < ORTHO_SCENARIOS; ++k) {
        orthoFrame(ARTF_BACKEND_VULKAN, k, &iso, b, &gpuDrawn);
        if (!gpuDrawn) break;
        orthoFrame(ARTF_BACKEND_SOFTWARE, k, &iso, a, &drawn);
        /* (and the CPU's bytes are the same whatever its kernels and threads) */
        orthoPortable = true;
        orthoFrame(ARTF_BACKEND_SOFTWARE, k, &iso, c, &drawn);
        orthoPortable = false;
        CHECK(hashOf(a, W * H) == hashOf(c, W * H));
        const double apart = differentShare(a, b, 24);
        printf("orthographic, %s, the GPU against the CPU: %.2f%% of pixels apart\n", k == 0 ? "drawn" : k == 1 ? "all after the frame" : k == 2 ? "fog that light goes through" : "traced", apart * 100);
        CHECK(apart < most[k]);
        /* and not the perspective's picture */
        orthoFrame(ARTF_BACKEND_SOFTWARE, k, NULL, b, &drawn);
        CHECK(differentShare(a, b, 24) > .2);
    }
    artfFree(a);
    artfFree(b);
    artfFree(c);
}

/* ---- picking ---- */

static ArtfMesh addStrip(ArtfScene* s);
static void bendStrip(ArtfScene* s, ArtfInstance i, float angle);

/* What is under a point of the frame, and along any ray: the right instance, place, normal and distance; through holes,
 * what is mostly not there and particles; bones as posed; in agreement with the frame drawn; and cheap. */
static void testPick(void) {
    ArtfScene* s = plainScene();
    const ArtfMaterial white = whiteMaterial(s);
    const ArtfInstance cube = addAt(s, artfSceneAddCube(s, 2), white, 0, 0, 0, 1);
    const ArtfInstance ball = addAt(s, artfSceneAddSphere(s, 1, 32, 48), white, 3, 0, 0, 1);
    ArtfPickHit hit;
    /* the middle of the frame: the cube's front, 4 from the eye */
    CHECK(artfScenePick(s, NULL, W * .5f, H * .5f, W, H, &hit));
    CHECK(hit.instance == cube && fabsf(hit.position.z - 1.f) < 1e-4f && fabsf(hit.distance - 4.f) < 1e-3f);
    CHECK(fabsf(hit.normal.z - 1.f) < 1e-5f && hit.u >= 0 && hit.u <= 1 && hit.v >= 0 && hit.v <= 1);
    CHECK(!artfScenePick(s, NULL, 1, 1, W, H, &hit));
    /* down onto the ball's top */
    CHECK(artfSceneRaycast(s, artfVec3(3, 5, 0), artfVec3(0, -2, 0), 0, &hit));
    CHECK(hit.instance == ball && fabsf(hit.position.y - 1.f) < .01f && hit.normal.y > .99f && fabsf(hit.distance - 4.f) < .01f);
    CHECK(!artfSceneRaycast(s, artfVec3(3, 5, 0), artfVec3(0, -1, 0), 3.5f, &hit));
    CHECK(!artfSceneRaycast(s, artfVec3(3, 5, 0), artfVec3(0, 1, 0), 0, &hit));
    /* a square in front of the cube: with holes where its texture has none, faded, shown, and of particles */
    ArtfMaterialDesc holes = {artfVec3(1, 1, 1), oneColor(s, 0x00FFFFFFu), 0, 1, artfVec3(0, 0, 0), ARTF_ALPHA_MASK, .5f, 0, false};
    const ArtfMaterial cut = artfSceneAddMaterial(s, &holes);
    const ArtfInstance square = addAt(s, addSquare(s, 0, 0, 0, 0), cut, 0, 0, 2, .5f);
    CHECK(artfScenePick(s, NULL, W * .5f, H * .5f, W, H, &hit) && hit.instance == cube);
    artfInstanceSetMaterial(s, square, white);
    CHECK(artfScenePick(s, NULL, W * .5f, H * .5f, W, H, &hit) && hit.instance == square && fabsf(hit.distance - 3.f) < 1e-3f);
    artfInstanceSetOpacity(s, square, .3f);
    CHECK(artfScenePick(s, NULL, W * .5f, H * .5f, W, H, &hit) && hit.instance == cube);
    artfInstanceSetOpacity(s, square, .8f);
    CHECK(artfScenePick(s, NULL, W * .5f, H * .5f, W, H, &hit) && hit.instance == square);
    artfSceneRemoveInstance(s, square);
    ArtfEmitterDesc smoke;
    memset(&smoke, 0, sizeof smoke);
    smoke.position = artfVec3(0, 0, 2.5f);
    smoke.size = smoke.sizeEnd = 1.5f;
    smoke.color = smoke.colorEnd = artfVec3(1, 1, 1);
    smoke.opacity = smoke.opacityEnd = 1;
    smoke.life = 10;
    const ArtfEmitter e = artfSceneAddEmitter(s, &smoke);
    artfEmitterBurst(s, e, 8);
    artfEmitterStep(s, e, .01f);
    CHECK(artfEmitterAlive(s, e) == 8);
    CHECK(artfScenePick(s, NULL, W * .5f, H * .5f, W, H, &hit) && hit.instance == cube);
    artfSceneRemoveEmitter(s, e);

    /* as the frame shows it, pixel by pixel, perspective and orthographic */
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    uint32_t* frame = artfMalloc(W * H * 4);
    for (int ortho = 0; ortho < 2; ++ortho) {
        const ArtfCamera eye = {artfVec3(1, 2, 6), artfVec3(1, 0, 0), artfVec3(0, 1, 0), 1.f, .1f, ortho ? 6.f : 0.f, 0};
        artfSceneSetCamera(s, &eye);
        drawInto(r, s, frame);
        int wrong = 0, shown = 0;
        for (int y = 0; y < H; y += 2)
            for (int x = 0; x < W; x += 2) {
                const bool there = artfScenePick(s, NULL, (float)x + .5f, (float)y + .5f, W, H, &hit), lit = CHANNEL(frame[y * W + x], 8) > 0;
                wrong += there != lit;
                shown += lit;
            }
        printf("picking, %s: %d of %d points that show differ from the frame\n", ortho ? "orthographic" : "perspective", wrong, shown);
        CHECK(shown > 1500 && wrong <= shown / 100);
    }
    /* orthographic: from the view's plane, straight ahead */
    const ArtfCamera plan = {artfVec3(0, 0, 10), artfVec3(0, 0, 0), artfVec3(0, 1, 0), 1.f, .5f, 4.f, 0};
    CHECK(artfScenePick(s, &plan, W * .375f, H * .5f, W, H, &hit) && hit.instance == cube && fabsf(hit.distance - 9.f) < 1e-3f && fabsf(hit.position.x + .8f) < 1e-3f);

    /* cheap: no tree is made again while nothing changes */
    const uint64_t top = s->trace.topVersion;
    const double t0 = artfNowMs();
    int found = 0;
    for (int i = 0; i < 10000; ++i) found += artfScenePick(s, NULL, (float)(i * 7 % W), (float)(i * 13 % H), W, H, &hit);
    const double took = artfNowMs() - t0;
    printf("picking: 10,000 picks in %.1f ms\n", took);
    CHECK(found > 0 && s->trace.topVersion == top);

    /* what moved, when a pick saw it first, is still seen to have moved by the frame after */
    const ArtfTransform moved = artfTransformAt(3, .5f, 0, 1);
    artfInstanceSetTransform(s, ball, &moved);
    CHECK(artfSceneRaycast(s, artfVec3(3, 5, 0), artfVec3(0, -1, 0), 0, &hit) && fabsf(hit.position.y - 1.5f) < .01f);
    artfSceneTraceSync(s);
    bool still = true;
    for (size_t i = 0; i < s->trace.instanceHandles.len; ++i)
        if (s->trace.instanceHandles.items[i] == ball) still = s->trace.motion.items[i * 12 + 7] == 0;
    CHECK(!still);

    /* bones, as posed */
    artfSceneRemoveInstance(s, ball);
    const ArtfInstance strip = addAt(s, addStrip(s), white, 4, -1, 0, 1);
    bendStrip(s, strip, 0);
    CHECK(artfSceneRaycast(s, artfVec3(4, .8f, 5), artfVec3(0, 0, -1), 0, &hit) && hit.instance == strip);
    bendStrip(s, strip, 1.4f);
    CHECK(!artfSceneRaycast(s, artfVec3(4, .8f, 5), artfVec3(0, 0, -1), 0, &hit) || hit.instance != strip);

    artfFree(frame);
    artfRendererFree(r);
    artfSceneFree(s);
}

/* A Vulkan renderer as on a device with no ray tracing hardware. */
static ArtfRenderer* gpuWithoutRayQueries(const ArtfRendererOptions* o) {
#ifdef _WIN32
    _putenv("ARTFUL_NO_RAY_QUERY=1");
#else
    setenv("ARTFUL_NO_RAY_QUERY", "1", 1);
#endif
    ArtfRenderer* r = artfRendererNew(o);
#ifdef _WIN32
    _putenv("ARTFUL_NO_RAY_QUERY=");
#else
    unsetenv("ARTFUL_NO_RAY_QUERY");
#endif
    return r;
}

/* A floor, a cube over it, a pane over it that lets half the light through, a light that casts, and the sun. */
static ArtfScene* tracedScene(float sunSize, float lampSize) {
    ArtfScene* s = plainScene();
    artfSceneSetSun(s, artfVec3(.3f, -1, .2f), artfVec3(1.2f, 1.2f, 1.2f));
    artfSceneSetSunSize(s, sunSize);
    artfSceneSetAmbient(s, artfVec3(.05f, .05f, .05f), artfVec3(.05f, .05f, .05f));
    setCamera(s, 0, 4, 6, 0, 0, 0);
    ArtfMaterialDesc grey = artfMaterialDefaults();
    grey.baseColor = artfVec3(.6f, .6f, .6f);
    addAt(s, artfSceneAddPlane(s, 20, 1), artfSceneAddMaterial(s, &grey), 0, -1, 0, 1);
    addAt(s, artfSceneAddCube(s, 1), whiteMaterial(s), -1, 0, 0, 1);
    const ArtfTransform flat = artfTransformTrs(artfVec3(1.5f, .5f, 0), artfQuatAxisAngle(artfVec3(1, 0, 0), -1.5708f), artfVec3(1, 1, 1));
    const ArtfInstance pane = artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), whiteMaterial(s), &flat);
    artfInstanceSetOpacity(s, pane, .5f);
    const ArtfLight lamp = artfSceneAddLight(s, artfVec3(1.5f, 2.5f, 0), artfVec3(.8f, .8f, .8f), 9);
    artfLightSetShadow(s, lamp, true);
    artfLightSetSize(s, lamp, lampSize);
    return s;
}

/* Shadows from rays (ArtfShadowDesc.traced): exact, through what shows through as much as it is not there, as the
 * tracer's own; soft by the lights' sizes; the same on the CPU, on the GPU through the ray tracing hardware, and on a GPU
 * without; and with them off again, the bytes as before. */
static void testTracedShadows(void) {
    const ArtfShadowDesc maps = {2048, 20, false, false}, traced = {0, 0, false, true};
    {
        /* where the trees say: under the cube nothing of the sun, under the pane half, in the open all of it */
        ArtfScene* s = tracedScene(0, 0);
        artfSceneSetShadows(s, &traced);
        artfSceneTraceSync(s);
        ArtfFrameData f;
        artfSceneFrame(s, W, H, &f);
        const float up[3] = {0, 1, 0}, underCube[3] = {-.7f, -1, .2f}, underPane[3] = {1.95f, -1, .3f}, open[3] = {3, -1, -3}, belowLamp[3] = {1.5f, -1, 0};
        const float a = artfTracedShadow(s, &f, underCube, up, -1), b = artfTracedShadow(s, &f, underPane, up, -1), c = artfTracedShadow(s, &f, open, up, -1);
        const float d = artfTracedShadow(s, &f, belowLamp, up, 0);
        printf("shadows from rays: under a cube %.3f of the sun, under a pane of half %.3f, in the open %.3f; under the pane, of the lamp %.3f\n", a, b, c, d);
        CHECK(a == 0 && c == 1 && fabsf(b - .5f) < .01f && fabsf(d - .5f) < .01f);
        CHECK(f.tracedCasts == (ARTF_TRACED_SUN | 1u) && f.traced == ARTF_TRACED_SHADOWS);
        artfSceneFree(s);
    }
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* frames[3][2];
    for (int k = 0; k < 3; ++k) frames[k][0] = frames[k][1] = NULL;
    bool hardware = false;
    artfSetLog(sawHardware, &hardware);
    for (int backend = 0; backend < 3; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = backend == 2 ? gpuWithoutRayQueries(&o) : artfRendererNew(&o);
        if (!r) continue;
        const char* name = backend == 0 ? "CPU" : backend == 1 ? "GPU" : "GPU without ray queries";
        ArtfScene* s = tracedScene(0, 0);
        /* against maps: the same shadows, but at their edges */
        artfSceneSetShadows(s, &maps);
        drawInto(r, s, a);
        artfSceneSetShadows(s, &traced);
        drawInto(r, s, b);
        const double apart = differentShare(a, b, 24);
        printf("%s, shadows from rays against maps: %.2f%% of pixels apart\n", name, apart * 100);
        CHECK(apart < .03);
        uint32_t** hard = &frames[0][backend > 0];
        if (backend < 2) {
            *hard = artfMalloc(W * H * 4);
            memcpy(*hard, b, W * H * 4);
        } else if (*hard) {
            const double rq = differentShare(*hard, b, 24);
            printf("shadows from rays on the GPU, through the trees against the hardware: %.2f%% of pixels apart\n", rq * 100);
            CHECK(rq < .005);
        }
        /* off again: as they were, to the byte */
        artfSceneSetShadows(s, &maps);
        drawInto(r, s, b);
        CHECK(differentShare(a, b, 0) == 0);
        artfSceneFree(s);
        /* soft: the light's size and the sun's make their shadows wider */
        for (int sun = 0; sun < 2; ++sun) {
            ArtfScene* bulb = bulbScene(.5f);
            if (sun) {
                artfSceneRemoveLight(bulb, 1);
                artfSceneSetSun(bulb, artfVec3(.3f, -1, .4f), artfVec3(1.5f, 1.5f, 1.5f));
                artfSceneSetSunSize(bulb, .25f);
            } else {
                artfLightSetShadow(bulb, 1, true);
            }
            artfSceneSetShadows(bulb, &traced);
            drawInto(r, bulb, b);
            if (sun) artfSceneSetSunSize(bulb, 0);
            else artfLightSetSize(bulb, 1, 0);
            drawInto(r, bulb, a);
            printf("%s, soft shadows from rays, from %s: %d pixels between light and shadow, %d sharp\n", name, sun ? "the sun" : "a light", greyCount(b), greyCount(a));
            CHECK(greyCount(b) > greyCount(a) + 100);
            uint32_t** kept = &frames[1 + sun][backend > 0];
            if (backend < 2) {
                *kept = artfMalloc(W * H * 4);
                memcpy(*kept, b, W * H * 4);
            } else if (*kept) {
                const double rq = differentShare(*kept, b, 24);
                printf("soft shadows from rays on the GPU, through the trees against the hardware: %.2f%% of pixels apart\n", rq * 100);
                CHECK(rq < .005);
            }
            artfSceneFree(bulb);
        }
        artfRendererFree(r);
    }
    artfSetLog(NULL, NULL);
    for (int k = 0; k < 3; ++k) {
        if (frames[k][0] && frames[k][1]) {
            const double apart = differentShare(frames[k][0], frames[k][1], 24);
            printf("shadows from rays%s, the GPU against the CPU: %.2f%% of pixels apart\n", k == 0 ? "" : k == 1 ? ", soft from a light" : ", soft from the sun", apart * 100);
            CHECK(apart < .01);
        }
        artfFree(frames[k][0]);
        artfFree(frames[k][1]);
    }
    printf(hardware ? "(shadows from rays went through the ray tracing hardware)\n" : "(no ray tracing hardware here)\n");
    artfFree(a);
    artfFree(b);
}

/* What bones bend has its bottom level built again each frame it moves, alone, and that draws as building all does: a
 * strip bent over a floor, its shadow from rays, posed step by step on one GPU, and at the end drawn by another that
 * builds all at once. (The hardware's refit did not: see rtRebuildMoved in vulkan.c.) */
static void testTracedMoved(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* stepped = artfRendererNew(&o);
    ArtfRenderer* fresh = artfRendererNew(&o);
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* c = artfMalloc(W * H * 4);
    const ArtfShadowDesc traced = {0, 0, false, true};
    ArtfScene* s = plainScene();
    artfSceneSetSun(s, artfVec3(.5f, -1, -.4f), artfVec3(1.2f, 1.2f, 1.2f));
    artfSceneSetAmbient(s, artfVec3(.05f, .05f, .05f), artfVec3(.05f, .05f, .05f));
    setCamera(s, 0, 3, 5, 0, .5f, 0);
    addAt(s, artfSceneAddPlane(s, 20, 1), whiteMaterial(s), 0, 0, 0, 1);
    const ArtfInstance strip = addAt(s, addStrip(s), whiteMaterial(s), 0, 0, 0, 1);
    artfSceneSetShadows(s, &traced);
    if (stepped && fresh && cpu) {
        for (int step = 0; step <= 6; ++step) {
            bendStrip(s, strip, .15f * (float)step);
            CHECK(artfRendererDraw(stepped, s));
            CHECK(artfRendererDraw(cpu, s));
        }
        drawInto(stepped, s, a);
        drawInto(fresh, s, b);
        drawInto(cpu, s, c);
        const double apart = differentShare(a, b, 0), cpuApart = differentShare(b, c, 24);
        printf("shadows from rays of what bones bend, built again as it moved, against built whole at once: %.3f%% of pixels apart; the GPU against the CPU: %.2f%%\n", apart * 100, cpuApart * 100);
        CHECK(apart == 0);
        CHECK(cpuApart < .01);
        /* and it was bent: the shadow is not where it was */
        bendStrip(s, strip, 0);
        drawInto(fresh, s, c);
        CHECK(differentShare(b, c, 24) > .002);
    }
    artfSceneFree(s);
    artfRendererFree(stepped);
    artfRendererFree(fresh);
    artfRendererFree(cpu);
    artfFree(a);
    artfFree(b);
    artfFree(c);
}

/* How much redder than green a part of the frame is. */
static double rednessOf(const uint32_t* f, int x0, int y0, int x1, int y1) {
    double sum = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) sum += (double)CHANNEL(f[y * W + x], 16) - (double)CHANNEL(f[y * W + x], 8);
    return sum / ((double)(x1 - x0) * (y1 - y0));
}

/* Reflections from rays (ArtfPostDesc.tracedReflections): a mirror floor shows a red wall above the frame, which the
 * march across the frame cannot; the same on the CPU and the GPU, with ray tracing hardware and without. */
static void testTracedReflections(void) {
    ArtfScene* s = plainScene();
    artfSceneSetSun(s, artfVec3(0, -1, -.3f), artfVec3(.3f, .3f, .3f));
    artfSceneSetAmbient(s, artfVec3(.1f, .1f, .1f), artfVec3(.1f, .1f, .1f));
    setCamera(s, 0, 2, 6, 0, 0, 0);
    ArtfMaterialDesc mirror = artfMaterialDefaults();
    mirror.baseColor = artfVec3(.9f, .9f, .9f);
    mirror.metallic = 1;
    mirror.roughness = .05f;
    addAt(s, artfSceneAddPlane(s, 40, 1), artfSceneAddMaterial(s, &mirror), 0, 0, 0, 1);
    ArtfMaterialDesc red = artfMaterialDefaults();
    red.baseColor = artfVec3(0, 0, 0);
    red.emissive = artfVec3(3, 0, 0);
    const ArtfTransform wall = artfTransformTrs(artfVec3(0, 10, -6), artfQuatIdentity(), artfVec3(8, 4, 1));
    artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &red), &wall);
    addAt(s, artfSceneAddSphere(s, .6f, 16, 24), whiteMaterial(s), 1.5f, .6f, 1, 1);
    uint32_t* frames[3] = {artfMalloc(W * H * 4), artfMalloc(W * H * 4), artfMalloc(W * H * 4)};
    uint32_t* marched = artfMalloc(W * H * 4);
    bool drawn[3] = {false, false, false};
    for (int backend = 0; backend < 3; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.hdr = true;
        ArtfRenderer* r = backend == 2 ? gpuWithoutRayQueries(&o) : artfRendererNew(&o);
        if (!r) continue;
        ArtfPostDesc post = {.reflections = 1};
        artfSceneSetPost(s, &post);
        drawInto(r, s, marched);
        post.tracedReflections = true;
        artfSceneSetPost(s, &post);
        drawInto(r, s, frames[backend]);
        drawn[backend] = true;
        const double before = rednessOf(marched, 120, 160, 200, 195), after = rednessOf(frames[backend], 120, 160, 200, 195);
        printf("%s, a wall above the frame in a mirror floor: red over green %.1f marched across the frame, %.1f from rays\n", backend == 0 ? "CPU" : backend == 1 ? "GPU" : "GPU without ray queries", before, after);
        CHECK(after > before + 30);
        artfRendererFree(r);
    }
    for (int k = 1; k < 3; ++k)
        if (drawn[0] && drawn[k]) {
            const double apart = differentShare(frames[0], frames[k], 24);
            printf("reflections from rays, the GPU%s against the CPU: %.2f%% of pixels apart\n", k == 2 ? " without ray queries" : "", apart * 100);
            CHECK(apart < .01);
        }
    artfSceneFree(s);
    for (int k = 0; k < 3; ++k) artfFree(frames[k]);
    artfFree(marched);
}
/* ---- scale: many as one, levels of what bends, emitters of many, meshes that come and go ---- */

static ArtfScene* groveScene(void) {
    ArtfScene* s = artfSceneNew();
    artfSceneSetBackground(s, artfVec3(0, 0, 0));
    const ArtfMesh ball = artfSceneAddSphere(s, .4f, 16, 24);
    ArtfMaterialDesc d = artfMaterialDefaults();
    d.baseColor = artfVec3(.3f, .7f, .3f);
    const ArtfMaterial m = artfSceneAddMaterial(s, &d);
    for (int z = 0; z < 6; ++z)
        for (int x = 0; x < 6; ++x) addAt(s, ball, m, (float)x - 2.5f, 0, (float)z - 2.5f, 1);
    return s;
}


/* Far away, many as one: balls in cubes become a merged mesh each past a distance, the same choice and near the same
 * picture on both backends; near, the same bytes as without it; cleared, all as it was. */
static void testHlod(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    o.backend = ARTF_BACKEND_VULKAN;
    ArtfRenderer* gpu = artfRendererNew(&o);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    uint32_t* c = artfMalloc((size_t)W * H * 4);
    ArtfScene* plain = groveScene();
    setCamera(plain, 0, 3, 8, 0, 0, 0);
    drawInto(cpu, plain, a);
    ArtfScene* s = groveScene();
    ArtfHlodDesc d;
    memset(&d, 0, sizeof d);
    d.cell = 100;
    /* (the cubes meet at the origin: four of them) */
    CHECK(artfSceneBuildHlod(s, &d) == 4);
    CHECK(artfSceneInstanceCount(s) == 40);
    CHECK(artfSceneBuildHlod(s, &d) == 0); /* (what is merged is not merged again) */
    setCamera(s, 0, 3, 8, 0, 0, 0);
    drawInto(cpu, s, b);
    CHECK(artfSceneHlodShown(s) == 0);
    CHECK(hashOf(a, (size_t)W * H) == hashOf(b, (size_t)W * H));
    /* far: four instances where there were 36 */
    setCamera(s, 0, 12, 40, 0, 0, 0);
    setCamera(plain, 0, 12, 40, 0, 0, 0);
    drawInto(cpu, s, b);
    CHECK(artfSceneHlodShown(s) == 4);
    ArtfFrameStats stats;
    artfRendererStats(cpu, &stats);
    CHECK(stats.instancesDrawn == 4);
    drawInto(cpu, plain, c);
    printf("many as one: %.3f%% of the pixels differ from all of them, far away\n", differentShare(b, c, 24) * 100);
    CHECK(differentShare(b, c, 24) < .01);
    if (gpu) {
        drawInto(gpu, s, c);
        CHECK(artfSceneHlodShown(s) == 4);
        CHECK(differentShare(b, c, 24) < .005);
    }
    /* near again, and cleared */
    setCamera(s, 0, 3, 8, 0, 0, 0);
    drawInto(cpu, s, b);
    CHECK(artfSceneHlodShown(s) == 0);
    CHECK(hashOf(a, (size_t)W * H) == hashOf(b, (size_t)W * H));
    setCamera(s, 0, 12, 40, 0, 0, 0);
    drawInto(cpu, s, b);
    artfSceneClearHlod(s);
    CHECK(artfSceneInstanceCount(s) == 36 && artfSceneHlodShown(s) == 0);
    setCamera(s, 0, 3, 8, 0, 0, 0);
    drawInto(cpu, s, b);
    CHECK(hashOf(a, (size_t)W * H) == hashOf(b, (size_t)W * H));
    /* some of them, by hand */
    ArtfInstance some[3] = {s->instanceHandles.items[0], s->instanceHandles.items[1], s->instanceHandles.items[2]};
    CHECK(artfSceneAddHlod(s, some, 3, NULL) != 0);
    CHECK(artfSceneInstanceCount(s) == 37);
    artfSceneFree(plain);
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfFree(c);
    artfRendererFree(cpu);
    if (gpu) artfRendererFree(gpu);
}

/* A tube that bends at its middle: 960 triangles, the upper half going with the second bone. */
static ArtfMesh addTube(ArtfScene* s) {
    enum { RINGS = 40, AROUND = 12 };
    static ArtfVertex v[(RINGS + 1) * (AROUND + 1)];
    static ArtfVertexSkin k[(RINGS + 1) * (AROUND + 1)];
    static uint32_t idx[RINGS * AROUND * 6];
    for (int r = 0; r <= RINGS; ++r)
        for (int a = 0; a <= AROUND; ++a) {
            const float y = (float)r / RINGS * 4.f, turn = (float)a / AROUND * 6.2831853f;
            const float cx = artfCosFast(turn), sz = artfSinFast(turn);
            const ArtfVertex q = {cx * .3f, y, sz * .3f, cx, 0, sz, (float)a / AROUND, (float)r / RINGS, 0};
            const float top = artfClampf((y - 1.6f) / .8f, 0, 1);
            const ArtfVertexSkin w = {{0, 1, 0, 0}, {1.f - top, top, 0, 0}};
            v[r * (AROUND + 1) + a] = q;
            k[r * (AROUND + 1) + a] = w;
        }
    uint32_t n = 0;
    for (uint32_t r = 0; r < RINGS; ++r)
        for (uint32_t a = 0; a < AROUND; ++a) {
            const uint32_t p = r * (AROUND + 1) + a, q = p + AROUND + 1;
            idx[n++] = p; idx[n++] = q + 1; idx[n++] = p + 1;
            idx[n++] = p; idx[n++] = q; idx[n++] = q + 1;
        }
    return artfSceneAddSkinnedMesh(s, v, (RINGS + 1) * (AROUND + 1), idx, n, k);
}

/* The upper half turned about (0, 2, 0), around z. */
static void bendTube(ArtfScene* s, ArtfInstance i, float angle) {
    const ArtfTransform turn = artfTransformTrs(artfVec3(0, 0, 0), artfQuatAxisAngle(artfVec3(0, 0, 1), angle), artfVec3(1, 1, 1));
    ArtfTransform joints[2] = {artfTransformIdentity(), turn};
    joints[1].m[3] = -turn.m[1] * 2.f;
    joints[1].m[7] = 2.f - turn.m[5] * 2.f;
    artfInstanceSetJoints(s, i, joints, 2);
}

/* What bends has levels: far, an instance takes a lighter one, bent as the whole would be, on both backends; near, the
 * whole. With the levels off, the picture far is near the same. */
static void testSkinLod(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    o.backend = ARTF_BACKEND_VULKAN;
    ArtfRenderer* gpu = artfRendererNew(&o);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    uint32_t* c = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    artfSceneSetLodError(s, 8); /* (so that the first level comes where the tube still shows) */
    const ArtfMesh tube = addTube(s);
    const ArtfMeshData* source = &s->meshes.items[tube - 1];
    CHECK(source->skinLodCount >= 2);
    const float error = source->skinLodErrors[0];
    const ArtfInstance i = addAt(s, tube, whiteMaterial(s), 0, -2, 0, 1);
    const uint32_t home = s->instances.items[0].mesh;
    setCamera(s, 0, 0, 3, 0, 0, 0);
    drawInto(cpu, s, a);
    CHECK(s->instances.items[0].mesh == home);
    /* as far as the first level takes */
    const float far = error * (float)H * .5f / tanf(.5f) / 8.f + 2.5f;
    printf("what bends: %u levels, the first at %.1f\n", source->skinLodCount, far);
    setCamera(s, 0, 0, far + 1.f, 0, 0, 0);
    bendTube(s, i, 1.2f);
    drawInto(cpu, s, a);
    const uint32_t level = s->instances.items[0].mesh;
    CHECK(level != home && s->meshes.items[level].home == home + 1);
    CHECK(s->meshes.items[level].indexCount < s->meshes.items[home].indexCount);
    {
        /* each vertex the level kept is where its own joints and weights put it */
        const ArtfMeshData* m = &s->meshes.items[level];
        ArtfVertex* got = artfMalloc((size_t)m->vertexCount * sizeof *got);
        artfMeshUnpack(m, got);
        const ArtfTransform turn = artfTransformTrs(artfVec3(0, 0, 0), artfQuatAxisAngle(artfVec3(0, 0, 1), 1.2f), artfVec3(1, 1, 1));
        ArtfTransform joints[2] = {artfTransformIdentity(), turn};
        joints[1].m[3] = -turn.m[1] * 2.f;
        joints[1].m[7] = 2.f - turn.m[5] * 2.f;
        float worst = 0, weightsApart = 0;
        for (uint32_t v = 0; v < m->vertexCount; ++v) {
            const float* rest = m->skin->rest + (size_t)v * 6;
            float want[3] = {0, 0, 0};
            for (int k = 0; k < 8; ++k) {
                const float w = m->skin->weights[(size_t)v * 8 + k];
                const float* J = joints[m->skin->joints[(size_t)v * 8 + k] & 1].m;
                for (int c = 0; c < 3; ++c) want[c] += w * (J[c * 4] * rest[0] + J[c * 4 + 1] * rest[1] + J[c * 4 + 2] * rest[2] + J[c * 4 + 3]);
            }
            worst = artfMaxf(worst, fabsf(want[0] - got[v].px) + fabsf(want[1] - got[v].py) + fabsf(want[2] - got[v].pz));
            /* (and its weights are those it was given, by its height) */
            const float top = artfClampf((rest[1] - 1.6f) / .8f, 0, 1);
            weightsApart = artfMaxf(weightsApart, fabsf(m->skin->weights[(size_t)v * 8 + 1] - top));
        }
        printf("what bends: a level's vertices %.4f from where their bones put them\n", worst);
        CHECK(worst < .01f && weightsApart < 1e-6f);
        artfFree(got);
    }
    if (gpu) {
        drawInto(gpu, s, b);
        CHECK(differentShare(a, b, 24) < .005);
    }
    /* the pose is the whole's: straight is far from it, and the whole bent is near it */
    bendTube(s, i, 0);
    drawInto(cpu, s, c);
    CHECK(differentShare(a, c, 24) > .002);
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, false);
    bendTube(s, i, 1.2f);
    drawInto(cpu, s, b);
    CHECK(s->instances.items[0].mesh == home);
    printf("what bends, far: a level against the whole, %.3f%% of the pixels differ; bent against straight, %.3f%%\n", differentShare(a, b, 24) * 100, differentShare(a, c, 24) * 100);
    CHECK(differentShare(a, b, 24) < .01);
    artfSceneRemoveInstance(s, i);
    CHECK(artfSceneRemoveMesh(s, tube));
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfFree(c);
    artfRendererFree(cpu);
    if (gpu) artfRendererFree(gpu);
}

static ArtfEmitterDesc swarmDesc(uint32_t most) {
    ArtfEmitterDesc d;
    memset(&d, 0, sizeof d);
    d.position = artfVec3(0, -.5f, 0);
    d.area = .2f;
    d.velocity = artfVec3(0, 3, 0);
    d.spread = .5f;
    d.gravity = artfVec3(0, -4, 0);
    d.drag = .3f;
    d.rate = 400;
    d.life = 2;
    d.size = .25f;
    d.sizeEnd = .08f;
    d.color = artfVec3(1, .6f, .2f);
    d.colorEnd = artfVec3(.3f, .3f, .9f);
    d.opacity = 1;
    d.opacityEnd = .2f;
    d.glow = .5f;
    d.most = most;
    d.turbulence = 2;
    d.ground = true;
    d.groundHeight = -1;
    d.bounce = .4f;
    d.friction = .3f;
    d.spin = 2;
    d.parallel = true;
    return d;
}

static int litCount(const uint32_t* f) {
    int n = 0;
    for (int i = 0; i < W * H; ++i) n += (f[i] & 0xFFFFFF) != 0;
    return n;
}

/* Emitters of many: the same particles on any number of workers, as many alive as said, none through the ground; the
 * GPU's compute shader moves them as the CPU does, steps that wait included, and both draw the same; 100,000 of them
 * draw. */
static void testSwarm(void) {
    ArtfScene* s1 = plainScene();
    ArtfScene* s2 = plainScene();
    const ArtfEmitterDesc d = swarmDesc(3000);
    const ArtfEmitter e1 = artfSceneAddEmitter(s1, &d), e2 = artfSceneAddEmitter(s2, &d);
    artfEmitterBurst(s1, e1, 300);
    artfEmitterBurst(s2, e2, 300);
    for (int k = 0; k < 90; ++k) {
        artfEmitterStep(s1, e1, 1.f / 60.f);
        artfEmitterStep(s2, e2, 1.f / 60.f);
    }
    ArtfSwarm* w1;
    ArtfSwarm* w2;
    CHECK(artfSceneSwarms(s1, &w1, 1) == 1 && artfSceneSwarms(s2, &w2, 1) == 1);
    ArtfPool* pool = artfPoolNew(4);
    const float* p1 = artfSwarmCpu(w1, NULL);
    const float* p2 = artfSwarmCpu(w2, pool);
    CHECK(!memcmp(p1, p2, (size_t)artfSwarmPlaces(w1) * ARTF_SWARM_FLOATS * 4));
    uint32_t alive = 0;
    float lowest = 1e30f;
    for (uint32_t i = 0; i < artfSwarmPlaces(w1); ++i)
        if (p1[i * ARTF_SWARM_FLOATS + 3] >= 0) {
            ++alive;
            lowest = artfMinf(lowest, p1[i * ARTF_SWARM_FLOATS + 1]);
        }
    printf("emitters of many: %u alive, the lowest at %.4f\n", alive, lowest);
    CHECK(alive == artfEmitterAlive(s1, e1) && alive > 500);
    CHECK(lowest >= -1.0001f);
    artfPoolFree(pool);
    artfSceneFree(s1);
    artfSceneFree(s2);

    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    o.backend = ARTF_BACKEND_VULKAN;
    ArtfRenderer* gpu = artfRendererNew(&o);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    setCamera(s, 0, 1, 7, 0, .5f, 0);
    const ArtfEmitter e = artfSceneAddEmitter(s, &d);
    artfEmitterBurst(s, e, 200);
    for (int k = 0; k < 60; ++k) {
        artfEmitterStep(s, e, 1.f / 60.f);
        if (k % 20 == 0) {
            CHECK(artfRendererDraw(cpu, s));
            if (gpu) CHECK(artfRendererDraw(gpu, s));
        }
    }
    drawInto(cpu, s, a);
    printf("emitters of many: %d pixels lit\n", litCount(a));
    CHECK(litCount(a) > 2000);
    if (gpu) {
        drawInto(gpu, s, b);
        printf("emitters of many: %.3f%% of the pixels differ between the backends\n", differentShare(a, b, 24) * 100);
        CHECK(differentShare(a, b, 24) < .01);
    }
    artfSceneFree(s);

    /* many */
    s = plainScene();
    setCamera(s, 0, 1, 7, 0, .5f, 0);
    ArtfEmitterDesc many = swarmDesc(100000);
    many.size = many.sizeEnd = .05f;
    many.area = 1.5f;
    many.rate = 0;
    const ArtfEmitter big = artfSceneAddEmitter(s, &many);
    artfEmitterBurst(s, big, 100000);
    artfEmitterStep(s, big, 1.f / 60.f);
    CHECK(artfEmitterAlive(s, big) == 100000);
    CHECK(artfSceneInstanceCount(s) == 0);
    double t0 = artfNowMs();
    drawInto(cpu, s, a);
    printf("100,000 particles: %.1f ms for three frames on the CPU, %d pixels lit\n", artfNowMs() - t0, litCount(a));
    CHECK(litCount(a) > 5000);
    if (gpu) {
        drawInto(gpu, s, b);
        CHECK(differentShare(a, b, 24) < .03);
    }
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(cpu);
    if (gpu) artfRendererFree(gpu);
}

/* A ball of 48 x 64 squares, the same each time it is read; how many times it was. */
static uint32_t ballVertices(void) { return 49 * 65; }
static uint32_t ballIndices(void) { return 48 * 64 * 6; }
static bool readBall(void* user, ArtfVertex* v, uint32_t* idx) {
    ++*(int*)user;
    for (int r = 0; r <= 48; ++r)
        for (int a = 0; a <= 64; ++a) {
            const float up = (float)r / 48 * 3.1415927f, turn = (float)a / 64 * 6.2831853f;
            const float y = artfCosFast(up), ring = artfSinFast(up);
            const ArtfVertex q = {ring * artfCosFast(turn), y, ring * artfSinFast(turn), ring * artfCosFast(turn), y, ring * artfSinFast(turn), (float)a / 64, (float)r / 48, 0};
            v[r * 65 + a] = q;
        }
    uint32_t n = 0;
    for (uint32_t r = 0; r < 48; ++r)
        for (uint32_t a = 0; a < 64; ++a) {
            const uint32_t p = r * 65 + a, q = p + 65;
            idx[n++] = p; idx[n++] = p + 1; idx[n++] = q + 1;
            idx[n++] = p; idx[n++] = q + 1; idx[n++] = q;
        }
    return true;
}

/* Meshes that come and go: only the lightest level held at first; near, all of it, read again, and the same bytes as
 * the mesh given whole; far, the heavy levels leave, memory too; near again, the same bytes. A budget holds them out. */
static void testMeshStreaming(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    int reads = 0;
    ArtfScene* whole = plainScene();
    ArtfVertex* v = artfMalloc(ballVertices() * sizeof *v);
    uint32_t* idx = artfMalloc(ballIndices() * 4);
    readBall(&reads, v, idx);
    addAt(whole, artfSceneAddMesh(whole, v, ballVertices(), idx, ballIndices()), whiteMaterial(whole), 0, 0, 0, 1);
    setCamera(whole, 0, 0, 3, 0, 0, 0);
    drawInto(r, whole, a);
    artfFree(v);
    artfFree(idx);

    reads = 0;
    ArtfScene* s = plainScene();
    const ArtfMesh mesh = artfSceneAddMeshStreamed(s, ballVertices(), ballIndices(), readBall, &reads);
    CHECK(mesh && reads == 1);
    const int lightest = (int)s->meshes.items[mesh - 1].lodCount;
    CHECK(lightest >= 2 && artfMeshResidentLevel(s, mesh) == lightest);
    ArtfSceneMemory memory;
    artfSceneMemory(s, &memory);
    const size_t held = memory.residentMeshes;
    CHECK(held < memory.meshes / 2);
    addAt(s, mesh, whiteMaterial(s), 0, 0, 0, 1);
    ArtfResidencyDesc rd;
    memset(&rd, 0, sizeof rd);
    rd.mode = ARTF_RESIDENCY_AUTOMATIC;
    rd.interval = 1;
    rd.meshes = true;
    artfSceneSetResidency(s, &rd);
    setCamera(s, 0, 0, 3, 0, 0, 0);
    drawInto(r, s, b);
    CHECK(artfMeshResidentLevel(s, mesh) == 0 && reads == 2);
    CHECK(hashOf(a, (size_t)W * H) == hashOf(b, (size_t)W * H));
    setCamera(s, 0, 0, 300, 0, 0, 0);
    drawInto(r, s, b);
    CHECK(artfMeshResidentLevel(s, mesh) > 0);
    artfSceneMemory(s, &memory);
    CHECK(memory.residentMeshes < memory.meshes);
    setCamera(s, 0, 0, 3, 0, 0, 0);
    drawInto(r, s, b);
    CHECK(artfMeshResidentLevel(s, mesh) == 0 && reads == 3);
    CHECK(hashOf(a, (size_t)W * H) == hashOf(b, (size_t)W * H));
    /* over the budget: the lightest, whatever the distance, and drawn */
    rd.meshBudget = 1;
    artfSceneSetResidency(s, &rd);
    drawInto(r, s, b);
    CHECK(artfMeshResidentLevel(s, mesh) == lightest);
    CHECK(litCount(b) > 1000 && hashOf(a, (size_t)W * H) != hashOf(b, (size_t)W * H));
    /* traced, all of it */
    const ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 1, 0};
    artfSceneSetTrace(s, &rays);
    CHECK(artfRendererDraw(r, s));
    CHECK(artfMeshResidentLevel(s, mesh) == 0);
    artfSceneFree(s);
    artfSceneFree(whole);
    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}
/* ---- lights with a shape, profiles, and materials past glTF's core ---- */

/* bulbScene's light as a point (0), a tube along x (1), a rectangle facing down (2) or up (3). */
static ArtfScene* shapeScene(int kind) {
    ArtfScene* s = bulbScene(0);
    if (kind == 1) artfLightSetShape(s, 1, artfVec3(1, 0, 0), artfVec3(0, 0, 0), 3, 0);
    if (kind >= 2) artfLightSetShape(s, 1, artfVec3(1, 0, 0), artfVec3(0, kind == 2 ? -1.f : 1.f, 0), 3, 1);
    return s;
}

/* How wide and how tall what is brighter than over is, in green. */
static void brightBox(const uint32_t* f, int over, int* width, int* height) {
    int x0 = W, x1 = -1, y0 = H, y1 = -1;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (CHANNEL(f[y * W + x], 8) > over) {
                x0 = artfMini(x0, x);
                x1 = artfMaxi(x1, x);
                y0 = artfMini(y0, y);
                y1 = artfMaxi(y1, y);
            }
    *width = x1 - x0 + 1;
    *height = y1 - y0 + 1;
}


/* A tube's highlight is long, a rectangle's long and wide, and a rectangle lights only the side it faces; drawn the same
 * on both backends, and traced, with soft shadows. A shape taken away leaves the bytes as they were. */
static void testLightShapes(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        int bright[4], wide[4], tall[4];
        double under[4];
        for (int kind = 0; kind < 4; ++kind) {
            ArtfScene* s = shapeScene(kind);
            drawInto(r, s, a);
            bright[kind] = brightCount(a, 150);
            brightBox(a, 150, &wide[kind], &tall[kind]);
            under[kind] = meanOf(a, W / 2, H * 3 / 4, 8);
            artfSceneFree(s);
        }
        printf("%s, light shapes: bright pixels %d, %d, %d (point, tube, rectangle), as wide as %d, %d, %d; under a rectangle %.0f, %.0f facing away\n", backend ? "GPU" : "CPU", bright[0],
               bright[1], bright[2], wide[0], wide[1], wide[2], under[2], under[3]);
        CHECK(bright[1] > bright[0] * 2 && bright[2] > bright[0] * 2 && wide[1] > wide[0] * 3 / 2 && wide[2] > wide[0] * 3 / 2);
        CHECK(under[3] < under[2] * .25 && under[3] < under[0] * .25);
        artfRendererFree(r);
    }
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    for (int kind = 1; kind < 3 && gpu; ++kind) {
        ArtfScene* s = shapeScene(kind);
        drawInto(cpu, s, a);
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 24);
        printf("a %s, the GPU against the CPU: %.2f%% of pixels apart\n", kind == 1 ? "tube" : "rectangle", apart * 100);
        CHECK(apart < .005);
        artfSceneFree(s);
    }
    /* traced: soft shadows, and the same on both */
    ArtfScene* point = shapeScene(0);
    ArtfScene* panel = shapeScene(2);
    const ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 64, 0};
    artfSceneSetTrace(point, &rays);
    artfSceneSetTrace(panel, &rays);
    for (int k = 0; k < 24; ++k) drawInto(cpu, point, a);
    for (int k = 0; k < 24; ++k) drawInto(cpu, panel, b);
    printf("traced, a rectangle: %d pixels between light and shadow, %d from a point\n", greyCount(b), greyCount(a));
    CHECK(greyCount(b) > greyCount(a) + 200);
    if (gpu) {
        for (int k = 0; k < 24; ++k) drawInto(gpu, panel, a);
        const double apart = differentShare(a, b, 24);
        printf("traced, a rectangle, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .02);
    }
    artfSceneFree(point);
    artfSceneFree(panel);
    /* none again: the same bytes; and a light with a shape out of reach changes none */
    ArtfScene* s = shapeScene(0);
    drawInto(cpu, s, a);
    const uint64_t before = hashOf(a, W * H);
    artfLightSetShape(s, 1, artfVec3(1, 0, 0), artfVec3(0, -1, 0), 3, 1);
    drawInto(cpu, s, a);
    artfLightSetShape(s, 1, artfVec3(1, 0, 0), artfVec3(0, -1, 0), 0, 0);
    const ArtfLight far = artfSceneAddLight(s, artfVec3(0, 50, 0), artfVec3(1, 1, 1), 1);
    artfLightSetShape(s, far, artfVec3(0, 0, 1), artfVec3(0, 0, 0), 2, 0);
    drawInto(cpu, s, a);
    CHECK(hashOf(a, W * H) == before);
    artfSceneRemoveLight(s, far);
    CHECK(s->lightShape[far - 1][3] == 0);
    artfSceneFree(s);
    artfRendererFree(cpu);
    artfRendererFree(gpu);
    artfFree(a);
    artfFree(b);
}

/* IES files read, by their symmetry; a light with a narrow profile lights a small round below it, on both backends, drawn
 * and traced; taken away, the bytes are as they were. */
static void testProfiles(void) {
    static const char narrow[] = "IESNA:LM-63-2002\r\n[TEST] a narrow beam\r\n[MANUFAC] none\r\nTILT=NONE\r\n1 1000 1 7 1 1 1 0 0 0\r\n1 1 100\r\n0 10 20 30 40 60 90\r\n0\r\n"
                                 "1000 900 400 50 10 0 0\r\n";
    int vertical = 0, horizontal = 0;
    float* table = artfIesDecode(narrow, sizeof narrow - 1, &vertical, &horizontal);
    CHECK(table && vertical == 181 && horizontal == 1);
    if (!table) return;
    CHECK(table[0] == 1.f && fabsf(table[10] - .9f) < 1e-5f && fabsf(table[15] - .65f) < 1e-5f && table[90] == 0 && table[180] == 0);
    /* a quarter, mirrored round; tilted lamps' data skipped; commas between numbers */
    static const char quarter[] = "IESNA91\nTILT=INCLUDE\n1\n2\n0 90\n1 1\n1, 100, 1, 2, 3, 1, 1, 0, 0, 0\n1 1 50\n0 90\n0 45 90\n10 0\n20 0\n40 0\n";
    int qv = 0, qh = 0;
    float* q = artfIesDecode(quarter, sizeof quarter - 1, &qv, &qh);
    CHECK(q && qv == 181 && qh == 72);
    if (q) CHECK(q[0] == .25f && q[18 * 181] == 1.f && q[36 * 181] == .25f && q[54 * 181] == 1.f && fabsf(q[9 * 181] - .5f) < 1e-5f && q[181 + 120] == 0);
    artfFree(q);
    CHECK(!artfIesDecode("nothing here", 12, &qv, &qh) && !artfIesDecode("TILT=NONE\n1 2 3", 15, &qv, &qh));

    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    o.backend = ARTF_BACKEND_VULKAN;
    ArtfRenderer* gpu = artfRendererNew(&o);
    ArtfScene* s = bulbScene(0);
    artfLightSet(s, 1, artfVec3(0, 2, 0), artfVec3(2, 2, 2), 12);
    drawInto(cpu, s, a);
    const uint64_t before = hashOf(a, W * H);
    const double wide = meanOf(a, W / 2 + 70, H * 3 / 4, 8);
    artfLightSetProfile(s, 1, table, vertical, horizontal);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRenderer* r = backend ? gpu : cpu;
        if (!r) continue;
        drawInto(r, s, backend ? b : a);
        const uint32_t* f = backend ? b : a;
        printf("%s, a narrow profile: %.0f below it, %.0f to the side, %.0f there with none\n", backend ? "GPU" : "CPU", meanOf(f, W / 2, H * 3 / 4, 8), meanOf(f, W / 2 + 70, H * 3 / 4, 8), wide);
        CHECK(meanOf(f, W / 2 + 70, H * 3 / 4, 8) < wide * .5 && meanOf(f, W / 2, H * 3 / 4, 8) > meanOf(f, W / 2 + 70, H * 3 / 4, 8) * 2);
    }
    if (gpu) {
        const double apart = differentShare(a, b, 24);
        printf("a profile, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .005);
        const ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 64, 0};
        artfSceneSetTrace(s, &rays);
        for (int k = 0; k < 24; ++k) drawInto(cpu, s, a);
        for (int k = 0; k < 24; ++k) drawInto(gpu, s, b);
        const double traced = differentShare(a, b, 24);
        printf("a profile traced, the GPU against the CPU: %.2f%% of pixels apart; %.0f to the side\n", traced * 100, meanOf(a, W / 2 + 70, H * 3 / 4, 8));
        CHECK(traced < .02 && meanOf(a, W / 2 + 70, H * 3 / 4, 8) < wide * .5);
        const ArtfTraceDesc none = {ARTF_TRACE_NONE, 0, 0, 0};
        artfSceneSetTrace(s, &none);
    }
    artfLightSetProfile(s, 1, NULL, 0, 0);
    drawInto(cpu, s, a);
    CHECK(hashOf(a, W * H) == before && !s->lightProfile[0]);
    artfFree(table);
    artfSceneFree(s);
    artfRendererFree(cpu);
    artfRendererFree(gpu);
    artfFree(a);
    artfFree(b);
}

/* coatScene's sphere made of more: each changes the picture where it should, on both backends alike, drawn and traced. */
static ArtfScene* moreScene(int kind, ArtfMaterial* made) {
    ArtfScene* s = coatScene(0, 0);
    ArtfMaterialDesc d = artfMaterialDefaults();
    d.baseColor = artfVec3(.5f, .05f, .05f);
    d.roughness = .35f;
    if (kind == 1) d.specularColor = artfVec3(.2f, .5f, 3.f);
    if (kind == 2) {
        d.metallic = 1;
        d.baseColor = artfVec3(.9f, .9f, .9f);
        d.anisotropy = .9f;
    }
    if (kind == 3) {
        d.iridescence = 1;
        d.iridescenceThickness = 550;
    }
    if (kind == 4) {
        d.subsurface = artfVec3(1, .4f, .3f);
        d.subsurfaceRadius = .5f;
    }
    if (kind == 5) {
        /* all at once, and a coat with textures */
        static uint32_t checks[16 * 16];
        for (int i = 0; i < 16 * 16; ++i) checks[i] = (i / 16 / 4 + i % 16 / 4) & 1 ? 0xFFFFFFFFu : 0x40404040u;
        const ArtfTexture t = artfSceneAddTexture(s, checks, 16, 16, 0);
        d.clearcoat = 1;
        d.clearcoatRoughness = .1f;
        d.clearcoatMap = t;
        d.sheen = artfVec3(.5f, .5f, .5f);
        d.sheenRoughness = .4f;
        d.sheenMap = t;
        d.specularColor = artfVec3(1, .5f, .5f);
        d.anisotropy = .5f;
        d.iridescence = .5f;
        d.subsurface = artfVec3(.5f, .2f, .1f);
    }
    if (kind == 6) {
        d.clearcoat = 1;
        d.clearcoatRoughness = .1f;
    }
    *made = artfSceneAddMaterial(s, &d);
    artfInstanceSetMaterial(s, s->instanceHandles.items[0], *made);
    return s;
}

static void testMoreMaterials(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        ArtfMaterial m;
        ArtfScene* plain = moreScene(0, &m);
        drawInto(r, plain, a);
        double apart[7] = {0};
        for (int kind = 1; kind < 7; ++kind) {
            ArtfScene* s = moreScene(kind, &m);
            drawInto(r, s, b);
            apart[kind] = differentShare(a, b, 8);
            if (kind == 1) {
                /* what it reflects goes blue */
                long blue = 0;
                for (int i = 0; i < W * H; ++i) blue += CHANNEL(b[i], 0) - CHANNEL(a[i], 0) - (CHANNEL(b[i], 16) - CHANNEL(a[i], 16));
                CHECK(blue > W * H / 100);
            }
            if (kind == 6) {
                /* textured, the coat shows its checks: no other coat shows them */
                ArtfScene* t = moreScene(5, &m);
                drawInto(r, t, a);
                apart[0] = differentShare(a, b, 8);
                drawInto(r, plain, a);
                artfSceneFree(t);
            }
            artfSceneFree(s);
        }
        printf("%s, past glTF's core, shares of pixels changed: specular %.2f, anisotropy %.2f, iridescence %.2f, under the surface %.2f, all %.2f\n", backend ? "GPU" : "CPU", apart[1],
               apart[2], apart[3], apart[4], apart[5]);
        for (int kind = 1; kind < 6; ++kind) CHECK(apart[kind] > .01);
        CHECK(apart[0] > .01);
        artfSceneFree(plain);
        artfRendererFree(r);
    }
    /* light under the surface comes through from behind, where the sun's map says it is thin */
    {
        ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* cpu = artfRendererNew(&o);
        double back[2];
        for (int k = 0; k < 2; ++k) {
            ArtfScene* s = plainScene();
            artfSceneSetAmbient(s, artfVec3(.05f, .05f, .05f), artfVec3(.05f, .05f, .05f));
            artfSceneSetSun(s, artfVec3(0, 0, 1), artfVec3(2, 2, 2)); /* from behind, toward the eye */
            setCamera(s, 0, .5f, 4, 0, 0, 0);
            const ArtfShadowDesc maps = {1024, 20, false};
            artfSceneSetShadows(s, &maps);
            ArtfMaterialDesc thin = artfMaterialDefaults();
            thin.baseColor = artfVec3(.6f, .5f, .4f);
            thin.subsurface = k ? artfVec3(1, .5f, .4f) : artfVec3(0, 0, 0);
            thin.subsurfaceRadius = .2f;
            thin.doubleSided = true;
            /* (a thin plate, its back to the sun) */
            const ArtfTransform at = artfTransformTrs(artfVec3(0, 0, 1.8f), artfQuatIdentity(), artfVec3(1.4f, 1.4f, .03f));
            artfSceneAddInstance(s, artfSceneAddCube(s, 1), artfSceneAddMaterial(s, &thin), &at);
            drawInto(cpu, s, a);
            back[k] = meanOf(a, W / 2, H / 2 - 40, 16);
            artfSceneFree(s);
        }
        printf("light through a thin plate, from behind: %.0f, %.0f with none\n", back[1], back[0]);
        CHECK(back[1] > back[0] + 20);
        artfRendererFree(cpu);
    }
    /* both at once, on both backends: drawn and traced */
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    ArtfMaterial m;
    for (int kind = 2; kind < 6 && gpu; ++kind) {
        ArtfScene* s = moreScene(kind, &m);
        drawInto(cpu, s, a);
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 24);
        printf("past glTF's core (%d), the GPU against the CPU: %.2f%% of pixels apart\n", kind, apart * 100);
        CHECK(apart < .005);
        if (kind == 5 || kind == 4) {
            ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 32, 0};
            artfSceneSetTrace(s, &rays);
            for (int k = 0; k < 16; ++k) drawInto(cpu, s, a);
            for (int k = 0; k < 16; ++k) drawInto(gpu, s, b);
            const double traced = differentShare(a, b, 24);
            printf("past glTF's core (%d) traced, the GPU against the CPU: %.2f%% of pixels apart\n", kind, traced * 100);
            CHECK(traced < .02);
        }
        artfSceneFree(s);
    }
    /* what a glass's inside takes: traced, not drawn */
    {
        double green[2];
        uint64_t drawn[2];
        for (int k = 0; k < 2; ++k) {
            ArtfScene* s = moreScene(0, &m);
            /* (what is seen through it: a white wall) */
            ArtfMaterialDesc wall = artfMaterialDefaults();
            wall.baseColor = artfVec3(0, 0, 0);
            wall.emissive = artfVec3(1, 1, 1);
            addAt(s, artfSceneAddCube(s, 1), artfSceneAddMaterial(s, &wall), 0, 0, -7, 8);
            ArtfMaterialDesc glass = artfMaterialDefaults();
            glass.alphaMode = ARTF_ALPHA_BLEND;
            glass.transparency = .9f;
            glass.ior = 1.5f;
            glass.attenuation = k ? artfVec3(.1f, .9f, .1f) : artfVec3(0, 0, 0);
            glass.attenuationDistance = k ? .5f : 0.f;
            artfMaterialSet(s, m, &glass);
            drawInto(cpu, s, a);
            drawn[k] = hashOf(a, W * H);
            ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 16, 0};
            artfSceneSetTrace(s, &rays);
            for (int j = 0; j < 8; ++j) drawInto(cpu, s, a);
            green[k] = meanOf(a, W / 2, H / 2 - 20, 8) - meanOf(a, W / 2, H / 2 - 20, 16);
            if (k && gpu) {
                for (int j = 0; j < 8; ++j) drawInto(gpu, s, b);
                const double traced = differentShare(a, b, 24);
                printf("a glass's inside traced, the GPU against the CPU: %.2f%% of pixels apart\n", traced * 100);
                CHECK(traced < .02);
            }
            artfSceneFree(s);
        }
        printf("traced through a green glass: green over red %.0f, %.0f clear\n", green[1], green[0]);
        CHECK(green[1] > green[0] + 20 && drawn[0] == drawn[1]);
    }
    /* the same bytes with every instruction set: all of it, under a rectangle with a profile and the sun's shadows */
    {
        ArtfScene* s = moreScene(5, &m);
        const ArtfShadowDesc maps = {1024, 20, true};
        artfSceneSetShadows(s, &maps);
        const ArtfLight panel = artfSceneAddLight(s, artfVec3(.5f, 2, 1), artfVec3(1, .8f, .6f), 8);
        artfLightSetShape(s, panel, artfVec3(1, 0, 0), artfVec3(0, -1, 0), 1.5f, .5f);
        const float beam[3] = {1, .5f, 0};
        artfLightSetProfile(s, panel, beam, 3, 1);
        drawInto(cpu, s, a);
        ArtfRendererOptions plain = options(ARTF_BACKEND_SOFTWARE, 0, true);
        ArtfRenderer* portable = artfRendererNew(&plain);
        drawInto(portable, s, b);
        CHECK(hashOf(a, W * H) == hashOf(b, W * H));
        artfRendererFree(portable);
        artfSceneFree(s);
    }
    /* none again: the same bytes as a material that never had any */
    {
        ArtfScene* s = moreScene(0, &m);
        drawInto(cpu, s, a);
        const uint64_t before = hashOf(a, W * H);
        ArtfMaterial all;
        ArtfScene* rich = moreScene(5, &all);
        ArtfMaterialDesc d = artfMaterialDefaults();
        d.baseColor = artfVec3(.5f, .05f, .05f);
        d.roughness = .35f;
        artfMaterialSet(rich, all, &d);
        CHECK(rich->moreMaterials == 0 && rich->layeredMaterials == 0);
        drawInto(cpu, rich, a);
        CHECK(hashOf(a, W * H) == before);
        artfSceneFree(rich);
        artfSceneFree(s);
    }
    artfRendererFree(cpu);
    artfRendererFree(gpu);
    artfFree(a);
    artfFree(b);
}

/* glTF's extensions past the core: what they say, and their textures. */
static void testGltfMore(void) {
    static const char file[] =
        "{\"asset\":{\"version\":\"2.0\"},\"extensionsRequired\":[\"KHR_materials_specular\",\"KHR_materials_volume\",\"KHR_materials_iridescence\",\"KHR_materials_anisotropy\"],"
        "\"buffers\":[{\"byteLength\":36,\"uri\":\"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":36}],\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"type\":\"VEC3\","
        "\"count\":3,\"max\":[1,1,0],\"min\":[0,0,0]}],"
        "\"images\":[{\"uri\":\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"extensions\":{\"KHR_materials_clearcoat\":{\"clearcoatFactor\":1,\"clearcoatTexture\":{\"index\":0},\"clearcoatRoughnessTexture\":{\"index\":0},"
        "\"clearcoatNormalTexture\":{\"index\":0}},\"KHR_materials_sheen\":{\"sheenColorFactor\":[1,1,1],\"sheenRoughnessFactor\":0.5,\"sheenColorTexture\":{\"index\":0},"
        "\"sheenRoughnessTexture\":{\"index\":0}},\"KHR_materials_specular\":{\"specularFactor\":0.5,\"specularColorFactor\":[1,0.5,0.25]},"
        "\"KHR_materials_volume\":{\"attenuationColor\":[0.5,1,0.25],\"attenuationDistance\":2},\"KHR_materials_iridescence\":{\"iridescenceFactor\":0.75,"
        "\"iridescenceIor\":1.4,\"iridescenceThicknessMaximum\":500},\"KHR_materials_anisotropy\":{\"anisotropyStrength\":0.6,\"anisotropyRotation\":1.5707963}}}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"material\":0}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}]}";
    ArtfScene* s = artfSceneNew();
    CHECK(artfSceneLoadModelMemory(s, file, sizeof file - 1, NULL, NULL) != 0);
    const ArtfMaterialMore* m = &s->materialsMore.items[s->materialsMore.len - 1];
    CHECK(m->coatMap && m->coatMap == m->coatRoughnessMap && m->coatMap == m->coatNormalMap && m->coatMap == m->sheenMap && m->coatMap == m->sheenRoughnessMap);
    CHECK(m->sheenRoughness == .5f && m->specularColor[0] == .5f && m->specularColor[1] == .25f && m->specularColor[2] == .125f);
    CHECK(m->iridescence == .75f && fabsf(m->iridescenceIor - 1.4f) < 1e-6f && m->iridescenceThickness == 500.f && fabsf(m->anisotropy - .6f) < 1e-6f);
    CHECK(fabsf(m->anisotropyDirection[0]) < 1e-3f && fabsf(m->anisotropyDirection[1] - 1.f) < 1e-3f);
    CHECK(fabsf(m->absorb[0] - .5f) < 1e-3f && m->absorb[1] == 0 && fabsf(m->absorb[2] - 1.f) < 1e-3f);
    const uint32_t all = ARTF_MORE_COAT | ARTF_MORE_SHEEN | ARTF_MORE_SPECULAR | ARTF_MORE_ANISOTROPY | ARTF_MORE_IRIDESCENCE | ARTF_MORE_MAPS | ARTF_MORE_VOLUME;
    CHECK(m->flags == all && s->moreMaterials == 1 && s->layeredMaterials == 1);
    artfSceneFree(s);
}

/* ---- temporal quality and speed ---- */

#define TEMPORAL_BYTES 0x881775c1da02a664ull /* (what testTemporalBytes hashed before these came) */

/* Some frames, the last read back. */
static void drawFrames(ArtfRenderer* r, ArtfScene* s, int n, uint32_t* frame) {
    for (int i = 0; i < n; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, frame));
}

/* Upscaling: the scene drawn at half the size and made whole by temporal antialiasing is close to it drawn at the
 * frame's size, and without temporal antialiasing too, a little softer; both backends alike. The overlay and what is
 * read back are the frame's size. */
static void testUpscale(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* half[2] = {artfMalloc(W * H * 4), artfMalloc(W * H * 4)};
    uint32_t* corner = artfCalloc(W * H, 4);
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 30; ++x) corner[y * W + x] = 0xFF2040A0u;
    memset(half[1], 0, W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.hdr = true;
        ArtfRenderer* whole = artfRendererNew(&o);
        o.renderScale = .5f;
        ArtfRenderer* scaled = artfRendererNew(&o);
        if (!whole || !scaled) {
            artfRendererFree(whole);
            artfRendererFree(scaled);
            continue;
        }
        ArtfScene* s = bleedScene();
        const ArtfPostDesc temporal = {.temporal = true};
        artfSceneSetPost(s, &temporal);
        drawFrames(whole, s, 24, a);
        drawFrames(scaled, s, 24, b);
        const double apart = meanApart(a, b), far = differentShare(a, b, 48);
        printf("%s, upscaled from half the size with temporal antialiasing: %.2f of 255 from the whole size, %.2f%% of pixels by more than 48\n", backend ? "GPU" : "CPU", apart,
               far * 100);
        CHECK(apart < 2.5 && far < .02);
        memcpy(half[backend], b, W * H * 4);
        /* without: read between its pixels */
        artfSceneSetPost(s, NULL);
        drawInto(whole, s, a);
        drawInto(scaled, s, b);
        const double plain = meanApart(a, b);
        printf("%s, upscaled without: %.2f of 255 from the whole size\n", backend ? "GPU" : "CPU", plain);
        CHECK(plain < 4);
        /* the overlay goes over the frame shown */
        const ArtfOverlay over = {corner, W, H, 0, false, NULL, 0};
        artfRendererSetOverlay(scaled, &over);
        drawInto(scaled, s, b);
        CHECK(b[5 * W + 5] == 0xFF2040A0u && b[19 * W + 29] == 0xFF2040A0u && b[20 * W + 30] != 0xFF2040A0u);
        artfSceneFree(s);
        artfRendererFree(whole);
        artfRendererFree(scaled);
    }
    if (half[1][0] || half[1][W * H / 2]) {
        const double apart = differentShare(half[0], half[1], 24);
        printf("upscaled, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .02);
    }
    /* a scale of 1 is no scale */
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    o.hdr = true;
    ArtfRenderer* none = artfRendererNew(&o);
    o.renderScale = 1;
    ArtfRenderer* one = artfRendererNew(&o);
    ArtfScene* s = bleedScene();
    drawInto(none, s, a);
    drawInto(one, s, b);
    CHECK(differentShare(a, b, 0) == 0);
    artfSceneFree(s);
    artfRendererFree(none);
    artfRendererFree(one);
    artfFree(a);
    artfFree(b);
    artfFree(half[0]);
    artfFree(half[1]);
    artfFree(corner);
}

/* What bones bend is followed by its vertices: a strip bending is blurred where it moves, and temporal antialiasing
 * leaves no trail behind it; both backends alike. */
/* addStrip's, wider, with a picture of stripes across it: what is followed wrong smears them. */
static ArtfInstance addStripedStrip(ArtfScene* s) {
    enum { ROWS = 12 };
    static ArtfVertex v[(ROWS + 1) * 2];
    static ArtfVertexSkin k[(ROWS + 1) * 2];
    static uint32_t idx[ROWS * 6];
    static uint32_t stripes[8 * 64];
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 8; ++x) stripes[y * 8 + x] = (y / 4) & 1 ? 0xFFFFFFFFu : 0xFF202020u;
    for (int r = 0; r <= ROWS; ++r)
        for (int c = 0; c < 2; ++c) {
            const float y = (float)r / ROWS * 2.f;
            const ArtfVertex q = {c ? .5f : -.5f, y, 0, 0, 0, 1, (float)c, y * .5f, 0};
            const float top = artfClampf((y - .8f) / .4f, 0, 1);
            const ArtfVertexSkin w = {{0, 1, 0, 0}, {1.f - top, top, 0, 0}};
            v[r * 2 + c] = q;
            k[r * 2 + c] = w;
        }
    uint32_t n = 0;
    for (uint32_t r = 0; r < ROWS; ++r) {
        const uint32_t a = r * 2;
        idx[n++] = a; idx[n++] = a + 1; idx[n++] = a + 3;
        idx[n++] = a; idx[n++] = a + 3; idx[n++] = a + 2;
    }
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), artfSceneAddTexture(s, stripes, 8, 64, 0), 0, 1, artfVec3(0, 0, 0)};
    return addAt(s, artfSceneAddSkinnedMesh(s, v, (ROWS + 1) * 2, idx, n, k), artfSceneAddMaterial(s, &d), 0, -1, 0, 1);
}

static void testSkinnedMotion(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* blurred[2] = {artfMalloc(W * H * 4), artfMalloc(W * H * 4)};
    memset(blurred[1], 0, W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.hdr = true;
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        ArtfScene* s = plainScene();
        const ArtfInstance strip = addAt(s, addStrip(s), whiteMaterial(s), 0, -1, 0, 1);
        /* blurred: only where it bends */
        const ArtfPostDesc blur = {.motionBlur = .5f};
        artfSceneSetPost(s, &blur);
        bendStrip(s, strip, 0);
        drawInto(r, s, a);
        bendStrip(s, strip, .5f);
        drawOnce(r, s, blurred[backend]);
        artfSceneSetPost(s, NULL);
        drawInto(r, s, b);
        int moved = 0, below = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const bool differs = abs(CHANNEL(b[y * W + x], 16) - CHANNEL(blurred[backend][y * W + x], 16)) > 8;
                moved += differs;
                below += differs && y > H / 2 + 10; /* (the strip's foot, which the bone does not move) */
            }
        printf("%s, a strip that bones bend, blurred: %d pixels changed, %d where it does not move\n", backend ? "GPU" : "CPU", moved, below);
        CHECK(moved > 150 && below == 0);
        /* followed: no trail */
        const ArtfPostDesc temporal = {.temporal = true};
        artfSceneFree(s);
        s = plainScene();
        const ArtfInstance striped = addStripedStrip(s);
        artfSceneSetPost(s, &temporal);
        for (int k = 0; k < 16; ++k) {
            bendStrip(s, striped, (float)k * .1f);
            drawOnce(r, s, a);
        }
        /* (against the same, still, once temporal antialiasing has settled on it) */
        drawFrames(r, s, 32, b);
        const double trail = meanApart(a, b), far = differentShare(a, b, 48);
        printf("%s, temporal antialiasing on a striped strip that bones bend: %.2f of 255 from it settled still, %.2f%% of pixels by more than 48\n", backend ? "GPU" : "CPU",
               trail, far * 100);
        CHECK(trail < .6 && far < .004);
        artfSceneFree(s);
        artfRendererFree(r);
    }
    if (blurred[1][0] || blurred[1][W * H / 2]) {
        const double apart = differentShare(blurred[0], blurred[1], 24);
        printf("a bending strip blurred, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .005);
    }
    artfFree(a);
    artfFree(b);
    artfFree(blurred[0]);
    artfFree(blurred[1]);
}

/* Motion blur keeps the edges of what is in front: a ball standing still before a wall that moves stays sharp, and
 * none of it is smeared over the wall around it; both backends alike. */
static ArtfScene* movingWallScene(ArtfInstance* wall, ArtfInstance* ball) {
    ArtfScene* s = plainScene();
    setCamera(s, 0, 0, 6, 0, 0, 0);
    static uint32_t stripes[64 * 64];
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) stripes[y * 64 + x] = (x / 4) & 1 ? 0xFF206020u : 0xFF102010u;
    ArtfMaterialDesc d = {artfVec3(1, 1, 1), artfSceneAddTexture(s, stripes, 64, 64, 0), 0, 1, artfVec3(0, 0, 0)};
    *wall = addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &d), 0, 0, -4, 6);
    ArtfMaterialDesc white = {artfVec3(1, 1, 1), 0, 0, 1, artfVec3(0, 0, 0)};
    *ball = addAt(s, artfSceneAddSphere(s, .8f, 32, 48), artfSceneAddMaterial(s, &white), 0, 0, 0, 1);
    return s;
}

static void testBlurEdges(void) {
    uint32_t* still = artfMalloc(W * H * 4);
    uint32_t* bare = artfMalloc(W * H * 4);
    uint32_t* blurred[2] = {artfMalloc(W * H * 4), artfMalloc(W * H * 4)};
    uint32_t* wallOnly = artfMalloc(W * H * 4);
    memset(blurred[1], 0, W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.hdr = true;
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        ArtfInstance wall, ball;
        ArtfScene* s = movingWallScene(&wall, &ball);
        const ArtfPostDesc blur = {.motionBlur = 1};
        const ArtfTransform from = artfTransformAt(0, 0, -4, 6), to = artfTransformAt(.6f, 0, -4, 6);
        /* the ball still, and the wall moving behind it */
        drawInto(r, s, still);
        artfSceneSetPost(s, &blur);
        artfInstanceSetTransform(s, wall, &from);
        drawInto(r, s, bare);
        artfInstanceSetTransform(s, wall, &to);
        drawOnce(r, s, blurred[backend]);
        /* the wall alone, as it moved */
        const ArtfTransform away = artfTransformAt(0, 40, 0, 1);
        artfInstanceSetTransform(s, ball, &away);
        artfInstanceSetTransform(s, wall, &from);
        drawInto(r, s, bare);
        artfInstanceSetTransform(s, wall, &to);
        drawOnce(r, s, wallOnly);
        /* the ball is where the still frame is white; around it, a ring of 1 to 4 pixels */
        int inside = 0, insideChanged = 0, ring = 0;
        double ringApart = 0;
        for (int y = 4; y < H - 4; ++y)
            for (int x = 4; x < W - 4; ++x) {
                const bool white = CHANNEL(still[y * W + x], 16) > 200;
                bool near = false, deep = white;
                for (int dy = -4; dy <= 4; ++dy)
                    for (int dx = -4; dx <= 4; ++dx) {
                        const bool there = CHANNEL(still[(y + dy) * W + x + dx], 16) > 200;
                        near |= there;
                        if (abs(dx) <= 2 && abs(dy) <= 2) deep &= there;
                    }
                const uint32_t p = blurred[backend][y * W + x];
                if (deep) {
                    ++inside;
                    insideChanged += abs(CHANNEL(p, 16) - CHANNEL(still[y * W + x], 16)) > 2;
                } else if (near && !white) {
                    ++ring;
                    ringApart += abs(CHANNEL(p, 8) - CHANNEL(wallOnly[y * W + x], 8)) + abs(CHANNEL(p, 16) - CHANNEL(wallOnly[y * W + x], 16));
                }
            }
        ringApart /= ring > 0 ? ring : 1;
        printf("%s, motion blur before a still ball: %d of %d pixels of it changed; around it, %.1f of 255 from the wall alone\n", backend ? "GPU" : "CPU", insideChanged, inside,
               ringApart);
        CHECK(inside > 500 && insideChanged == 0 && ringApart < 8);
        artfSceneFree(s);
        artfRendererFree(r);
    }
    if (blurred[1][0] || blurred[1][W * H / 2]) {
        const double apart = differentShare(blurred[0], blurred[1], 24);
        printf("motion blur before a still ball, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .01);
    }
    artfFree(still);
    artfFree(bare);
    artfFree(wallOnly);
    artfFree(blurred[0]);
    artfFree(blurred[1]);
}

/* Soft shadows with temporal antialiasing take fewer reads a frame, turned from one frame to the next: what the frames
 * gather comes to what all the reads give; both backends alike. */
static void testSoftShadowsTurning(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    uint32_t* turned[2] = {artfMalloc(W * H * 4), artfMalloc(W * H * 4)};
    memset(turned[1], 0, W * H * 4);
    for (int backend = 0; backend < 2; ++backend) {
        ArtfRendererOptions o = options(backend ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE, 0, false);
        o.hdr = true;
        ArtfRenderer* r = artfRendererNew(&o);
        if (!r) continue;
        for (int sun = 0; sun < 2; ++sun) {
            ArtfScene* s = bulbScene(.5f);
            if (sun) {
                artfSceneRemoveLight(s, 1);
                artfSceneSetSun(s, artfVec3(.3f, -1, .4f), artfVec3(1.5f, 1.5f, 1.5f));
                artfSceneSetSunSize(s, .08f);
            } else {
                artfLightSetShadow(s, 1, true);
            }
            /* what temporal antialiasing itself changes, with sharp shadows: the edges it smooths */
            const ArtfShadowDesc hard = {1024, 20, false}, soft = {1024, 20, true};
            const ArtfPostDesc temporal = {.temporal = true};
            artfSceneSetShadows(s, &hard);
            drawInto(r, s, a);
            artfSceneSetPost(s, &temporal);
            drawFrames(r, s, 48, b);
            const double smoothed = meanApart(a, b), smoothedFar = differentShare(a, b, 32);
            /* all the reads, then turned and gathered over frames */
            artfSceneSetPost(s, NULL);
            artfSceneSetShadows(s, &soft);
            drawInto(r, s, a);
            artfSceneSetPost(s, &temporal);
            drawFrames(r, s, 48, b);
            const double apart = meanApart(a, b), far = differentShare(a, b, 32);
            printf("%s, soft shadows from %s, turned over 48 frames: %.2f of 255 from all the reads, %.2f%% of pixels by more than 32 (sharp ones, %.2f and %.2f%%)\n",
                   backend ? "GPU" : "CPU", sun ? "the sun" : "a light", apart, far * 100, smoothed, smoothedFar * 100);
            CHECK(apart < smoothed + .5 && far < smoothedFar + .004);
            if (!sun) memcpy(turned[backend], b, W * H * 4);
            artfSceneFree(s);
        }
        artfRendererFree(r);
    }
    if (turned[1][0] || turned[1][W * H / 2]) {
        const double apart = differentShare(turned[0], turned[1], 24);
        printf("soft shadows turned, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .01);
    }
    artfFree(a);
    artfFree(b);
    artfFree(turned[0]);
    artfFree(turned[1]);
}

/* What is off is as it was, to the byte: a frame in floats with soft shadows and no temporal antialiasing, and one
 * with temporal antialiasing following a moving instance, on the CPU, hash as they did before upscaling, vertices'
 * motion and turned shadows came. */
static void testTemporalBytes(void) {
    uint32_t* a = artfMalloc(W * H * 4);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 2, false);
    o.hdr = true;
    ArtfRenderer* r = artfRendererNew(&o);
    ArtfScene* s = richScene();
    const ArtfShadowDesc soft = {512, 20, true};
    artfSceneSetShadows(s, &soft);
    artfSceneSetSunSize(s, .05f);
    artfLightSetShadow(s, 1, true);
    artfLightSetSize(s, 1, .3f);
    const ArtfPostDesc some = {.bloom = .2f, .occlusion = .5f};
    artfSceneSetPost(s, &some);
    drawInto(r, s, a);
    uint64_t h = hashOf(a, (size_t)W * H);
    const ArtfPostDesc temporal = {.temporal = true};
    ArtfInstance ball;
    ArtfScene* moving = movingScene(&ball);
    artfSceneSetPost(moving, &temporal);
    for (int k = 0; k < 6; ++k) {
        placeBall(moving, ball, -.6f + (float)k * .2f);
        drawOnce(r, moving, a);
    }
    h = h * 31 + hashOf(a, (size_t)W * H);
    printf("what is off, to the byte: %016llx\n", (unsigned long long)h);
    CHECK(h == TEMPORAL_BYTES);
    artfSceneFree(s);
    artfSceneFree(moving);
    artfRendererFree(r);
    artfFree(a);
}

/* Light that bounced does not go through walls: inside a closed box under a bright sky, a point by the wall takes
 * next to nothing, though a probe outside is nearer to it than one inside. */
static void testProbesWalls(void) {
    ArtfScene* s = artfSceneNew();
    setCamera(s, 0, 0, 10, 0, 0, 0);
    artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(0, 0, 0));
    artfSceneSetAmbient(s, artfVec3(1, 1, 1), artfVec3(1, 1, 1));
    artfSceneAddInstance(s, artfSceneAddCube(s, 4), whiteMaterial(s), NULL);
    const ArtfIndirectDesc probes = {{8, 8, 8}, 128, .5f, 1, {-6, -6, -6}, {6, 6, 6}};
    artfSceneSetIndirect(s, &probes);
    ArtfFrameData frame;
    artfSceneFrame(s, W, H, &frame);
    for (int k = 0; k < 8; ++k) artfSceneIndirectStep(s, &frame, NULL);
    const float inside[3] = {1.8f, 0, 0}, inward[3] = {-1, 0, 0}, outside[3] = {2.3f, 0, 0}, outward[3] = {1, 0, 0};
    float in[3], out[3];
    artfIndirectAt(&s->indirect, inside, inward, in);
    artfIndirectAt(&s->indirect, outside, outward, out);
    printf("light that bounced, inside a box by its wall: %.3f of what is just outside\n", in[1] / out[1]);
    CHECK(out[1] > .5f && in[1] < out[1] * .1f);
    artfSceneFree(s);
    /* the kernels read the probes eight at a time: the same bytes as the portable ones, with walls and without */
    for (int through = 0; through < 2; ++through) {
        s = bleedScene();
        ArtfIndirectDesc d = {{8, 4, 8}, 64, .5f, 1, {-4, -1.5f, -3}, {4, 3, 3}, through != 0};
        uint32_t* a = artfMalloc(W * H * 4);
        uint32_t* b = artfMalloc(W * H * 4);
        ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
        ArtfRenderer* fast = artfRendererNew(&o);
        artfSceneSetIndirect(s, &d);
        drawInto(fast, s, a);
        o = options(ARTF_BACKEND_SOFTWARE, 0, true);
        ArtfRenderer* plain = artfRendererNew(&o);
        artfSceneSetIndirect(s, &d); /* (the probes start over: the same steps) */
        drawInto(plain, s, b);
        CHECK(differentShare(a, b, 0) == 0);
        artfRendererFree(fast);
        artfRendererFree(plain);
        artfSceneFree(s);
        artfFree(a);
        artfFree(b);
    }
}

/* A frame in floats with four samples a pixel: on the GPU as on the CPU, with what comes after and fog that light goes
 * through; and smoother than with one. */
static void testFloatsSamples(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    o.hdr = true;
    o.antialias = true;
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (!gpu) return;
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    o.antialias = false;
    ArtfRenderer* single = artfRendererNew(&o);
    ArtfScene* s = bleedScene();
    const ArtfPostDesc all = {.bloom = .2f, .occlusion = 1, .reflections = 1, .volumetric = 1, .scattering = .4f};
    artfSceneSetPost(s, &all);
    artfSceneSetFog(s, artfVec3(.5f, .5f, .55f), .03f);
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    drawInto(cpu, s, a);
    drawInto(gpu, s, b);
    const double apart = differentShare(a, b, 16);
    printf("four samples in floats, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
    CHECK(apart < .03);
    drawInto(single, s, b);
    CHECK(differentShare(a, b, 4) > .002); /* (edges come out otherwise) */
    artfSceneFree(s);
    artfRendererFree(gpu);
    artfRendererFree(cpu);
    artfRendererFree(single);
    artfFree(a);
    artfFree(b);
}

/* Lights past the frame's sixteen that ask for shadows get them, in the atlas's small maps: on both backends alike. */
static void testExtraShadows(void) {
    ArtfScene* s = artfSceneNew();
    artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(0, 0, 0));
    artfSceneSetAmbient(s, artfVec3(.02f, .02f, .02f), artfVec3(.02f, .02f, .02f));
    setCamera(s, 0, 6, 6, 0, 0, 0);
    const ArtfMaterial white = whiteMaterial(s);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 20, 1), white, NULL);
    const ArtfTransform above = artfTransformAt(0, 1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddCube(s, 1), white, &above);
    /* the frame's sixteen: far off and dark */
    for (int i = 0; i < 16; ++i) artfSceneAddLight(s, artfVec3(100.f + (float)i, 50, 0), artfVec3(0, 0, 0), 1);
    /* behind the cube and above it: its shadow falls toward the eye */
    const ArtfLight lamp = artfSceneAddLight(s, artfVec3(0, 3, -1.5f), artfVec3(1, 1, 1), 8);
    const ArtfShadowDesc shadows = {1024, 30};
    artfSceneSetShadows(s, &shadows);
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    drawInto(cpu, s, a);
    artfLightSetShadow(s, lamp, true);
    drawInto(cpu, s, b);
    /* under the cube, by the eye's side of it: darker with the shadow */
    const double without = meanOf(a, W / 2, H * 2 / 3, 8), with = meanOf(b, W / 2, H * 2 / 3, 8);
    printf("a light past the sixteen, under a cube: %.1f without its shadow, %.1f with\n", without, with);
    CHECK(with < without * .6);
    o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        drawInto(gpu, s, a);
        const double apart = differentShare(a, b, 24);
        printf("its shadow, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .02);
        artfRendererFree(gpu);
    }
    artfRendererFree(cpu);
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
}

/* Decals: a picture over the floor, through a box; the same on both backends, and gone when removed. */
static void testDecals(void) {
    ArtfScene* s = plainScene();
    setCamera(s, 0, 4, 3, 0, 0, 0);
    const ArtfTransform floor = artfTransformAt(0, 0, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 20, 1), whiteMaterial(s), &floor);
    /* a checker, red where it shows and clear between */
    static uint32_t checker[32 * 32];
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) checker[y * 32 + x] = ((x / 8 + y / 8) & 1) ? 0xFFFF2020u : 0x00000000u;
    ArtfDecalDesc d;
    memset(&d, 0, sizeof d);
    d.transform = artfTransformTrs(artfVec3(0, 0, 0), artfQuatAxisAngle(artfVec3(0, 1, 0), .3f), artfVec3(2, 1, 2));
    d.texture = artfSceneAddTexture(s, checker, 32, 32, 0);
    d.color = artfVec3(1, 1, 1);
    d.opacity = 1;
    const ArtfDecal decal = artfSceneAddDecal(s, &d);
    CHECK(decal != 0);
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    drawInto(cpu, s, a);
    /* red and white squares where the box meets the floor */
    int red = 0;
    for (int i = 0; i < W * H; ++i) red += (a[i] >> 16 & 255) > 200 && (a[i] >> 8 & 255) < 120;
    printf("a decal: %d pixels red\n", red);
    CHECK(red > W * H / 50);
    o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 24);
        printf("a decal, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .01);
        artfRendererFree(gpu);
    }
    artfSceneRemoveDecal(s, decal);
    drawInto(cpu, s, b);
    red = 0;
    for (int i = 0; i < W * H; ++i) red += (b[i] >> 16 & 255) > 200 && (b[i] >> 8 & 255) < 120;
    CHECK(red == 0);
    artfRendererFree(cpu);
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
}

/* Particles and terrain: made of the scene's own parts, so the backends draw them alike. */
static void testExtras(void) {
    ArtfScene* s = artfSceneNew();
    setCamera(s, 0, 1, 6, 0, 0, 0);
    static float heights[33 * 33];
    for (int z = 0; z < 33; ++z)
        for (int x = 0; x < 33; ++x) heights[z * 33 + x] = (float)((x - 16) * (x - 16) + (z - 16) * (z - 16)) / 512.f;
    const ArtfMesh hill = artfSceneAddTerrain(s, heights, 33, 33, .25f, 2.f, 1.f);
    CHECK(hill != 0);
    CHECK(s->meshes.items[hill - 1].vertexCount == 33 * 33 && s->meshes.items[hill - 1].indexCount == 32 * 32 * 6);
    const ArtfTransform below = artfTransformAt(0, -1.5f, 0, 1);
    artfSceneAddInstance(s, hill, whiteMaterial(s), &below);
    ArtfEmitterDesc d;
    memset(&d, 0, sizeof d);
    d.velocity = artfVec3(0, 1, 0);
    d.spread = .3f;
    d.rate = 200;
    d.life = 1;
    d.size = .3f;
    d.sizeEnd = .1f;
    d.color = artfVec3(1, .6f, .2f);
    d.colorEnd = artfVec3(1, .2f, .1f);
    d.opacity = 1;
    d.glow = 4;
    d.most = 100;
    const ArtfEmitter e = artfSceneAddEmitter(s, &d);
    for (int i = 0; i < 30; ++i) artfEmitterStep(s, e, 1.f / 30.f);
    /* as many as the rate and the life allow, and no more than most */
    const uint32_t alive = artfEmitterAlive(s, e);
    CHECK(alive >= 90 && alive <= 100);
    uint32_t* a = artfMalloc(W * H * 4);
    uint32_t* b = artfMalloc(W * H * 4);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    drawInto(cpu, s, a);
    /* the fire glows over the hill: somewhere above the middle, bright */
    int brightest = 0;
    for (int y = H / 4; y < H / 2; ++y)
        for (int x = W / 2 - 20; x < W / 2 + 20; ++x) brightest = artfMaxi(brightest, (int)(a[y * W + x] >> 16 & 255));
    CHECK(brightest > 240);
    o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        drawInto(gpu, s, b);
        const double apart = differentShare(a, b, 24);
        printf("particles and terrain, the GPU against the CPU: %.2f%% of pixels apart\n", apart * 100);
        CHECK(apart < .02);
        artfRendererFree(gpu);
    }
    /* gone, they leave nothing */
    artfSceneRemoveEmitter(s, e);
    drawInto(cpu, s, b);
    CHECK(differentShare(a, b, 24) > .002);
    artfRendererFree(cpu);
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
}

static void testGltfTargets(void) {
    static const char file[] = "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":80,\"uri\":\"data:application/octet-stream;base64,AAABAAIAAAAAAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABAQAAAAAA=\"}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":6},{\"buffer\":0,\"byteOffset\":8,\"byteLength\":36},{\"buffer\":0,\"byteOffset\":44,\"byteLength\":36}],\"accessors\":[{\"bufferView\":0,\"componentType\":5123,\"type\":\"SCALAR\",\"count\":3},{\"bufferView\":1,\"componentType\":5126,\"type\":\"VEC3\",\"count\":3,\"max\":[1,1,0],\"min\":[0,0,0]},{\"bufferView\":2,\"componentType\":5126,\"type\":\"VEC3\",\"count\":3}],\"meshes\":[{\"weights\":[1],\"primitives\":[{\"attributes\":{\"POSITION\":1},\"indices\":0,\"targets\":[{\"POSITION\":2}]}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    ArtfScene* s = artfSceneNew();
    const ArtfModel m = artfSceneLoadModelMemory(s, file, sizeof file - 1, NULL, NULL);
    CHECK(m && artfModelInstanceCount(s, m) == 1);
    artfScenePosesApply(s);
    /* the top went from 1 to 4, as the mesh's own weight says */
    ArtfVertex v[3];
    artfMeshUnpack(&s->meshes.items[s->instances.items[0].mesh], v);
    float top = 0;
    for (int i = 0; i < 3; ++i) top = artfMaxf(top, v[i].py);
    CHECK(fabsf(top - 4.f) < .02f);
    artfSceneFree(s);
}

static void testSlerp(void) {
    /* around y, at a steady speed, even where the two are far apart */
    const float half = 1.3089969f; /* 150 degrees, halved */
    const float a[4] = {0, 0, 0, 1}, b[4] = {0, sinf(half), 0, cosf(half)};
    for (int i = 1; i < 8; ++i) {
        float q[4];
        artfQuatSlerp(a, b, (float)i / 8.f, q);
        CHECK(fabsf(q[1] - sinf(half * (float)i / 8.f)) < 1e-5f && fabsf(q[3] - cosf(half * (float)i / 8.f)) < 1e-5f);
    }
}

/* ---- what bends ---- */

/* A strip standing up, of two bones: what is below y = 1 stays with the first, what is above goes with the second. */
static ArtfMesh addStrip(ArtfScene* s) {
    enum { ROWS = 12 };
    static ArtfVertex v[(ROWS + 1) * 2];
    static ArtfVertexSkin k[(ROWS + 1) * 2];
    static uint32_t idx[ROWS * 6];
    for (int r = 0; r <= ROWS; ++r)
        for (int c = 0; c < 2; ++c) {
            const float y = (float)r / ROWS * 2.f;
            const ArtfVertex q = {c ? .3f : -.3f, y, 0, 0, 0, 1, 0, 0, 0};
            const float top = artfClampf((y - .8f) / .4f, 0, 1);
            const ArtfVertexSkin w = {{0, 1, 0, 0}, {1.f - top, top, 0, 0}};
            v[r * 2 + c] = q;
            k[r * 2 + c] = w;
        }
    uint32_t n = 0;
    for (uint32_t r = 0; r < ROWS; ++r) {
        const uint32_t a = r * 2;
        idx[n++] = a; idx[n++] = a + 1; idx[n++] = a + 3;
        idx[n++] = a; idx[n++] = a + 3; idx[n++] = a + 2;
    }
    return artfSceneAddSkinnedMesh(s, v, (ROWS + 1) * 2, idx, n, k);
}

/* The second bone bent around (0, 1, 0), by this much around z. */
static void bendStrip(ArtfScene* s, ArtfInstance i, float angle) {
    const ArtfTransform turn = artfTransformTrs(artfVec3(0, 0, 0), artfQuatAxisAngle(artfVec3(0, 0, 1), angle), artfVec3(1, 1, 1));
    ArtfTransform joints[2] = {artfTransformIdentity(), turn};
    /* about the joint: move it to the origin, turn, and back */
    const float* m = turn.m;
    joints[1].m[3] = -m[1];
    joints[1].m[7] = 1.f - m[5];
    artfInstanceSetJoints(s, i, joints, 2);
}

static void testSkin(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    ArtfMaterial white = whiteMaterial(s);
    ArtfMesh strip = addStrip(s);
    ArtfInstance one = addAt(s, strip, white, -1, -1, 0, 1), two = addAt(s, strip, white, 1, -1, 0, 1);
    CHECK(artfSceneInstanceCount(s) == 2);
    /* a pixel high up on the left strip: there when straight, gone when bent; the other stays */
    const size_t high = (size_t)(H / 2 - 30) * W + W / 2 - 36, other = (size_t)(H / 2 - 30) * W + W / 2 + 36;
    for (int traced = 0; traced < 2; ++traced) {
        const ArtfTraceDesc rays = {ARTF_TRACE_RAYS, 1, 6, 0};
        artfSceneSetTrace(s, traced ? &rays : NULL);
        bendStrip(s, one, 0);
        for (int i = 0; i < 7; ++i) CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, a));
        CHECK(a[high] == 0xFFFFFFFFu && a[other] == 0xFFFFFFFFu);
        bendStrip(s, one, 1.4f);
        for (int i = 0; i < 7; ++i) CHECK(artfRendererDraw(r, s));
        CHECK(artfRendererReadPixels(r, b));
        CHECK(b[high] == 0xFF000000u && b[other] == 0xFFFFFFFFu);
    }
    artfSceneSetTrace(s, NULL);
    /* a wrong call says so */
    artfSetLog(countLog, &logged);
    logged = 0;
    ArtfInstance plain = addAt(s, addSquare(s, 0, 0, 0, 0), white, 0, 0, -3, 1);
    bendStrip(s, plain, 1.f);
    CHECK(logged == 1);
    artfSetLog(NULL, NULL);
    (void)two;
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

/* The two backends, drawn and traced, and shadows, see the same pose. */
/* A square that a target stretches up, and bones that take it far from where it was modelled. */
static void testMorph(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    const ArtfVertex v[4] = {{-.3f, -.3f, 0, 0, 0, 1, 0, 0, 0}, {.3f, -.3f, 0, 0, 0, 1, 0, 0, 0}, {.3f, .3f, 0, 0, 0, 1, 0, 0, 0}, {-.3f, .3f, 0, 0, 0, 1, 0, 0, 0}};
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    const float up[12] = {0, 0, 0, 0, 0, 0, 0, .8f, 0, 0, .8f, 0};
    const ArtfMorphTarget target = {up, NULL};
    ArtfVertexSkin k[4];
    memset(k, 0, sizeof k);
    for (int i = 0; i < 4; ++i) {
        k[i].joints[5] = 1; /* (in the second half of what a vertex has) */
        k[i].weights[5] = 1;
    }
    const ArtfMesh mesh = artfSceneAddMorphMesh(s, v, 4, idx, 6, k, &target, 1);
    CHECK(mesh != 0);
    const ArtfInstance i = addAt(s, mesh, whiteMaterial(s), 0, 0, 0, 1);
    const size_t high = (size_t)(H / 2 - 30) * W + W / 2, mid = (size_t)(H / 2) * W + W / 2;
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFFFFFFFFu && a[high] == 0xFF000000u);
    const float full = 1.f;
    artfInstanceSetMorphWeights(s, i, &full, 1);
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFFFFFFFFu && a[high] == 0xFFFFFFFFu);

    /* far out, and back: the box its bits stand for follows */
    ArtfTransform joints[2] = {artfTransformIdentity(), artfTransformIdentity()};
    joints[1].m[3] = 500.f;
    artfInstanceSetJoints(s, i, joints, 2);
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFF000000u);
    setCamera(s, 500, 0, 5, 500, 0, 0);
    drawInto(r, s, a);
    CHECK(a[mid] == 0xFFFFFFFFu && a[high] == 0xFFFFFFFFu);
    artfSceneFree(s);
    artfFree(a);
    artfRendererFree(r);
}

static void testSkinBothBackends(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (!gpu) return;
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = shadowScene();
    ArtfMesh strip = addStrip(s);
    ArtfInstance i = addAt(s, strip, 0, 2, -1, 1, 1);
    const ArtfShadowDesc desc = {1024, 40};
    artfSceneSetShadows(s, &desc);
    for (int step = 0; step < 4; ++step) {
        bendStrip(s, i, (float)step * .5f);
        drawInto(gpu, s, a);
        drawInto(cpu, s, b);
        CHECK(differentShare(a, b, 24) < .01);
    }
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfRendererFree(gpu);
    artfRendererFree(cpu);
}

/* ---- glTF ---- */

static void put32le(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

/* A .glb made here: two triangles of a square, with colors, under a node that moves and scales it. */
static size_t makeGlb(uint8_t* out, const char* extra, bool breakIt) {
    static const float positions[12] = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
    static const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    char json[1400];
    int n = snprintf(json, sizeof json,
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"children\":[1],\"translation\":[1,0,0]},{\"mesh\":0,\"scale\":[2,2,2]}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1,\"material\":0}]}],"
        "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.5,0.25,1,1]%s}],"
        "\"buffers\":[{\"byteLength\":60}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteLength\":48},{\"buffer\":0,\"byteOffset\":48,\"byteLength\":%d}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},{\"bufferView\":1,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}]}",
        extra, breakIt ? 4000 : 12);
    while (n & 3) json[n++] = ' ';
    memcpy(out, "glTF", 4);
    put32le(out + 4, 2);
    put32le(out + 8, (uint32_t)(12 + 8 + n + 8 + 60));
    put32le(out + 12, (uint32_t)n);
    put32le(out + 16, 0x4E4F534Au);
    memcpy(out + 20, json, (size_t)n);
    uint8_t* bin = out + 20 + n;
    put32le(bin, 60);
    put32le(bin + 4, 0x004E4942u);
    memcpy(bin + 8, positions, 48);
    memcpy(bin + 56, indices, 12);
    return (size_t)(20 + n + 8 + 60);
}

/* A .glb with a clip: a node that moves from x = 0 to x = 2 in 2 seconds; and a skin of two bones, the second
 * turning a quarter around z in the same time. */
static size_t makeAnimatedGlb(uint8_t* out) {
    static const float times[2] = {0, 2}, moves[6] = {0, 0, 0, 2, 0, 0};
    static const float turns[8] = {0, 0, 0, 1, 0, 0, .70710678f, .70710678f};
    static const float positions[12] = {-.5f, 0, 0, .5f, 0, 0, .5f, 2, 0, -.5f, 2, 0};
    static const uint16_t indices[6] = {0, 1, 2, 0, 2, 3};
    static const uint8_t joints[16] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    static const float weights[16] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    float inverses[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1};
    static uint8_t bin[512];
    size_t at = 0;
    size_t offsets[8], lengths[8];
#define PUT(i, p, n) do { offsets[i] = at; lengths[i] = n; memcpy(bin + at, p, n); at += ((size_t)(n) + 3) & ~(size_t)3; } while (0)
    PUT(0, times, 8); PUT(1, moves, 24); PUT(2, turns, 32); PUT(3, positions, 48); PUT(4, indices, 12); PUT(5, joints, 16); PUT(6, weights, 64); PUT(7, inverses, 128);
#undef PUT
    char views[900];
    int v = 0;
    for (int i = 0; i < 8; ++i) v += snprintf(views + v, sizeof views - (size_t)v, "%s{\"buffer\":0,\"byteOffset\":%d,\"byteLength\":%d}", i ? "," : "", (int)offsets[i], (int)lengths[i]);
    char json[3000];
    int n = snprintf(json, sizeof json,
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0,1,3]}],"
        "\"nodes\":[{\"mesh\":0,\"translation\":[0,0,0]},{\"children\":[2]},{\"translation\":[0,1,0]},{\"mesh\":0,\"skin\":0}],"
        "\"skins\":[{\"joints\":[1,2],\"inverseBindMatrices\":7}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":3,\"JOINTS_0\":5,\"WEIGHTS_0\":6},\"indices\":4}]}],"
        "\"animations\":[{\"name\":\"go\",\"samplers\":[{\"input\":0,\"output\":1},{\"input\":0,\"output\":2}],"
        "\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}},{\"sampler\":1,\"target\":{\"node\":2,\"path\":\"rotation\"}}]}],"
        "\"buffers\":[{\"byteLength\":%d}],\"bufferViews\":[%s],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},{\"bufferView\":1,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\"},{\"bufferView\":3,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
        "{\"bufferView\":4,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"},{\"bufferView\":5,\"componentType\":5121,\"count\":4,\"type\":\"VEC4\"},"
        "{\"bufferView\":6,\"componentType\":5126,\"count\":4,\"type\":\"VEC4\"},{\"bufferView\":7,\"componentType\":5126,\"count\":2,\"type\":\"MAT4\"}]}",
        (int)at, views);
    while (n & 3) json[n++] = ' ';
    memcpy(out, "glTF", 4);
    put32le(out + 4, 2);
    put32le(out + 8, (uint32_t)(12 + 8 + n + 8) + (uint32_t)at);
    put32le(out + 12, (uint32_t)n);
    put32le(out + 16, 0x4E4F534Au);
    memcpy(out + 20, json, (size_t)n);
    uint8_t* chunk = out + 20 + n;
    put32le(chunk, (uint32_t)at);
    put32le(chunk + 4, 0x004E4942u);
    memcpy(chunk + 8, bin, at);
    return (size_t)(20 + n + 8) + at;
}

static void testClips(void) {
    static uint8_t glb[8192];
    const size_t size = makeAnimatedGlb(glb);
    ArtfScene* s = plainScene();
    const ArtfModel m = artfSceneLoadModelMemory(s, glb, size, NULL, NULL);
    CHECK(m != 0);
    if (!m) { artfSceneFree(s); return; }
    CHECK(artfModelClipCount(s, m) == 1);
    CHECK(!strcmp(artfModelClipName(s, m, 0), "go"));
    CHECK(artfModelClipDuration(s, m, 0) == 2.f);
    CHECK(artfModelInstanceCount(s, m) == 2);
    /* a rigid part moves with its node; time goes round */
    artfAnimationSet(s, m, 0, .5f);
    CHECK(fabsf(s->instances.items[0].m[3] - .5f) < 1e-4f);
    artfAnimationSet(s, m, 0, 2.5f);
    CHECK(fabsf(s->instances.items[0].m[3] - .5f) < 1e-4f);
    /* a skinned part's top goes with its bone: from (x, 2) to around the joint at (0, 1): a quarter turn */
    const uint32_t bent = s->instances.items[1].mesh;
    artfAnimationSet(s, m, 0, 1.9999f);
    artfScenePosesApply(s);
    const ArtfMeshData* mesh = &s->meshes.items[bent];
    ArtfVertex v[4];
    artfMeshUnpack(mesh, v);
    float leftmost = 9;
    for (int i = 0; i < 4; ++i) leftmost = artfMinf(leftmost, v[i].px);
    CHECK(fabsf(leftmost + 1.f) < .02f);
    /* a matching clip in a scene with no clip asked for stays as it is */
    artfSetLog(countLog, &logged);
    logged = 0;
    artfAnimationSet(s, m, 9, 0);
    artfAnimationSet(s, 99, 0, 0);
    CHECK(logged == 2);
    artfSetLog(NULL, NULL);
    const ArtfTransform moved = artfTransformAt(5, 0, 0, 1);
    artfModelSetTransform(s, m, &moved);
    CHECK(s->instances.items[0].m[3] > 5.f);
    /* the file broken anywhere: no crash */
    static uint8_t bad[8192];
    for (size_t i = 0; i < size; i += 5) {
        memcpy(bad, glb, size);
        bad[i] ^= 0x5A;
        artfSetLog(countLog, &logged);
        ArtfScene* t = artfSceneNew();
        const ArtfModel made = artfSceneLoadModelMemory(t, bad, size, NULL, NULL);
        if (made && artfModelClipCount(t, made)) artfAnimationSet(t, made, 0, 1.f);
        artfSceneFree(t);
        artfSetLog(NULL, NULL);
    }
    artfSceneFree(s);
}

static void testGltf(void) {
    static uint8_t glb[4096];
    size_t size = makeGlb(glb, "}", false);
    ArtfScene* s = plainScene();
    CHECK(artfSceneLoadGltfMemory(s, glb, size, NULL, NULL) == 1);
    {
        /* the same, made apart from the scene */
        ArtfPreparedModel* prepared = artfPrepareModelMemory(glb, size, NULL);
        CHECK(prepared != NULL);
        ArtfScene* other = artfSceneNew();
        const ArtfModel model = artfSceneAddPreparedModel(other, prepared, NULL);
        CHECK(model && artfModelInstanceCount(other, model) == 1);
        CHECK(other->meshes.len == s->meshes.len);
        artfSceneFree(other);
        artfPreparedModelFree(artfPrepareModelMemory(glb, size, NULL)); /* (or left behind) */
    }
    ArtfVec3 center;
    float radius;
    CHECK(artfSceneBounds(s, &center, &radius));
    CHECK(near_((int)(center.x * 100.f), 100, 2) && near_((int)(center.y * 100.f), 0, 2));
    /* the node's scale came through, and its parent's move: what is at the middle is the material's color */
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    drawInto(r, s, a);
    const size_t at = (size_t)(H / 2) * W + W / 2 + 36;
    CHECK(near_(CHANNEL(a[at], 16), (int)(sqrtf(.5f) * 255.f), 2) && near_(CHANNEL(a[at], 8), (int)(sqrtf(.25f) * 255.f), 2));
    artfSceneFree(s);

    /* what is wrong is left out, and nothing goes past what is there */
    artfSetLog(countLog, &logged);
    logged = 0;
    s = plainScene();
    size = makeGlb(glb, "}", true);
    CHECK(artfSceneLoadGltfMemory(s, glb, size, NULL, NULL) == 1); /* (only its indices are gone: all vertices, no triangles) */
    artfSceneFree(s);
    s = plainScene();
    CHECK(artfSceneLoadGltfMemory(s, "{\"asset\":{\"version\":\"1.0\"}}", 28, NULL, NULL) == 0);
    CHECK(artfSceneLoadGltfMemory(s, "{not json", 9, NULL, NULL) == 0);
    CHECK(artfSceneLoadGltf(s, "no such file.glb", NULL) == 0);
    CHECK(logged >= 3);
    /* every cut of a good file, and every byte broken in turn: no crash, whatever comes out */
    size = makeGlb(glb, "}", false);
    for (size_t cut = 0; cut < size; cut += 7) artfSceneLoadGltfMemory(s, glb, cut, NULL, NULL);
    static uint8_t bad[4096];
    for (size_t i = 0; i < size; i += 3) {
        memcpy(bad, glb, size);
        bad[i] ^= 0x5A;
        artfSceneLoadGltfMemory(s, bad, size, NULL, NULL);
    }
    artfSceneFree(s);
    artfSetLog(NULL, NULL);

    /* an image: a PNG of 2 x 1 pixels, stored, one red and one half blue */
    static const uint8_t png[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 20, 'I', 'D', 'A', 'T', 0x78, 1, 1, 9, 0, 0xF6, 0xFF, 0, 255, 0, 0, 255, 0, 0, 255, 128, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 'I', 'E', 'N', 'D', 0, 0, 0, 0};
    int w = 0, h = 0;
    uint32_t* pixels = artfImageDecode(png, sizeof png, &w, &h);
    CHECK(pixels && w == 2 && h == 1);
    if (pixels) CHECK(pixels[0] == 0xFFFF0000u && pixels[1] == 0x800000FFu);
    artfFree(pixels);
    CHECK(!artfImageDecode("not a picture", 13, &w, &h));

    artfFree(a);
    artfRendererFree(r);
}

/* ---- rays ---- */

static void traceWith(ArtfScene* s, ArtfTraceMode mode, int samples, int bounces) {
    ArtfTraceDesc d = {mode, 1, samples, bounces};
    artfSceneSetTrace(s, &d);
}

static void drawSamples(ArtfRenderer* r, ArtfScene* s, int samples, uint32_t* frame) {
    for (int i = 0; i < samples; ++i) CHECK(artfRendererDraw(r, s));
    CHECK(artfRendererReadPixels(r, frame));
}

/* A mean of a little square of pixels around (x, y), for what is noisy. */
static double meanOf(const uint32_t* frame, int x, int y, int shift) {
    double sum = 0;
    for (int j = -3; j <= 3; ++j)
        for (int i = -3; i <= 3; ++i) sum += CHANNEL(frame[(size_t)(y + j) * W + x + i], shift);
    return sum / 49;
}

static void testTrace(ArtfBackend backend) {
    ArtfRendererOptions o = options(backend, 0, false);
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) return;
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    const size_t mid = (size_t)(H / 2) * W + W / 2;

    /* with nothing to reflect and no shadow to cast, rays show what the rasterizer shows, but for edges */
    ArtfScene* s = plainScene();
    artfSceneSetAmbient(s, artfVec3(.9f, .8f, .7f), artfVec3(.1f, .2f, .3f));
    ArtfMaterialDesc matte = {artfVec3(.8f, .4f, .3f), 0, 0, 1, artfVec3(.02f, 0, 0)};
    addAt(s, artfSceneAddSphere(s, 1, 32, 48), artfSceneAddMaterial(s, &matte), -1.5f, 0, 0, 1);
    addAt(s, addSquare(s, 0xFFFF8040u, 0xFF40FF80u, 0xFF8040FFu, 0xFFFFFFFFu), whiteMaterial(s), 1.5f, 0, 0, 1);
    drawInto(r, s, a);
    traceWith(s, ARTF_TRACE_RAYS, 16, 0);
    drawSamples(r, s, 16, b);
    const double far = differentShare(a, b, 24);
    printf("rays against triangles, pixels that differ by more than 24 of 255: %.2f%%\n", far * 100);
    CHECK(far < .01);
    ArtfFrameStats stats;
    artfRendererStats(r, &stats);
    CHECK(stats.traceSamples == 16);
    artfSceneFree(s);

    /* a shadow: a floor with a ball between it and the sun, on both sides of the shadow's edge */
    s = plainScene();
    ArtfMaterial white = whiteMaterial(s);
    artfSceneSetAmbient(s, artfVec3(.1f, .1f, .1f), artfVec3(.1f, .1f, .1f));
    artfSceneSetSun(s, artfVec3(0, -1, 0), artfVec3(.9f, .9f, .9f));
    setCamera(s, 0, 6, .01f, 0, 0, 0);
    ArtfTransform t = artfTransformAt(0, 0, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 30, 1), white, &t);
    addAt(s, artfSceneAddSphere(s, 1, 32, 32), white, 2.5f, 1.5f, 0, 1);
    traceWith(s, ARTF_TRACE_RAYS, 2, 0);
    drawSamples(r, s, 2, a);
    const double lit = meanOf(a, W / 2 - 60, H / 2, 8);
    CHECK(lit > 200);
    artfSceneSetSun(s, artfVec3(-.7f, -1, 0), artfVec3(.9f, .9f, .9f));
    drawSamples(r, s, 2, a);
    int dark = 0;
    for (size_t i = 0; i < (size_t)W * H; ++i) dark += CHANNEL(a[i], 8) < 110 && CHANNEL(a[i], 8) > 60;
    CHECK(dark > 300);
    artfSceneFree(s);

    /* a mirror shows what it faces, and a surface that does not shine does not */
    s = plainScene();
    white = whiteMaterial(s);
    ArtfMaterialDesc mirror = {.baseColor = {1, 1, 1}, .specular = 1.f, .roughness = .05f, .metallic = 1.f};
    ArtfMaterialDesc lamp = {artfVec3(0, 0, 0), 0, 0, 1, artfVec3(1, 0, 0)};
    artfSceneSetAmbient(s, artfVec3(0, 0, 0), artfVec3(0, 0, 0));
    addAt(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &mirror), 0, 0, 0, 2);
    const ArtfTransform behind = artfTransformTrs(artfVec3(0, 0, 8), artfQuatAxisAngle(artfVec3(0, 1, 0), 3.14159265f), artfVec3(8, 8, 8));
    artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &lamp), &behind);
    traceWith(s, ARTF_TRACE_RAYS, 8, 0);
    drawSamples(r, s, 8, a);
    CHECK(meanOf(a, W / 2, H / 2, 16) > 200);
    artfSceneFree(s);

    /* what is half there lets half through, and a hole all of it; no background gives alpha */
    s = plainScene();
    white = whiteMaterial(s);
    artfSceneSetBackgroundAlpha(s, 0);
    ArtfInstance half = addAt(s, addSquare(s, 0, 0, 0, 0), white, 0, 0, 0, 1);
    artfInstanceSetOpacity(s, half, .5f);
    traceWith(s, ARTF_TRACE_RAYS, 128, 0);
    drawSamples(r, s, 128, a);
    CHECK(a[0] == 0);
    CHECK(near_((int)meanOf(a, W / 2, H / 2, 24), 128, 14));
    artfSceneFree(s);

    /* paths: light from a red wall reaches a white floor that rays alone leave white */
    s = plainScene();
    white = whiteMaterial(s);
    artfSceneSetSun(s, artfVec3(-.6f, -1, -.3f), artfVec3(1, 1, 1));
    artfSceneSetAmbient(s, artfVec3(.05f, .05f, .05f), artfVec3(.05f, .05f, .05f));
    setCamera(s, 0, 3, 7, 0, 0, 0);
    ArtfMaterialDesc redWall = {artfVec3(1, .02f, .02f), 0, 0, 1, artfVec3(0, 0, 0)};
    t = artfTransformAt(0, -1, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 40, 1), white, &t);
    const ArtfTransform wall = artfTransformTrs(artfVec3(2.2f, 1, 0), artfQuatAxisAngle(artfVec3(0, 1, 0), -1.5708f), artfVec3(4, 2, 1));
    artfSceneAddInstance(s, addSquare(s, 0, 0, 0, 0), artfSceneAddMaterial(s, &redWall), &wall);
    traceWith(s, ARTF_TRACE_RAYS, 4, 0);
    drawSamples(r, s, 4, a);
    traceWith(s, ARTF_TRACE_PATHS, 128, 0);
    drawSamples(r, s, 128, b);
    const int fx = W / 2 + 28, fy = H / 2 + 34;
    const double rayTint = meanOf(a, fx, fy, 16) - meanOf(a, fx, fy, 8), pathTint = meanOf(b, fx, fy, 16) - meanOf(b, fx, fy, 8);
    printf("a white floor by a red wall, red over green: %.1f with rays, %.1f with paths\n", rayTint, pathTint);
    CHECK(pathTint > rayTint + 8);
    /* a still scene gathers, and a change starts over */
    artfRendererStats(r, &stats);
    CHECK(stats.traceSamples == 128);
    CHECK(artfRendererDraw(r, s));
    artfRendererStats(r, &stats);
    CHECK(stats.traceSamples == 128);
    setCamera(s, 0, 3, 6.9f, 0, 0, 0);
    CHECK(artfRendererDraw(r, s));
    artfRendererStats(r, &stats);
    CHECK(stats.traceSamples == 1);
    artfSceneSetTrace(s, NULL);
    drawInto(r, s, a);
    artfSceneFree(s);
    (void)mid;

    artfFree(a);
    artfFree(b);
    artfRendererFree(r);
}

/* Rays do not depend on threads or kernels; the trees hold what they hold. */
static void testTraceData(void) {
    ArtfScene* s = layeredScene();
    artfSceneTraceSync(s);
    const ArtfTrace* t = &s->trace;
    CHECK(t->instances.len == s->instances.len);
    /* every node's box holds its children's, and all leaves together hold each triangle once */
    uint8_t* seen = artfCalloc(t->triangles.len, 1);
    for (size_t i = 0; i < t->nodes.len; ++i) {
        const ArtfTraceNode* n = &t->nodes.items[i];
        if (n->count) {
            CHECK(n->count <= 4);
            for (uint32_t k = 0; k < n->count; ++k) ++seen[n->first + k];
        } else {
            for (int c = 0; c < 2; ++c)
                for (int k = 0; k < 3; ++k) {
                    if (t->nodes.items[n->first + (uint32_t)c].lo[k] < n->lo[k] || t->nodes.items[n->first + (uint32_t)c].hi[k] > n->hi[k]) { CHECK(!"a child outside its box"); }
                }
        }
    }
    bool once = true;
    for (size_t i = 0; i < t->triangles.len; ++i) once = once && seen[i] == 1;
    CHECK(once);
    artfFree(seen);

    uint32_t* frame = artfMalloc((size_t)W * H * 4);
    traceWith(s, ARTF_TRACE_PATHS, 3, 0);
    static const struct { int threads; ArtfSimd simd; } runs[] = {{1, ARTF_SIMD_AUTO}, {4, ARTF_SIMD_AUTO}, {1, ARTF_SIMD_PORTABLE}, {3, ARTF_SIMD_128}};
    uint64_t reference = 0;
    for (size_t i = 0; i < ARTF_COUNT(runs); ++i) {
        ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, runs[i].threads, false);
        o.simd = runs[i].simd;
        ArtfRenderer* r = artfRendererNew(&o);
        drawSamples(r, s, 3, frame);
        const uint64_t h = hashOf(frame, (size_t)W * H);
        if (!i) reference = h;
        CHECK(h == reference);
        artfRendererFree(r);
    }
    artfFree(frame);
    artfSceneFree(s);
}

/* The GPU's shader against this C: the same picture, once the noise is gone. */
static void testTraceBothBackends(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (!gpu) return;
    o.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* cpu = artfRendererNew(&o);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    for (int mode = ARTF_TRACE_RAYS; mode <= ARTF_TRACE_PATHS; ++mode)
        for (int clear = 0; clear < 2; ++clear) {
            ArtfScene* s = layeredScene();
            if (clear) artfSceneSetBackgroundAlpha(s, 0);
            traceWith(s, (ArtfTraceMode)mode, 96, 0);
            drawSamples(gpu, s, 96, a);
            drawSamples(cpu, s, 96, b);
            const double far = differentShare(a, b, 24);
            printf("%s, %s: pixels that differ by more than 24 of 255: %.2f%%\n", mode == ARTF_TRACE_RAYS ? "rays" : "paths", clear ? "no background" : "a background", far * 100);
            CHECK(far < .02);
            artfSceneFree(s);
        }
    artfFree(a);
    artfFree(b);
    artfRendererFree(gpu);
    artfRendererFree(cpu);
}

static void testFluid(void) {
    const size_t cells = (size_t)ARTF_FLUID_WIDTH * ARTF_FLUID_HEIGHT;
    const float inner = 3.83f, outer = 34.f, spin = .6f;
    ArtfFluid* a = artfFluidNew(inner, outer, spin, 0);
    ArtfFluid* b = artfFluidNew(inner, outer, spin, 0);
    float* before = artfMalloc(cells * 4);
    memcpy(before, artfFluidField(a), cells * 4);

    /* it moves, the same way however the time is cut up */
    CHECK(artfFluidAdvance(a, 2.0));
    CHECK(artfFluidAdvance(b, 1.0));
    CHECK(artfFluidAdvance(b, 2.0));
    CHECK(memcmp(before, artfFluidField(a), cells * 4) != 0);
    CHECK(memcmp(artfFluidField(a), artfFluidField(b), cells * 4) == 0);
    CHECK(!artfFluidAdvance(a, artfFluidTime(a) + .1));

    /* Each ring goes around at Kerr's orbital speed: the shift that best lays a ring on itself as it
     * was is the one its orbit gives. (Over a little time: turbulence soon makes it another ring.) */
    static const int rows[2] = {90, 140};
    const float* now = artfFluidField(a);
    memcpy(before, now, cells * 4);
    const double start = artfFluidTime(a);
    CHECK(artfFluidAdvance(a, start + 2.05));
    const double passed = artfFluidTime(a) - start;
    now = artfFluidField(a);
    for (int k = 0; k < 2; ++k) {
        const int j = rows[k];
        const float rho = (float)j / (float)(ARTF_FLUID_HEIGHT - 1), r = inner / (1.f - rho * (1.f - inner / outer));
        const double expected = passed / ((double)r * sqrt((double)r) + spin) / 6.2831853 * ARTF_FLUID_WIDTH;
        double best = 1e30;
        int found = 0;
        for (int d = 0; d < ARTF_FLUID_WIDTH; ++d) {
            double sum = 0;
            for (int i = 0; i < ARTF_FLUID_WIDTH; ++i) {
                const double gap = (double)before[(size_t)j * ARTF_FLUID_WIDTH + i] - now[(size_t)j * ARTF_FLUID_WIDTH + ((i + d) & (ARTF_FLUID_WIDTH - 1))];
                sum += gap * gap;
            }
            if (sum < best) { best = sum; found = d; }
        }
        printf("the gas at r = %.1f: moved %d cells, an orbit says %.1f\n", (double)r, found, expected);
        CHECK(fabs((double)found - expected) < 3.0);
    }

    /* a long run: it neither blows up nor dies away */
    double first = 0;
    for (size_t i = 0; i < cells; ++i) first += now[i];
    for (double t = start + 3.0; t < 300.0; t += 3.0) artfFluidAdvance(a, t);
    now = artfFluidField(a);
    double sum = 0;
    bool finite = true;
    for (size_t i = 0; i < cells; ++i) {
        finite = finite && now[i] >= 0.f && now[i] < 100.f;
        sum += now[i];
    }
    printf("what shows of the gas, after a long run: %.2f of what it was\n", sum / first);
    CHECK(finite);
    CHECK(sum > first * .5 && sum < first * 2.);
    artfFree(before);
    artfFluidFree(a);
    artfFluidFree(b);
}

/* A background pass of the program's own: light the same everywhere, and what the scene does with it. */
typedef struct FlatPass { float light; int released, frames, simd; } FlatPass;

static void flatFrame(void* user, const ArtfCamera* camera, int width, int height, void* params) {
    (void)camera;
    FlatPass* p = user;
    ++p->frames;
    float* out = params;
    out[0] = p->light;
    out[1] = (float)width;
    out[2] = (float)height;
}

static void flatRows(void* user, const void* params, const ArtfBackgroundRows* rows) {
    FlatPass* p = user;
    const float* in = params;
    p->simd = rows->simd;
    CHECK(rows->x % 64 == 0 && rows->y % 64 == 0 && rows->stride % 8 == 0);
    CHECK(rows->x + rows->width <= rows->frameWidth && rows->y + rows->height <= rows->frameHeight);
    CHECK(in[1] == (float)rows->frameWidth && in[2] == (float)rows->frameHeight);
    for (int y = 0; y < rows->height; ++y)
        for (int x = 0; x < rows->width; ++x)
            for (int c = 0; c < 3; ++c) rows->light[c][(size_t)y * rows->stride + x] = in[0];
}

static void flatRelease(void* user) { ++((FlatPass*)user)->released; }

static void testBackgroundPass(void) {
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 1, false); /* (one thread: rows checks as it goes) */
    ArtfRenderer* cpu = artfRendererNew(&o);
    ArtfScene* s = plainScene();
    artfSceneSetBackgroundAlpha(s, 0.f);
    FlatPass flat = {.25f, 0, 0, 0}, other = {1.f, 0, 0, 0};
    ArtfBackgroundPass p;
    memset(&p, 0, sizeof p);
    p.user = &flat;
    p.frame = flatFrame;
    p.rows = flatRows;
    p.release = flatRelease;
    p.exposure = 1.f;
    artfSceneSetBackgroundPass(s, &p);
    CHECK(artfSceneBackgroundPass(s) && artfSceneBackgroundPass(s)->user == &flat);
    CHECK(!artfBlackHoleOf(s));
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(cpu, a));
    CHECK(flat.frames == 1 && flat.simd != ARTF_SIMD_AUTO);
    /* the same color everywhere, whatever the background's alpha: the tone curve of .25, with a little noise */
    const double top = .25 * (.25 * 2.51 + .03), bottom = .25 * (.25 * 2.43 + .59) + .14;
    const int expected = (int)(sqrt(top / bottom) * 255.0);
    bool even = true;
    for (size_t i = 0; i < (size_t)W * H; ++i) even = even && a[i] >> 24 == 255 && abs(CHANNEL(a[i], 16) - expected) <= 1 && CHANNEL(a[i], 16) == CHANNEL(a[i], 0);
    CHECK(even);
    /* given again with the same user, nothing is let go; another user, and the first is */
    artfSceneSetBackgroundPass(s, &p);
    CHECK(flat.released == 0);
    p.user = &other;
    artfSceneSetBackgroundPass(s, &p);
    CHECK(flat.released == 1 && other.released == 0);
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(cpu, a));
    CHECK(CHANNEL(a[W * H / 2], 16) > expected + 40);
    /* a view leaves it out */
    artfRendererSetView(cpu, ARTF_VIEW_NORMALS);
    other.frames = 0;
    CHECK(artfRendererDraw(cpu, s));
    CHECK(other.frames == 0);
    artfRendererSetView(cpu, ARTF_VIEW_LIT);
    /* with none, the frame is as it was */
    artfSceneSetBackgroundPass(s, NULL);
    CHECK(other.released == 1);
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(cpu, a));
    CHECK(a[0] == 0);
    /* the scene lets go of it as it goes */
    artfSceneSetBackgroundPass(s, &p);
    artfSceneFree(s);
    CHECK(other.released == 2);
    artfRendererFree(cpu);
    artfFree(a);
}

static void testBlackHole(void) {
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* a_ = a;
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);

    /* The shadow: what general relativity says, and nothing here assumes. Light passing closer than
     * sqrt(27) masses does not come back; one who stays still at r sees that as the angle whose sine
     * is sqrt(27) sqrt(1 - 2 / r) / r. The disc is kept far out, seen along its axis: near in it would
     * stand in front of the shadow, while from there only the images of it that went around the hole
     * come close to that edge, and they all but touch it. */
    static const float distances[3] = {14.f, 30.f, 80.f};
    for (int i = 0; i < 3; ++i) {
        const float r = distances[i];
        ArtfScene* s = holeScene(r, 1.57079633f, 0, 0);
        ArtfBlackHoleDesc far = artfBlackHoleOf(s)->desc;
        far.discInner = 10.f;
        far.discOuter = 60.f;
        far.discGain = 200.f;
        far.samples = 4;
        far.stars = 0;
        artfSceneSetBlackHole(s, &far);
        /* closing in on the hole so that the shadow fills the frame: a pixel is then a small angle */
        const float sine = sqrtf(27.f) * sqrtf(1.f - 2.f / r) / r, angle = asinf(sine);
        ArtfCamera c = s->camera;
        c.fovY = angle * 2.6f;
        artfSceneSetCamera(s, &c);
        const float expected = tanf(angle) / tanf(c.fovY * .5f) * (float)H * .5f;
        const float seen = shadowRadius(cpu, s, a);
        printf("the shadow at %.0f masses: %.1f pixels, %.1f expected\n", (double)r, (double)seen, (double)expected);
        CHECK(fabsf(seen - expected) < 1.5f);
        artfSceneFree(s);
    }

    /* A spinning hole's shadow is not round and not centered. Seen from its equator, its edges are
     * the two circular orbits of light, one with the spin and one against it, at r = 2 (1 + cos(2/3
     * acos(-+a))), and each one's xi says where that lands. Space turns at the eye too. This is held
     * against no more than those formulas, with nothing but the sky to light the edge. */
    static const float spins[3] = {.3f, .6f, .95f};
    for (int i = 0; i < 3; ++i) {
        const double a = spins[i], r = 60.0;
        ArtfScene* s = holeScene((float)r, 0, 0, 0);
        ArtfBlackHoleDesc lit = artfBlackHoleOf(s)->desc;
        lit.spin = spins[i];
        lit.discGain = 0;
        lit.discThickness = 0;
        lit.stars = 1e4f;
        artfSceneSetBlackHole(s, &lit);
        ArtfCamera c = s->camera;
        c.fovY = .36f;
        artfSceneSetCamera(s, &c);
        CHECK(artfRendererDraw(cpu, s));
        CHECK(artfRendererReadPixels(cpu, a_));
        const uint32_t* row = a_ + (size_t)(H / 2) * W;
        int left = W / 2, right = W / 2;
        while (left > 0 && (row[left] & 255) < 40) --left;
        while (right < W - 1 && (row[right] & 255) < 40) ++right;
        /* where each edge should be: a photon of that xi, seen by one at rest in the turning space */
        const double big = (r * r + a * a) * (r * r + a * a) - a * a * (r * r - 2 * r + a * a);
        const double varpi = sqrt(big) / r, alpha = r * sqrt(r * r - 2 * r + a * a) / sqrt(big), turning = 2 * a * r / big;
        double edge[2];
        for (int k = 0; k < 2; ++k) {
            /* with the spin, then against it */
            const double orbit = 2.0 * (1.0 + cos(2.0 / 3.0 * acos(k ? a : -a)));
            const double xi = -(orbit * orbit * orbit - 3 * orbit * orbit + a * a * orbit + a * a) / (a * (orbit - 1.0));
            const double along = xi * alpha / (varpi * (1.0 - turning * xi));
            edge[k] = along / sqrt(1.0 - along * along) / tan(c.fovY * .5) * H * .5;
        }
        printf("spin %.2f: the shadow from %d to %d pixels, %.1f to %.1f expected\n", a, left + 1 - W / 2, right - W / 2, edge[1], edge[0]);
        CHECK(fabs((double)(right - W / 2) - edge[0]) < 2.0);
        CHECK(fabs((double)(left + 1 - W / 2) - edge[1]) < 2.0);
        artfSceneFree(s);
    }

    /* the same pixels on any instruction set, with any number of threads */
    ArtfScene* s = holeScene(26.f, .06f, .5f, 1.f);
    ArtfBlackHoleDesc all = artfBlackHoleOf(s)->desc;
    all.spin = .9f;
    all.bloom = 1.f;
    all.samples = 2;
    all.shutter = 3.f;
    all.velocity = artfVec3(.3f, 0, 0);
    artfSceneSetBlackHole(s, &all);
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(cpu, a));
    const uint64_t expected = hashOf(a, (size_t)W * H);
    const ArtfSimd simds[3] = {ARTF_SIMD_PORTABLE, ARTF_SIMD_128, ARTF_SIMD_256};
    for (int i = 0; i < 3; ++i)
        for (int threads = 1; threads <= 3; threads += 2) {
            ArtfRendererOptions other = options(ARTF_BACKEND_SOFTWARE, threads, false);
            other.simd = simds[i];
            ArtfRenderer* r = artfRendererNew(&other);
            CHECK(artfRendererDraw(r, s));
            CHECK(artfRendererReadPixels(r, b));
            CHECK(hashOf(b, (size_t)W * H) == expected);
            artfRendererFree(r);
        }
    artfSceneFree(s);

    /* The gas goes from +x toward +z, so on the right it comes at an eye on the +z side: that side
     * is brighter, by as much as relativity says. With Doppler off the two sides are alike but for
     * the turbulence, which has no reason to be even. */
    s = holeScene(26.f, .06f, 0, 0);
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(cpu, a));
    const double left = brightness(a, 0, W / 2), right = brightness(a, W / 2, W);
    CHECK(right > 0 && fabs(left - right) / (left + right) < .2);
    artfSceneFree(s);
    s = holeScene(26.f, .06f, 1.f, 0);
    {
        /* (exposed low: a side that has gone white tells nothing of how much brighter it is) */
        ArtfBlackHoleDesc d = artfBlackHoleOf(s)->desc;
        d.exposure = .1f;
        artfSceneSetBlackHole(s, &d);
    }
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(cpu, a));
    printf("Doppler: the side that comes at the eye is %.2f times as bright\n", brightness(a, W / 2, W) / brightness(a, 0, W / 2));
    CHECK(brightness(a, W / 2, W) > brightness(a, 0, W / 2) * 1.5);
    artfSceneFree(s);

    /* time turns the gas, and costs no new map */
    s = holeScene(26.f, .06f, 0, 0);
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(cpu, a));
    const uint64_t version = artfBlackHoleOf(s)->mapVersion;
    ArtfBlackHoleDesc d = artfBlackHoleOf(s)->desc;
    d.time = 40.f;
    artfSceneSetBlackHole(s, &d);
    CHECK(artfBlackHoleOf(s)->mapVersion == version);
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(cpu, b));
    CHECK(hashOf(a, (size_t)W * H) != hashOf(b, (size_t)W * H));

    /* what the scene holds goes in front; with the hole gone, the frame is as it would have been */
    ArtfMaterialDesc red = {artfVec3(0, 0, 0), 0, 0, 1, artfVec3(1, 0, 0)};
    const ArtfVec3 eye = s->camera.position;
    ArtfTransform t = artfTransformAt(0, eye.y * .75f, eye.z * .75f, 1); /* on the way from the eye to the hole */
    artfSceneAddInstance(s, artfSceneAddSphere(s, 1.f, 16, 24), artfSceneAddMaterial(s, &red), &t);
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(cpu, a));
    CHECK((a[(H / 2) * W + W / 2] & 0xFFFFFF) == 0xFF0000);
    CHECK((a[(H / 8) * W + W / 2] & 0xFFFFFF) != 0xFF0000);
    artfSceneSetBlackHole(s, NULL);
    artfSceneSetBackground(s, artfVec3(0, 0, 1));
    CHECK(artfRendererDraw(cpu, s));
    CHECK(artfRendererReadPixels(cpu, a));
    CHECK((a[0] & 0xFFFFFF) == 0x0000FF);
    artfSceneSetBlackHole(s, &d);

    /* new sizes, down to nothing to speak of, and none of them a multiple of a tile or of eight lanes */
    static const int sizes[4][2] = {{W / 2, H / 2}, {101, 67}, {3, 2}, {1, 1}};
    for (int i = 0; i < 4; ++i) {
        artfRendererResize(cpu, sizes[i][0], sizes[i][1]);
        CHECK(artfRendererDraw(cpu, s));
    }
    artfRendererResize(cpu, W, H);
    CHECK(artfRendererDraw(cpu, s));

    /* the GPU draws the same hole, with what is in front of it, with and without occlusion */
    o = options(ARTF_BACKEND_VULKAN, 0, false);
    ArtfRenderer* gpu = artfRendererNew(&o);
    if (gpu) {
        d.doppler = .6f;
        d.bloom = 1.f;
        d.samples = 2;
        d.spin = .8f;
        d.shutter = 2.f;
        d.velocity = artfVec3(.2f, 0, -.1f);
        artfSceneSetBlackHole(s, &d);
        CHECK(artfRendererDraw(cpu, s));
        CHECK(artfRendererReadPixels(cpu, b));
        for (int occlusion = 0; occlusion < 2; ++occlusion) {
            artfSceneSetFeature(s, ARTF_FEATURE_OCCLUSION, occlusion != 0);
            CHECK(artfRendererDraw(gpu, s));
            CHECK(artfRendererDraw(gpu, s));
            CHECK(artfRendererDraw(gpu, s));
            CHECK(artfRendererReadPixels(gpu, a));
            const double far = differentShare(a, b, 24);
            printf("black hole, pixels that differ by more than 24 of 255: %.2f%%\n", far * 100);
            CHECK(far < .01);
            CHECK(differentShare(a, b, 96) < .001);
        }
        /* new sizes, and a hole that goes and comes back */
        for (int i = 0; i < 4; ++i) {
            artfRendererResize(gpu, sizes[i][0], sizes[i][1]);
            CHECK(artfRendererDraw(gpu, s));
        }
        artfRendererResize(gpu, W, H);
        artfSceneSetBlackHole(s, NULL);
        CHECK(artfRendererDraw(gpu, s));
        artfSceneSetBlackHole(s, &d);
        CHECK(artfRendererDraw(gpu, s));
        CHECK(artfRendererDraw(gpu, s));
        CHECK(artfRendererDraw(gpu, s));
        CHECK(artfRendererReadPixels(gpu, a));
        CHECK(differentShare(a, b, 24) < .01);
        /* another disc goes up */
        d.discOuter = 12.f;
        artfSceneSetBlackHole(s, &d);
        CHECK(artfRendererDraw(gpu, s));
        CHECK(artfRendererDraw(gpu, s));
        CHECK(artfRendererDraw(gpu, s));
        CHECK(artfRendererDraw(cpu, s));
        CHECK(artfRendererReadPixels(gpu, a));
        CHECK(artfRendererReadPixels(cpu, b));
        CHECK(differentShare(a, b, 24) < .01);
        artfRendererFree(gpu);
    }
    artfSceneFree(s);
    artfRendererFree(cpu);
    artfFree(a);
    artfFree(b);
}

int main(void) {
    /* (a feature asked for is drawn on the next frame, as each test expects; testShadersAhead checks the other way) */
    artfVulkanShadersWait = true;
    testPacking();
    testMeshlets();
    testInstances();
    testSamePixels();
    testNoGaps();
    testClipping();
    testCulling();
    testOverlay();
    testResize();
    testSimplify();
    testLod();
    testOcclusion(ARTF_BACKEND_SOFTWARE);
    testOcclusionUnderOverlay();
    testOcclusion(ARTF_BACKEND_VULKAN);
    testVulkan();
    testColorData();
    testLayers(ARTF_BACKEND_SOFTWARE);
    testLayers(ARTF_BACKEND_VULKAN);
    testMaterials(ARTF_BACKEND_SOFTWARE, false);
    testMaterials(ARTF_BACKEND_SOFTWARE, true);
    testMaterials(ARTF_BACKEND_VULKAN, false);
    testMaterials(ARTF_BACKEND_VULKAN, true);
    testSamePixelsOfLayers();
    testAntialias(ARTF_BACKEND_SOFTWARE);
    testAntialias(ARTF_BACKEND_VULKAN);
    testLayersBothBackends();
    testPictures();
    testTraceData();
    testTrace(ARTF_BACKEND_SOFTWARE);
    testTrace(ARTF_BACKEND_VULKAN);
    testTraceBothBackends();
    testHandles();
    testLook(ARTF_BACKEND_SOFTWARE);
    testLook(ARTF_BACKEND_VULKAN);
    testLifetimes();
    testPhysical(ARTF_BACKEND_SOFTWARE);
    testPhysical(ARTF_BACKEND_VULKAN);
    testShadowBytes();
    testShadows(ARTF_BACKEND_SOFTWARE);
    testShadows(ARTF_BACKEND_VULKAN);
    testShadowsBothBackends();
    testGltf();
    testClips();
    testSkin(ARTF_BACKEND_SOFTWARE);
    testSkin(ARTF_BACKEND_VULKAN);
    testSkinBothBackends();
    testMorph(ARTF_BACKEND_SOFTWARE);
    testMorph(ARTF_BACKEND_VULKAN);
    testDenoiseBytes();
    testDenoise(ARTF_BACKEND_SOFTWARE);
    testDenoise(ARTF_BACKEND_VULKAN);
    testDenoiseChange(ARTF_BACKEND_SOFTWARE);
    testDenoiseChange(ARTF_BACKEND_VULKAN);
    testDenoiseMotion(ARTF_BACKEND_SOFTWARE);
    testDenoiseMotion(ARTF_BACKEND_VULKAN);
    testDenoiseBones(ARTF_BACKEND_SOFTWARE);
    testDenoiseBones(ARTF_BACKEND_VULKAN);
    testOcclusionAuto(ARTF_BACKEND_SOFTWARE);
    testOcclusionAuto(ARTF_BACKEND_VULKAN);
    testShadowCache(ARTF_BACKEND_SOFTWARE);
    testShadowCache(ARTF_BACKEND_VULKAN);
    testShadowRegions(ARTF_BACKEND_SOFTWARE);
    testShadowRegions(ARTF_BACKEND_VULKAN);
    testLodFade(ARTF_BACKEND_SOFTWARE);
    testLodFade(ARTF_BACKEND_VULKAN);
    testGpuMeshlets();
    testLayersBehindSolid();
    testLayersSorted(ARTF_BACKEND_SOFTWARE);
    testLayersSorted(ARTF_BACKEND_VULKAN);
    testBlockDecoders();
    testStreamedTexture(ARTF_BACKEND_SOFTWARE);
    testStreamedTexture(ARTF_BACKEND_VULKAN);
    testResidencyGraph(ARTF_BACKEND_SOFTWARE);
    testResidencyGraph(ARTF_BACKEND_VULKAN);
    testMeshPlaces(ARTF_BACKEND_SOFTWARE);
    testMeshPlaces(ARTF_BACKEND_VULKAN);
    testSlerp();
    testGltfSparse();
    testGltfTargets();
    testGltfExtensions();
    testGltfMeshopt();
    testEnvironment();
    testFloats();
    testGrading();
    testCoat();
    testLightShapes();
    testProfiles();
    testMoreMaterials();
    testGltfMore();
    testSky();
    testSkyMore();
    testLookMore();
    testSizedLights();
    testMotion();
    testUpscale();
    testSkinnedMotion();
    testBlurEdges();
    testSoftShadowsTurning();
    testTemporalBytes();
    testSoftShadows();
    testProbeReflections();
    testCullSize();
    testParticleForces();
    testTerrainLayers();
    testHlod();
    testSkinLod();
    testSwarm();
    testMeshStreaming(ARTF_BACKEND_SOFTWARE);
    testMeshStreaming(ARTF_BACKEND_VULKAN);
    testTraceHardware();
    testViews();
    testShadersAhead();
    testPostKernels();
    testPassTimes();
    testOrthographic();
    testPick();
    testTracedShadows();
    testTracedReflections();
    testTracedMoved();
    testProbesWalls();
    testExtraShadows();
    testFloatsSamples();
    testDecals();
    testExtras();
    testLostDevice();
    testRefusedSwapchain();
    testBlocks();
    testPrepared();
    testRemoving(ARTF_BACKEND_SOFTWARE);
    testRemoving(ARTF_BACKEND_VULKAN);
    testResidency(ARTF_BACKEND_SOFTWARE);
    testResidency(ARTF_BACKEND_VULKAN);
    testGraphs(ARTF_BACKEND_SOFTWARE);
    testGraphs(ARTF_BACKEND_VULKAN);
    testGraphsBothBackends();
    testFluid();
    testBackgroundPass();
    testBlackHole();
#ifdef ARTFUL_COUNT_ALLOCATIONS
    printf("allocations still alive: %ld\n", artfAllocationsAlive());
    CHECK(artfAllocationsAlive() == 0);
#endif
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
