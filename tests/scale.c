/* artful-tests: particles, terrain, many as one, emitters of many. */
#include "tests.h"

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

void testParticleForces(void) {
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
void testTerrainLayers(void) {
    static float heights[65 * 65];
    for (int z = 0; z < 65; ++z)
        for (int x = 0; x < 65; ++x) {
            const float dx = (float)(x - 32) / 32.f, dz = (float)(z - 32) / 32.f, r2 = dx * dx + dz * dz;
            heights[z * 65 + x] = r2 < 1 ? (1.f - r2) * (1.f - r2) : 0.f;
        }
    uint32_t* a[2] = {artfCalloc(W * H, 4), artfCalloc(W * H, 4)}; /* (zeros: where Vulkan does not run, the GPU's stays so) */
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
/* ---- scale: many as one, levels of what bends, emitters of many, meshes that come and go ---- */

static ArtfScene* groveScene(void) {
    ArtfScene* s = artfSceneNew();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true); /* (whatever ARTFUL_FEATURES says) */
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
void testHlod(void) {
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
void testSkinLod(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    o.backend = ARTF_BACKEND_VULKAN;
    ArtfRenderer* gpu = artfRendererNew(&o);
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    uint32_t* c = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true); /* (whatever ARTFUL_FEATURES says) */
    artfSceneSetLodError(s, 8); /* (so that the first level comes where the tube still shows) */
    const ArtfMesh tube = addTube(s);
    const ArtfMeshData* source = &s->meshes.items[artfMeshIndexOf(s, tube)];
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

/* What bends, drawn at a lighter level, is met by rays as its whole, as the trees hold whole meshes: picking finds it
 * where its pose put it; its shadow from rays is as with the levels off, on the CPU and on Vulkan, through the trees
 * and the ray tracing hardware, whose bottom level of the whole is refitted as the pose moves. And the copies of its
 * levels, posed, write nothing over the trees' other meshes (they once wrote over mesh 0's vertices). */
void testTracedSkinLod(void) {
    ArtfRendererOptions o = options(ARTF_BACKEND_SOFTWARE, 0, false);
    ArtfRenderer* cpu = artfRendererNew(&o);
    o.backend = ARTF_BACKEND_VULKAN;
    ArtfRenderer* gpus[2] = {artfRendererNew(&o), gpuWithoutRayQueries(&o)};
    uint32_t* a = artfMalloc((size_t)W * H * 4);
    uint32_t* b = artfMalloc((size_t)W * H * 4);
    uint32_t* c = artfMalloc((size_t)W * H * 4);
    ArtfScene* s = plainScene();
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true);
    artfSceneSetLodError(s, 32);
    artfSceneSetSun(s, artfVec3(.5f, -1, .3f), artfVec3(1.2f, 1.2f, 1.2f));
    artfSceneSetAmbient(s, artfVec3(.1f, .1f, .1f), artfVec3(.1f, .1f, .1f));
    /* mesh 0, a cube far from all else: what the copies of the levels wrote over */
    const ArtfMaterial white = whiteMaterial(s);
    addAt(s, artfSceneAddCube(s, .5f), white, 40, 0, 0, 1);
    CHECK(s->instances.items[0].mesh == 0);
    addAt(s, artfSceneAddPlane(s, 40, 1), white, 0, -2, 0, 1);
    const ArtfMesh tube = addTube(s);
    const ArtfInstance i = addAt(s, tube, white, 0, -2, 0, 1);
    const ArtfInstanceData* d = &s->instances.items[2];
    const uint32_t home = d->mesh;
    const float error = s->meshes.items[artfMeshIndexOf(s, tube)].skinLodErrors[0];
    const float far = error * (float)H * .5f / tanf(.5f) / 32.f + 2.5f;
    setCamera(s, 0, .5f * far, .9f * far, 0, -1.5f, 0);
    const ArtfShadowDesc traced = {0, 0, false, true};
    artfSceneSetShadows(s, &traced);
    bendTube(s, i, 1.2f);
    drawInto(cpu, s, a);
    const uint32_t level = d->mesh;
    CHECK(level != home && s->meshes.items[level].home == home + 1);
    /* picking: the bent top, not where it is at rest; and the cube as it was made */
    ArtfPickHit hit;
    CHECK(artfSceneRaycast(s, artfVec3(-1.5f * sinf(1.2f), 1.5f * cosf(1.2f), 5), artfVec3(0, 0, -1), 0, &hit) && hit.instance == i);
    CHECK(!artfSceneRaycast(s, artfVec3(0, 1.5f, 5), artfVec3(0, 0, -1), 0, &hit));
    CHECK(artfSceneRaycast(s, artfVec3(40.2f, 5, .1f), artfVec3(0, -1, 0), 0, &hit) && fabsf(hit.position.y - .25f) < 1e-4f && hit.normal.y > .999f);
    for (int k = 0; k < 6; ++k) {
        bendTube(s, i, .2f * (float)k);
        drawInto(cpu, s, c);
        CHECK(d->mesh != home);
    }
    CHECK(artfSceneRaycast(s, artfVec3(40.2f, 5, .1f), artfVec3(0, -1, 0), 0, &hit) && fabsf(hit.position.y - .25f) < 1e-4f && hit.normal.y > .999f);
    CHECK(artfSceneRaycast(s, artfVec3(39.9f, 0, 5), artfVec3(0, 0, -1), 0, &hit) && fabsf(hit.position.z - .25f) < 1e-4f && hit.normal.z > .999f);
    /* the shadow on the floor: as the whole's, with the levels off, and far from none. (The tube itself, a level much
     * lighter than the whole at 32 pixels, is shaded by the whole: limits.md) */
    uint32_t* e = artfMalloc((size_t)W * H * 4);
    bendTube(s, i, 1.2f);
    drawInto(cpu, s, a);
    CHECK(d->mesh == level);
    artfSceneSetShadows(s, NULL);
    drawInto(cpu, s, c);
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, false);
    drawInto(cpu, s, e);
    artfSceneSetShadows(s, &traced);
    drawInto(cpu, s, b);
    CHECK(d->mesh == home);
    int floor = 0, shadow = 0, apart = 0;
    for (int p = 0; p < W * H; ++p) {
        /* (the floor, unshadowed, as either draws it: not the tube) */
        if (c[p] != c[W * H - 1] || e[p] != c[W * H - 1]) continue;
        int dark = 0, off = 0;
        for (int k = 0; k < 24; k += 8) {
            dark = artfMaxi(dark, abs(CHANNEL(a[p], k) - CHANNEL(c[p], k)));
            off = artfMaxi(off, abs(CHANNEL(a[p], k) - CHANNEL(b[p], k)));
        }
        ++floor;
        shadow += dark > 24;
        apart += off > 24;
    }
    printf("what bends, drawn at a lighter level: of %d pixels of floor, %d in its shadow from rays, %d apart from the whole's\n", floor, shadow, apart);
    CHECK(floor > W * H / 2 && shadow > 400 && apart <= shadow / 50);
    artfFree(e);
    artfSceneSetFeature(s, ARTF_FEATURE_LOD, true);
#ifdef ARTFUL_VULKAN /* (the GPU's: what its counters say) */
    for (int g = 0; g < 2; ++g) {
        if (!gpus[g]) continue;
        bendTube(s, i, 1.2f);
        drawInto(cpu, s, a);
        CHECK(d->mesh == level);
        const uint64_t before[2] = {artfVulkanBottomLevels[0], artfVulkanBottomLevels[1]};
        drawInto(gpus[g], s, b);
        const bool hardware = artfVulkanBottomLevels[0] > before[0];
        if (!g && !hardware) missing("rt");
        const double first = differentShare(a, b, 24);
        /* posed again: the whole refitted, with the hardware, and only it */
        bendTube(s, i, .9f);
        drawInto(cpu, s, a);
        const uint64_t was[2] = {artfVulkanBottomLevels[0], artfVulkanBottomLevels[1]};
        drawInto(gpus[g], s, b);
        CHECK(d->mesh == level);
        const double second = differentShare(a, b, 24);
        printf("what bends, drawn at a lighter level, its shadow from rays on the GPU%s against the CPU: %.3f%%, posed again %.3f%% of pixels apart\n",
               g ? " without ray queries" : hardware ? " through the ray tracing hardware" : "", first * 100, second * 100);
        CHECK(first < .005 && second < .005);
        if (hardware) CHECK(artfVulkanBottomLevels[0] + artfVulkanBottomLevels[1] - was[0] - was[1] == 1);
        if (g) CHECK(artfVulkanBottomLevels[0] == was[0] && artfVulkanBottomLevels[1] == was[1]);
    }
#else
    missing("vulkan");
#endif
    artfSceneSetShadows(s, NULL);
    artfSceneRemoveInstance(s, i);
    CHECK(artfSceneRemoveMesh(s, tube));
    artfSceneFree(s);
    artfFree(a);
    artfFree(b);
    artfFree(c);
    artfRendererFree(cpu);
    for (int g = 0; g < 2; ++g)
        if (gpus[g]) artfRendererFree(gpus[g]);
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

/* Emitters of many: the same particles on any number of workers, as many alive as said, none through the ground; the
 * GPU's compute shader moves them as the CPU does, steps that wait included, and both draw the same; 100,000 of them
 * draw. */
void testSwarm(void) {
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

/* Particles and terrain: made of the scene's own parts, so the backends draw them alike. */
void testExtras(void) {
    ArtfScene* s = artfSceneNew();
    setCamera(s, 0, 1, 6, 0, 0, 0);
    static float heights[33 * 33];
    for (int z = 0; z < 33; ++z)
        for (int x = 0; x < 33; ++x) heights[z * 33 + x] = (float)((x - 16) * (x - 16) + (z - 16) * (z - 16)) / 512.f;
    const ArtfMesh hill = artfSceneAddTerrain(s, heights, 33, 33, .25f, 2.f, 1.f);
    CHECK(hill != 0);
    CHECK(s->meshes.items[artfMeshIndexOf(s, hill)].vertexCount == 33 * 33 && s->meshes.items[artfMeshIndexOf(s, hill)].indexCount == 32 * 32 * 6);
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
