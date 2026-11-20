/* artful-tests: what the tests measure pictures with (tools/flip.c). */
#include "tests.h"
#include "scene/sampling.h"

static void fill(float* p, int n, float r, float g, float b) {
    for (int i = 0; i < n; ++i) {
        p[i * 3] = r;
        p[i * 3 + 1] = g;
        p[i * 3 + 2] = b;
    }
}

/* FLIP as its paper defines it, on pictures whose answer is known: alike is 0; green against blue, FLIP's largest
 * color difference, is 1 with no edge to add; an edge one picture has and the other not costs where it is and not
 * far from it; a step of a byte is next to nothing; and light past 1 goes through HDR-FLIP's exposures. */
void testFlip(void) {
    enum { FW = 64, FH = 48, N = FW * FH };
    float* a = artfMalloc(sizeof(float) * 3 * N);
    float* b = artfMalloc(sizeof(float) * 3 * N);
    float* map = artfMalloc(sizeof(float) * N);

    fill(a, N, .3f, .5f, .2f);
    FlipStats s = flipLinear(a, a, FW, FH, 0, NULL);
    CHECK(s.mean == 0 && s.max == 0);

    fill(a, N, 0, 1, 0);
    fill(b, N, 0, 0, 1);
    s = flipLinear(a, b, FW, FH, 0, NULL);
    CHECK(fabs(s.mean - 1) < 1e-4 && fabs(s.max - 1) < 1e-4);

    /* black against white: as far as their lightness, short of green against blue */
    fill(a, N, 0, 0, 0);
    fill(b, N, 1, 1, 1);
    s = flipLinear(a, b, FW, FH, 0, NULL);
    CHECK(s.mean > .5 && s.mean < 1);
    const double blackWhite = s.mean;
    s = flipLinear(b, a, FW, FH, 0, NULL);
    CHECK(fabs(s.mean - blackWhite) < 1e-5); /* (alike either way round, for colors this far apart) */

    /* a white bar on grey that only one has: an error over the bar, nothing far from it */
    fill(a, N, .2f, .2f, .2f);
    memcpy(b, a, sizeof(float) * 3 * N);
    for (int y = 20; y < 28; ++y)
        for (int x = 0; x < FW; ++x) b[(y * FW + x) * 3] = b[(y * FW + x) * 3 + 1] = b[(y * FW + x) * 3 + 2] = 1;
    s = flipLinear(a, b, FW, FH, 0, map);
    CHECK(map[24 * FW + 32] > .5f && map[2 * FW + 32] < .02f && s.max > .5);

    /* a step of a byte, as two GPUs round: far below what the eye tells */
    uint32_t* fa = artfMalloc(sizeof(uint32_t) * N);
    uint32_t* fb = artfMalloc(sizeof(uint32_t) * N);
    for (int i = 0; i < N; ++i) {
        fa[i] = 0xFF000000u | (uint32_t)(i % 200 + 20) * 0x010101u;
        fb[i] = fa[i] + (i & 1 ? 0x010101u : 0);
    }
    s = flipFrames(fa, fb, FW, FH, 0, NULL);
    CHECK(s.mean > 0 && s.mean < .05); /* (.019 here: a pattern a pixel wide, which FLIP's edges see a little) */
    printf("FLIP: black against white %.4f, a bar that one has %.4f at it, a byte apart %.5f\n", blackWhite, map[24 * FW + 32], s.mean);

    /* in light past 1: alike is 0, and a sun twice as bright is told apart */
    fill(a, N, .5f, .5f, .5f);
    for (int i = 0; i < 40; ++i) a[(i * 37 % N) * 3] = a[(i * 37 % N) * 3 + 1] = a[(i * 37 % N) * 3 + 2] = 40;
    memcpy(b, a, sizeof(float) * 3 * N);
    s = flipHdr(a, b, FW, FH, 0, NULL);
    CHECK(s.max == 0);
    for (int i = 0; i < 40; ++i) b[(i * 37 % N) * 3] = b[(i * 37 % N) * 3 + 1] = b[(i * 37 % N) * 3 + 2] = 80;
    s = flipHdr(a, b, FW, FH, 0, NULL);
    CHECK(s.max > .1);

    artfFree(fa);
    artfFree(fb);
    artfFree(a);
    artfFree(b);
    artfFree(map);
}

