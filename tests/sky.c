/* artful-tests: the sky, and light from all around from a picture. */
#include "tests.h"

/* Light from all around, from a picture: the maps, a Radiance file, and both backends drawing the same. */
void testEnvironment(void) {
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

/* A sky from the sun: blue overhead by day, red toward a low sun, the sun reddened and then gone; the same on both
 * backends. Fog by height: thinner above. */
void testSky(void) {
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
    const ArtfPostDesc fog = {.screen.volumetric = 1, .screen.scattering = .3f};
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

void testSkyMore(void) {
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
