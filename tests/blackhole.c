/* artful-tests: a fluid, a background pass and the black hole (extensions/blackhole). */
#include "tests.h"

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
void testFluid(void) {
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

void testBackgroundPass(void) {
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

void testBlackHole(void) {
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