/* What frames are made of: sRGB, by core/color.h's polynomials. Every byte goes to light and back to itself; both
 * ways are within 1e-5 of the curve the standard defines (worked out here in doubles, with pow); and the curve is
 * monotonic, so that no lighter light shows darker. */
void testSrgb(void) {
    bool back = true, rising = true;
    double worstDecode = 0, worstEncode = 0;
    float before = -1;
    for (int b = 0; b < 256; ++b) {
        const float shown = (float)b / 255.f, light = artfSrgbDecode(shown);
        const double exact = shown <= .04045 ? shown / 12.92 : pow((shown + .055) / 1.055, 2.4);
        worstDecode = fmax(worstDecode, fabs(light - exact));
        back = back && (int)(artfSrgbEncode(light) * 255.f + .5f) == b;
        rising = rising && light > before;
        before = light;
    }
    before = -1;
    for (int i = 0; i <= 100000; ++i) {
        const float light = (float)i / 100000.f, shown = artfSrgbEncode(light);
        const double exact = light <= .0031308 ? light * 12.92 : 1.055 * pow(light, 1 / 2.4) - .055;
        worstEncode = fmax(worstEncode, fabs(shown - exact));
        rising = rising && shown >= before;
        before = shown;
    }
    printf("sRGB: within %.2g of the curve decoding, %.2g encoding\n", worstDecode, worstEncode);
    CHECK(back && rising && worstDecode < 1e-5 && worstEncode < 1e-5);
    CHECK(artfSrgbEncode(-1.f) == 0 && artfSrgbDecode(0) == 0);
}

/* Where a path's samples fall (scene/sampling.h): Sobol's points, scrambled by Owen's way for each pixel. Scrambling
 * keeps what makes them good: the first 2^m of each pixel put one in each 2^-m of each dimension, and the first two
 * dimensions one in each of the 2^m boxes of every shape; two pixels' sequences are not the same; and the numbers are
 * Sobol's own where nothing scrambles them. */
void testSobol(void) {
    bool spread = true;
    for (uint32_t pixel = 0; pixel < 16; ++pixel)
        for (int m = 1; m <= 10; ++m) {
            const uint32_t n = 1u << m, seed = artfPathSeed(pixel * 7919u, 3);
            static uint8_t seen[4][1024], boxes[11][1024];
            memset(seen, 0, sizeof seen);
            memset(boxes, 0, sizeof boxes);
            for (uint32_t i = 0; i < n; ++i) {
                float u[4];
                artfSobol4(i, seed, u);
                for (int d = 0; d < 4; ++d) {
                    CHECK(u[d] >= 0 && u[d] < 1);
                    const uint32_t cell = (uint32_t)(u[d] * (float)n);
                    spread = spread && !seen[d][cell];
                    seen[d][cell] = 1;
                }
                for (int k = 0; k <= m; ++k) {
                    const uint32_t bx = (uint32_t)(u[0] * (float)(1u << k)), by = (uint32_t)(u[1] * (float)(1u << (m - k)));
                    const uint32_t box = bx * (1u << (m - k)) + by;
                    spread = spread && !boxes[k][box];
                    boxes[k][box] = 1;
                }
            }
        }
    CHECK(spread);
    float a[4], b[4];
    artfSobol4(5, artfPathSeed(1, 0), a);
    artfSobol4(5, artfPathSeed(2, 0), b);
    CHECK(a[0] != b[0] && a[1] != b[1]);
    /* unscrambled (Owen's way with nothing to undo it is not the identity, so by the matrices): Joe and Kuo's third
     * dimension at 1..4 is 1/2, 3/4, 1/4, 3/8 */
    const float third[4] = {.5f, .75f, .25f, .375f};
    for (uint32_t i = 1; i <= 4; ++i) {
        uint32_t x = 0;
        for (int k = 0; k < ARTF_SOBOL_BITS; ++k)
            if (i >> k & 1u) x ^= artfSobolMatrix[2][k];
        CHECK((float)x / 4294967296.f == third[i - 1]);
    }
}
