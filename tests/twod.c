/* artful-tests: drawing in 2D (src/2d): shapes and their edges, textures, lights and normal maps, soft shadows, gi,
 * bloom, the same bytes on any number of threads, handles, picking, and a 2D frame over a 3D one. */
#include "tests.h"
#include "artful/2d.h"
#include "2d/internal2d.h"

#define W2 256
#define H2 160

static ArtfRenderer2D* renderer2D(int threads) {
    ArtfRenderer2DOptions o = {.width = W2, .height = H2, .threads = threads, .backend = ARTF_BACKEND2D_SOFTWARE};
    return artfRenderer2DNew(&o);
}

/* A scene whose world is the frame's pixels: the camera's middle is the frame's. */
static ArtfScene2D* pixelScene(void) {
    ArtfScene2D* s = artfScene2DNew();
    ArtfCamera2D c = {.center = {W2 * .5f, H2 * .5f}, .zoom = 1};
    artfScene2DSetCamera(s, &c);
    artfScene2DSetBackground(s, artfVec3(0, 0, 0), 1);
    return s;
}

static ArtfSprite2D addRect(ArtfScene2D* s, float x, float y, float w, float h, ArtfVec3 color, bool unlit) {
    ArtfSprite2DDesc d = artfSprite2DDefaults();
    d.position = artfVec2(x, y);
    d.size = artfVec2(w, h);
    d.color = color;
    d.unlit = unlit;
    return artfScene2DAddSprite(s, &d);
}

static uint32_t pixelAt(ArtfRenderer2D* r, int x, int y) { return artfRenderer2DPixels(r)[(size_t)y * W2 + x]; }
static int redAt(ArtfRenderer2D* r, int x, int y) { return CHANNEL(pixelAt(r, x, y), 16); }
static int greenAt(ArtfRenderer2D* r, int x, int y) { return CHANNEL(pixelAt(r, x, y), 8); }

/* atan2 of additions and a division: within 2e-5 all around. */
void test2DAtan(void) {
    double worst = 0;
    for (int i = 0; i < 4096; ++i) {
        const double a = (double)i / 4096. * 6.283185307179586 - 3.141592653589793;
        for (int k = 1; k < 4; ++k) {
            const float x = (float)(cos(a) * k * 7.3), y = (float)(sin(a) * k * 7.3);
            double e = fabs((double)artf2DAtan2(y, x) - atan2((double)y, (double)x));
            if (e > 3.14159) e = fabs(e - 6.283185307179586);
            if (e > worst) worst = e;
        }
    }
    CHECK(worst < 2e-5);
    CHECK(artf2DAtan2(0, 0) == 0);
}

/* Shapes: where they are, with edges a pixel wide, in their layers' order; unlit, as they are. */
void test2DShapes(void) {
    ArtfRenderer2D* r = renderer2D(0);
    ArtfScene2D* s = pixelScene();
    addRect(s, 64, 64, 40, 20, artfVec3(1, 0, 0), true);
    /* a green circle on top of it, added first but in a higher layer */
    ArtfSprite2DDesc c = artfSprite2DDefaults();
    c.shape = ARTF_SHAPE_ELLIPSE;
    c.position = artfVec2(84, 64);
    c.size = artfVec2(20, 20);
    c.color = artfVec3(0, 1, 0);
    c.unlit = true;
    c.layer = 1;
    artfScene2DAddSprite(s, &c);
    /* a triangle, and a line */
    const ArtfVec2 tri[3] = {{0, -20}, {20, 20}, {-20, 20}};
    ArtfSprite2DDesc p = artfSprite2DDefaults();
    p.shape = ARTF_SHAPE_POLYGON;
    p.position = artfVec2(180, 60);
    p.points = tri;
    p.pointCount = 3;
    p.color = artfVec3(0, 0, 1);
    p.unlit = true;
    artfScene2DAddSprite(s, &p);
    const ArtfVec2 ends[2] = {{-30, 0}, {30, 0}};
    ArtfSprite2DDesc l = artfSprite2DDefaults();
    l.shape = ARTF_SHAPE_LINE;
    l.position = artfVec2(128, 130);
    l.points = ends;
    l.pointCount = 2;
    l.thickness = 4;
    l.unlit = true;
    artfScene2DAddSprite(s, &l);
    artfRenderer2DDraw(r, s);

    CHECK(redAt(r, 50, 64) == 255 && greenAt(r, 50, 64) == 0);
    CHECK(greenAt(r, 84, 64) == 255 && redAt(r, 84, 64) == 0); /* (the circle is over the rect) */
    CHECK(pixelAt(r, 20, 20) == 0xFF000000u);
    /* the rect's left edge is at x 44: pixel 43 is out, 44 in; its top at y 54 */
    CHECK(redAt(r, 43, 64) == 0 && redAt(r, 44, 64) == 255);
    CHECK(redAt(r, 50, 53) == 0 && redAt(r, 50, 54) == 255);
    /* an edge between pixels is half: sqrt(.5) of 255 */
    ArtfSprite2DDesc half = artfSprite2DDefaults();
    half.position = artfVec2(30.5f, 140);
    half.size = artfVec2(10, 10);
    half.unlit = true;
    ArtfSprite2D h = artfScene2DAddSprite(s, &half);
    artfRenderer2DDraw(r, s);
    CHECK(near_(redAt(r, 25, 140), 180, 2) && redAt(r, 26, 140) == 255);
    artfScene2DRemoveSprite(s, h);
    /* the triangle: its middle blue, outside it not; its slanted edges partly */
    CHECK(CHANNEL(pixelAt(r, 180, 65), 0) == 255 && CHANNEL(pixelAt(r, 165, 45), 0) == 0);
    int partial = 0;
    for (int x = 160; x < 200; ++x) {
        const int b = CHANNEL(pixelAt(r, x, 60), 0);
        partial += b > 0 && b < 255;
    }
    CHECK(partial == 2);
    /* the line: 4 wide, round ends */
    CHECK(redAt(r, 128, 130) == 255 && redAt(r, 128, 127) == 0 && redAt(r, 128, 133) == 0);
    CHECK(redAt(r, 99, 130) == 255 && redAt(r, 95, 130) == 0);
    artfScene2DFree(s);
    artfRenderer2DFree(r);
}

/* Textures: their texels where they are, nearest or blended, from a frame's premultiplied bytes too; mipmaps when
 * smaller. */
void test2DTextures(void) {
    ArtfRenderer2D* r = renderer2D(0);
    ArtfScene2D* s = pixelScene();
    const uint32_t checker[4] = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu};
    ArtfTexture2DDesc td = {.pixels = checker, .width = 2, .height = 2};
    const ArtfTexture2D t = artfScene2DAddTexture(s, &td);
    ArtfSprite2DDesc d = artfSprite2DDefaults();
    d.position = artfVec2(64, 64);
    d.size = artfVec2(64, 64);
    d.texture = t;
    d.nearest = true;
    d.unlit = true;
    const ArtfSprite2D sp = artfScene2DAddSprite(s, &d);
    artfRenderer2DDraw(r, s);
    CHECK(pixelAt(r, 40, 40) == 0xFFFF0000u && pixelAt(r, 90, 40) == 0xFF00FF00u);
    CHECK(pixelAt(r, 40, 90) == 0xFF0000FFu && pixelAt(r, 90, 90) == 0xFFFFFFFFu);
    /* blended: the middle is all four, in light */
    d.nearest = false;
    artfSprite2DSet(s, sp, &d);
    artfRenderer2DDraw(r, s);
    const uint32_t mid = pixelAt(r, 64, 64);
    CHECK(near_(CHANNEL(mid, 16), (int)(sqrtf(.5f) * 255.f + .5f), 3) && near_(CHANNEL(mid, 8), CHANNEL(mid, 16), 4));
    /* a half transparent texel from a frame (premultiplied): the same light as from a straight one */
    const uint32_t straight = 0x80FF0000u, premultiplied = 0x80800000u;
    ArtfTexture2DDesc a = {.pixels = &straight, .width = 1, .height = 1}, b = {.pixels = &premultiplied, .width = 1, .height = 1, .premultiplied = true};
    const ArtfTexture2D ta = artfScene2DAddTexture(s, &a), tb = artfScene2DAddTexture(s, &b);
    CHECK(artf2DTexture(s, ta)->texels[0][0] == artf2DTexture(s, tb)->texels[0][0]);
    /* mipmaps: 64 texels of stripes, drawn 8 pixels wide, average out */
    uint32_t stripes[64 * 64];
    for (int i = 0; i < 64 * 64; ++i) stripes[i] = (i & 1) ? 0xFFFFFFFFu : 0xFF000000u;
    ArtfTexture2DDesc sd = {.pixels = stripes, .width = 64, .height = 64};
    d.texture = artfScene2DAddTexture(s, &sd);
    d.position = artfVec2(200, 64);
    d.size = artfVec2(8, 8);
    artfScene2DAddSprite(s, &d);
    artfRenderer2DDraw(r, s);
    CHECK(near_(redAt(r, 200, 64), (int)(sqrtf(.5f) * 255.f + .5f), 3));
    CHECK(artf2DTexture(s, d.texture)->levels == 7);
    artfScene2DFree(s);
    artfRenderer2DFree(r);
}

/* A floor, lit by one light: brightest under it, as bright as its light there, nothing past its range; a spot only in
 * its cone; a normal map turned to the light brighter than one turned away. */
void test2DLights(void) {
    ArtfRenderer2D* r = renderer2D(0);
    ArtfScene2D* s = pixelScene();
    addRect(s, W2 * .5f, H2 * .5f, W2, H2, artfVec3(1, 1, 1), false);
    ArtfLight2DDesc l = artfLight2DDefaults();
    l.position = artfVec2(64, 80);
    l.range = 60;
    l.height = 10;
    const ArtfLight2D light = artfScene2DAddLight(s, &l);
    artfRenderer2DDraw(r, s);
    CHECK(near_(redAt(r, 64, 80), 255, 1));
    CHECK(redAt(r, 64, 80) > redAt(r, 84, 80) && redAt(r, 84, 80) > redAt(r, 104, 80) && redAt(r, 124, 80) == 0);
    CHECK(redAt(r, 43, 80) == redAt(r, 84, 80)); /* (pixels' middles 20.5 either side) */
    /* a spot pointing to +x, 0.3 either way */
    l.kind = ARTF_LIGHT2D_SPOT;
    l.coneInner = .2f;
    l.coneOuter = .3f;
    artfLight2DSet(s, light, &l);
    artfRenderer2DDraw(r, s);
    CHECK(redAt(r, 90, 80) > 100 && redAt(r, 38, 80) == 0 && redAt(r, 64, 106) == 0);
    /* ambient */
    artfScene2DRemoveLight(s, light);
    artfScene2DSetAmbient(s, artfVec3(.25f, .25f, .25f));
    artfRenderer2DDraw(r, s);
    CHECK(near_(redAt(r, 10, 10), 128, 1));
    artfScene2DSetAmbient(s, artfVec3(0, 0, 0));

    /* normal maps: a sprite facing left (x -1), lit from the left, then from the right */
    const uint32_t facing = 0xFF2880DBu; /* (x -.69, y 0, z .72, as red, green and blue) */
    ArtfTexture2DDesc nd = {.pixels = &facing, .width = 1, .height = 1, .normalMap = true};
    ArtfSprite2DDesc sp = artfSprite2DDefaults();
    sp.position = artfVec2(128, 80);
    sp.size = artfVec2(20, 20);
    sp.normalMap = artfScene2DAddTexture(s, &nd);
    sp.layer = 1;
    artfScene2DAddSprite(s, &sp);
    l = artfLight2DDefaults();
    l.position = artfVec2(88, 80);
    l.range = 100;
    l.height = 20;
    l.shadows = false;
    const ArtfLight2D fromLeft = artfScene2DAddLight(s, &l);
    artfRenderer2DDraw(r, s);
    const int lit = redAt(r, 128, 80), floorLeft = redAt(r, 108, 80);
    l.position = artfVec2(168, 80);
    artfLight2DSet(s, fromLeft, &l);
    artfRenderer2DDraw(r, s);
    const int unlit = redAt(r, 128, 80);
    CHECK(lit > unlit + 60 && floorLeft > 0);
    artfScene2DFree(s);
    artfRenderer2DFree(r);
}

/* Shadows: a wall between a light and the floor darkens what is behind it, not what is beside it; its own face is
 * lit; a wide light gives a penumbra where a point gives an edge; the sun's are parallel. */
void test2DShadows(void) {
    ArtfRenderer2D* r = renderer2D(0);
    ArtfScene2D* s = pixelScene();
    addRect(s, W2 * .5f, H2 * .5f, W2, H2, artfVec3(1, 1, 1), false);
    ArtfSprite2DDesc wall = artfSprite2DDefaults();
    wall.position = artfVec2(100, 80);
    wall.size = artfVec2(8, 30);
    wall.color = artfVec3(1, 0, 0);
    wall.castsShadow = true;
    wall.layer = 1;
    const ArtfSprite2D w = artfScene2DAddSprite(s, &wall);
    ArtfLight2DDesc l = artfLight2DDefaults();
    l.position = artfVec2(60, 80);
    l.range = 250;
    l.height = 10;
    const ArtfLight2D light = artfScene2DAddLight(s, &l);
    artfRenderer2DDraw(r, s);
    CHECK(redAt(r, 130, 80) == 0);              /* behind the wall */
    CHECK(redAt(r, 130, 130) > 20);             /* beside its shadow */
    CHECK(redAt(r, 97, 80) > 30);               /* the wall's own face */
    CHECK(greenAt(r, 97, 80) == 0);
    /* hard: the shadow's edge goes from dark to lit within a few pixels; wide: over many. Down a column behind the
     * wall, how many pixels see some of the light and not all, against the same light with no shadows */
    int open[80], grey[2] = {0, 0};
    l.shadows = false;
    artfLight2DSet(s, light, &l);
    artfRenderer2DDraw(r, s);
    for (int y = 0; y < 80; ++y) open[y] = redAt(r, 150, 80 + y);
    l.shadows = true;
    for (int soft = 0; soft < 2; ++soft) {
        l.radius = soft ? 12.f : 0.f;
        artfLight2DSet(s, light, &l);
        artfRenderer2DDraw(r, s);
        for (int y = 0; y < 80; ++y) {
            const int v = redAt(r, 150, 80 + y);
            grey[soft] += v > open[y] / 10 && v < open[y] * 9 / 10;
        }
    }
    CHECK(grey[0] <= 3 && grey[1] > 10);
    /* the outline counts, not the picture: an occluder alone (an open wall) throws a shadow too */
    artfScene2DRemoveSprite(s, w);
    const ArtfVec2 line[2] = {{100, 60}, {100, 100}};
    const ArtfOccluder2D o = artfScene2DAddOccluder(s, line, 2, false);
    artfRenderer2DDraw(r, s);
    CHECK(redAt(r, 130, 80) < 10 && redAt(r, 90, 80) > 50);
    /* the sun, from the left: all behind the wall in shadow, as far as the frame goes */
    artfScene2DRemoveLight(s, light);
    ArtfLight2DDesc sun = artfLight2DDefaults();
    sun.kind = ARTF_LIGHT2D_SUN;
    sun.direction = 0;
    sun.height = .5f;
    artfScene2DAddLight(s, &sun);
    artfRenderer2DDraw(r, s);
    CHECK(redAt(r, 250, 80) == 0 && redAt(r, 250, 50) > 50 && redAt(r, 50, 80) > 50);
    artfScene2DRemoveOccluder(s, o);
    artfRenderer2DDraw(r, s);
    CHECK(redAt(r, 250, 80) > 50);
    artfScene2DFree(s);
    artfRenderer2DFree(r);
}

/* gi: a glowing sprite lights the floor around it, less farther; a wall stops its light; what is lit bounces. */
void test2DGI(void) {
    ArtfRenderer2D* r = renderer2D(0);
    ArtfScene2D* s = pixelScene();
    addRect(s, W2 * .5f, H2 * .5f, W2, H2, artfVec3(1, 1, 1), false);
    ArtfSprite2DDesc glow = artfSprite2DDefaults();
    glow.shape = ARTF_SHAPE_ELLIPSE;
    glow.position = artfVec2(128, 80);
    glow.size = artfVec2(16, 16);
    glow.emissive = artfVec3(4, 4, 4);
    glow.layer = 1;
    artfScene2DAddSprite(s, &glow);
    ArtfGI2DDesc gi = artfGI2DDefaults();
    gi.scale = 2;
    artfScene2DSetGI(s, &gi);
    artfRenderer2DDraw(r, s);
    const int near1 = redAt(r, 150, 80), far1 = redAt(r, 200, 80), left = redAt(r, 106, 80), up = redAt(r, 128, 58);
    CHECK(near1 > far1 && far1 > 0);
    CHECK(near_(left, near1, 8) && near_(up, near1, 8)); /* (all around alike) */
    /* a wall between: behind it darker than the same distance on the other side */
    ArtfSprite2DDesc wall = artfSprite2DDefaults();
    wall.position = artfVec2(165, 80);
    wall.size = artfVec2(6, 60);
    wall.castsShadow = true;
    wall.color = artfVec3(0, 0, 0);
    wall.layer = 1;
    const ArtfSprite2D w = artfScene2DAddSprite(s, &wall);
    artfRenderer2DDraw(r, s);
    CHECK(redAt(r, 190, 80) + 10 < redAt(r, 66, 80));
    /* a white wall, lit by a light, bounces onto the floor beside it: more with bounce than without */
    wall.color = artfVec3(1, 1, 1);
    artfSprite2DSet(s, w, &wall);
    ArtfLight2DDesc l = artfLight2DDefaults();
    l.position = artfVec2(150, 20);
    l.range = 80;
    l.mask = 2;
    wall.lightMask = 3; /* (the light reaches only the wall) */
    artfSprite2DSet(s, w, &wall);
    artfScene2DAddLight(s, &l);
    gi.bounce = 0;
    artfScene2DSetGI(s, &gi);
    for (int i = 0; i < 3; ++i) artfRenderer2DDraw(r, s);
    const int without = redAt(r, 175, 40);
    gi.bounce = .9f;
    artfScene2DSetGI(s, &gi);
    for (int i = 0; i < 3; ++i) artfRenderer2DDraw(r, s);
    CHECK(redAt(r, 175, 40) > without + 3);
    ArtfFrameStats2D st;
    artfRenderer2DStats(r, &st);
    CHECK(st.giMs > 0 && st.spritesDrawn == 3 && st.lightsDrawn == 1 && st.shadowMaps == 1);
    artfScene2DFree(s);
    artfRenderer2DFree(r);
}

/* Bloom spreads what is bright, and keeps the rest; exposure and tone curves. */
void test2DPost(void) {
    ArtfRenderer2D* r = renderer2D(0);
    ArtfScene2D* s = pixelScene();
    addRect(s, 128, 80, 6, 6, artfVec3(20, 20, 20), true);
    artfRenderer2DDraw(r, s);
    CHECK(redAt(r, 128, 80) == 255 && redAt(r, 140, 80) == 0);
    ArtfPost2DDesc p = {.bloom = .2f, .bloomRadius = .1f};
    artfScene2DSetPost(s, &p);
    artfRenderer2DDraw(r, s);
    CHECK(redAt(r, 140, 80) > 20 && redAt(r, 140, 80) > redAt(r, 160, 80) && redAt(r, 128, 80) == 255);
    p.bloom = 0;
    p.exposure = .5f;
    p.tone = ARTF_TONE2D_REINHARD;
    artfScene2DSetPost(s, &p);
    addRect(s, 40, 40, 10, 10, artfVec3(1, 1, 1), true);
    artfRenderer2DDraw(r, s);
    CHECK(near_(redAt(r, 40, 40), (int)(sqrtf(.5f / 1.5f) * 255.f + .5f), 1));
    artfScene2DFree(s);
    artfRenderer2DFree(r);
}

/* All of it at once, on one thread and on many: the same bytes. */
/* All of it at once: textures and a normal map, every shape, shadows from a lamp, a spot and the sun, gi, bloom. */
static ArtfScene2D* richScene2D(void) {
    ArtfScene2D* s = artfScene2DNew();
    ArtfCamera2D c = {.center = {10, 5}, .zoom = 1.7f, .rotation = .3f};
    artfScene2DSetCamera(s, &c);
    artfScene2DSetAmbient(s, artfVec3(.05f, .05f, .08f));
    static uint32_t tex[16 * 16], normals[16 * 16];
    for (int i = 0; i < 256; ++i) {
        tex[i] = 0xFF000000u | (uint32_t)(i * 7 % 256) << 16 | (uint32_t)(i * 13 % 256) << 8 | 0x40;
        normals[i] = 0xFF000000u | (uint32_t)(96 + i % 64) << 16 | (uint32_t)(96 + i / 16 * 4) << 8 | 0xD0;
    }
    ArtfTexture2DDesc td = {.pixels = tex, .width = 16, .height = 16}, nd = {.pixels = normals, .width = 16, .height = 16, .normalMap = true};
    const ArtfTexture2D t1 = artfScene2DAddTexture(s, &td), t2 = artfScene2DAddTexture(s, &nd);
    ArtfSprite2DDesc floor_ = artfSprite2DDefaults();
    floor_.size = artfVec2(300, 200);
    floor_.texture = t1;
    floor_.normalMap = t2;
    floor_.specular = .5f;
    floor_.uvMax = artfVec2(8, 6);
    artfScene2DAddSprite(s, &floor_);
    for (int i = 0; i < 12; ++i) {
        ArtfSprite2DDesc d = artfSprite2DDefaults();
        d.shape = (ArtfShape2D)(i % 3);
        d.position = artfVec2((float)(i % 4) * 25.f - 40.f, (float)(i / 4) * 25.f - 25.f);
        d.size = artfVec2(10.f + (float)i, 8.f);
        d.rotation = (float)i * .4f;
        d.cornerRadius = 2;
        d.color = artfVec3(.2f + (float)i * .05f, .5f, .7f);
        d.castsShadow = i % 2 == 0;
        d.emissive = i == 5 ? artfVec3(3, 2, 1) : artfVec3(0, 0, 0);
        d.opacity = i == 7 ? .5f : 1;
        d.layer = 1;
        const ArtfVec2 star[5] = {{0, -8}, {5, 7}, {-7, -3}, {7, -3}, {-5, 7}};
        if (d.shape == ARTF_SHAPE_POLYGON) {
            d.points = star;
            d.pointCount = 5;
        }
        artfScene2DAddSprite(s, &d);
    }
    ArtfLight2DDesc l = artfLight2DDefaults();
    l.position = artfVec2(-10, 10);
    l.range = 90;
    l.radius = 3;
    l.color = artfVec3(1, .8f, .6f);
    artfScene2DAddLight(s, &l);
    l.kind = ARTF_LIGHT2D_SPOT;
    l.position = artfVec2(50, -40);
    l.direction = 2.4f;
    l.color = artfVec3(.3f, .5f, 1);
    artfScene2DAddLight(s, &l);
    ArtfLight2DDesc sun = artfLight2DDefaults();
    sun.kind = ARTF_LIGHT2D_SUN;
    sun.direction = 1;
    sun.radius = .05f;
    sun.intensity = .3f;
    artfScene2DAddLight(s, &sun);
    ArtfGI2DDesc gi = artfGI2DDefaults();
    artfScene2DSetGI(s, &gi);
    ArtfPost2DDesc p = {.bloom = .1f, .tone = ARTF_TONE2D_ACES};
    artfScene2DSetPost(s, &p);
    return s;
}

void test2DSameBytes(void) {
    uint64_t hashes[3];
    const int threads[3] = {1, 3, 0};
    for (int t = 0; t < 3; ++t) {
        ArtfRenderer2D* r = renderer2D(threads[t]);
        ArtfScene2D* s = richScene2D();
        artfRenderer2DDraw(r, s);
        artfRenderer2DDraw(r, s); /* (the second with what bounced in the first) */
        hashes[t] = hashOf(artfRenderer2DPixels(r), W2 * H2);
        artfScene2DFree(s);
        artfRenderer2DFree(r);
    }
    CHECK(hashes[0] == hashes[1] && hashes[1] == hashes[2]);
}

/* Handles that were removed stop working; a slot taken again names something new; picking finds what is on top;
 * the camera's maps there and back. */
static int logged2D;
static void count2D(void* user, const char* message) { (void)user, (void)message, ++logged2D; }
void test2DHandles(void) {
    ArtfScene2D* s = pixelScene();
    const ArtfSprite2D a = addRect(s, 50, 50, 20, 20, artfVec3(1, 1, 1), true);
    const ArtfSprite2D b = addRect(s, 55, 50, 20, 20, artfVec3(1, 1, 1), true);
    CHECK(artfScene2DPick(s, artfVec2(58, 50)) == b && artfScene2DPick(s, artfVec2(42, 50)) == a && !artfScene2DPick(s, artfVec2(5, 5)));
    artfScene2DRemoveSprite(s, b);
    CHECK(artfScene2DPick(s, artfVec2(58, 50)) == a);
    artfSetLog(count2D, NULL);
    logged2D = 0;
    artfSprite2DPlace(s, b, artfVec2(0, 0), 0);
    CHECK(logged2D == 1);
    const ArtfSprite2D c = addRect(s, 0, 0, 1, 1, artfVec3(1, 1, 1), true);
    CHECK(c != b && (c & 0xFFFFFF) == (b & 0xFFFFFF));
    artfSprite2DPlace(s, b, artfVec2(0, 0), 0);
    CHECK(logged2D == 2);
    artfSetLog(NULL, NULL);
    ArtfCamera2D cam = {.center = {100, -40}, .zoom = 2.5f, .rotation = 1.1f};
    artfScene2DSetCamera(s, &cam);
    const ArtfVec2 p = artfScene2DToFrame(s, 640, 480, artfVec2(120, -10));
    const ArtfVec2 back = artfScene2DToWorld(s, 640, 480, p);
    CHECK(fabsf(back.x - 120) < 1e-3f && fabsf(back.y + 10) < 1e-3f);
    const ArtfVec2 middle = artfScene2DToFrame(s, 640, 480, cam.center);
    CHECK(fabsf(middle.x - 320) < 1e-3f && fabsf(middle.y - 240) < 1e-3f);
    artfScene2DFree(s);
}

/* With 3D: a 2D frame with a clear background over a 3D one as its overlay; the 3D frame as a 2D texture. */
void test2DOver3D(void) {
    ArtfRenderer2DOptions o2 = {.width = W, .height = H, .backend = ARTF_BACKEND2D_SOFTWARE};
    ArtfRenderer2D* r2 = artfRenderer2DNew(&o2);
    ArtfScene2D* s2 = artfScene2DNew();
    ArtfCamera2D cam = {.center = {W * .5f, H * .5f}, .zoom = 1};
    artfScene2DSetCamera(s2, &cam);
    artfScene2DSetBackground(s2, artfVec3(0, 0, 0), 0);
    ArtfSprite2DDesc d = artfSprite2DDefaults();
    d.position = artfVec2(40, 40);
    d.size = artfVec2(20, 20);
    d.color = artfVec3(0, 1, 0);
    d.unlit = true;
    artfScene2DAddSprite(s2, &d);
    artfRenderer2DDraw(r2, s2);
    const uint32_t* over = artfRenderer2DPixels(r2);
    CHECK(over[0] == 0 && over[40 * W + 40] == 0xFF00FF00u);

    ArtfRenderer* r3 = artfRendererNew(&(ArtfRendererOptions){.backend = ARTF_BACKEND_SOFTWARE, .width = W, .height = H});
    ArtfScene* s3 = plainScene();
    ArtfOverlay ov = {.pixels = over, .width = W, .height = H};
    artfRendererSetOverlay(r3, &ov);
    uint32_t* frame = artfMalloc(W * H * 4);
    drawInto(r3, s3, frame);
    CHECK(frame[40 * W + 40] == 0xFF00FF00u && frame[150 * W + 300] != 0xFF00FF00u);
    /* and back: the 3D frame, premultiplied, as a texture under a 2D sprite */
    ArtfTexture2DDesc td = {.pixels = frame, .width = W, .height = H, .premultiplied = true};
    d.texture = artfScene2DAddTexture(s2, &td);
    d.position = artfVec2(W * .5f, H * .5f);
    d.size = artfVec2(W, H);
    d.color = artfVec3(1, 1, 1);
    d.nearest = true;
    d.layer = -1;
    artfScene2DAddSprite(s2, &d);
    artfRenderer2DDraw(r2, s2);
    CHECK(artfRenderer2DPixels(r2)[150 * W + 300] == frame[150 * W + 300]);
    artfFree(frame);
    artfRendererFree(r3);
    artfSceneFree(s3);
    artfScene2DFree(s2);
    artfRenderer2DFree(r2);
}

/* ---- the GPU ---- */

/* Of two frames: the share of pixels with a channel more than threshold apart, and the most any is. */
static double apart2D(const uint32_t* a, const uint32_t* b, int threshold, int* most) {
    int count = 0;
    *most = 0;
    for (int i = 0; i < W2 * H2; ++i)
        for (int c = 0; c < 32; c += 8) {
            const int d = abs((int)(a[i] >> c & 255) - (int)(b[i] >> c & 255));
            if (d > *most) *most = d;
            if (d > threshold) {
                ++count;
                break;
            }
        }
    return (double)count / (W2 * H2);
}

/* Vulkan against the CPU: the same scenes give the same picture, within a step of a byte, but where a polygon's
 * edge crosses a pixel (sixteen samples there, against the CPU's exact spans), over frames as gi bounces; a texture
 * changed between frames is seen; a frame with a clear background stays clear. */
void test2DVulkan(void) {
    ArtfRenderer2DOptions o = {.width = W2, .height = H2, .backend = ARTF_BACKEND2D_VULKAN};
    ArtfRenderer2D* gpu = artfRenderer2DNew(&o);
    if (!gpu) {
        puts("2d/vulkan: no Vulkan here, not run");
        return;
    }
    CHECK(artfRenderer2DBackend(gpu) == ARTF_BACKEND2D_VULKAN && strlen(artfRenderer2DDescription(gpu)) > 0);
    ArtfRenderer2D* cpu = renderer2D(0);
    CHECK(artfRenderer2DBackend(cpu) == ARTF_BACKEND2D_SOFTWARE);
    for (int variant = 0; variant < 4; ++variant) {
        ArtfScene2D* s = richScene2D();
        if (variant == 1) artfScene2DSetPost(s, NULL);
        if (variant == 2) {
            artfScene2DSetGI(s, NULL);
            ArtfPost2DDesc p = {.exposure = 1.5f, .tone = ARTF_TONE2D_REINHARD};
            artfScene2DSetPost(s, &p);
        }
        if (variant == 3) artfScene2DSetBackground(s, artfVec3(.2f, .1f, 0), 0);
        for (int frame = 0; frame < 3; ++frame) {
            artfRenderer2DDraw(cpu, s);
            artfRenderer2DDraw(gpu, s);
            int most;
            const double share = apart2D(artfRenderer2DPixels(cpu), artfRenderer2DPixels(gpu), 2, &most);
            CHECK(share < .003);
            if (share >= .003) printf("  variant %d frame %d: %.4f apart, at most %d\n", variant, frame, share, most);
        }
        artfScene2DFree(s);
    }
    /* a texture changed between frames, and a clear background */
    ArtfScene2D* s = pixelScene();
    artfScene2DSetBackground(s, artfVec3(0, 0, 0), 0);
    uint32_t px = 0xFFFF0000u;
    ArtfTexture2DDesc td = {.pixels = &px, .width = 1, .height = 1};
    ArtfSprite2DDesc d = artfSprite2DDefaults();
    d.position = artfVec2(64, 64);
    d.size = artfVec2(32, 32);
    d.texture = artfScene2DAddTexture(s, &td);
    d.unlit = true;
    artfScene2DAddSprite(s, &d);
    artfRenderer2DDraw(gpu, s);
    CHECK(artfRenderer2DPixels(gpu)[64 * W2 + 64] == 0xFFFF0000u && artfRenderer2DPixels(gpu)[5 * W2 + 5] == 0);
    px = 0xFF00FF00u;
    artfScene2DSetTexture(s, d.texture, &td);
    artfRenderer2DDraw(gpu, s);
    CHECK(artfRenderer2DPixels(gpu)[64 * W2 + 64] == 0xFF00FF00u);
    ArtfFrameStats2D st;
    artfRenderer2DStats(gpu, &st);
    CHECK(st.gpuMs > 0 && st.spritesDrawn == 1);
    artfScene2DFree(s);
    artfRenderer2DFree(cpu);
    artfRenderer2DFree(gpu);
}

#ifdef ARTFUL_VULKAN
#include "2d/vulkan2d.h"
#endif

/* Vulkan, two frames on their way: each frame read is the one drawn last, as the CPU draws it, while a sprite moves
 * and gi bounces; and of the textures, only those that changed go up, where they were if they still fit. */
void test2DVulkanFrames(void) {
#ifdef ARTFUL_VULKAN
    ArtfRenderer2DOptions o = {.width = W2, .height = H2, .backend = ARTF_BACKEND2D_VULKAN};
    ArtfRenderer2D* gpu = artfRenderer2DNew(&o);
    if (!gpu) {
        puts("2d/vulkanFrames: no Vulkan here, not run");
        return;
    }
    ArtfRenderer2D* cpu = renderer2D(0);
    ArtfScene2D* s = pixelScene();
    addRect(s, W2 * .5f, H2 * .5f, W2, H2, artfVec3(.8f, .8f, .8f), false);
    ArtfSprite2DDesc wall = artfSprite2DDefaults();
    wall.size = artfVec2(10, 50);
    wall.castsShadow = true;
    wall.layer = 1;
    const ArtfSprite2D w = artfScene2DAddSprite(s, &wall);
    ArtfSprite2DDesc glow = artfSprite2DDefaults();
    glow.shape = ARTF_SHAPE_ELLIPSE;
    glow.position = artfVec2(200, 40);
    glow.size = artfVec2(12, 12);
    glow.emissive = artfVec3(3, 2, 1);
    glow.layer = 1;
    artfScene2DAddSprite(s, &glow);
    ArtfLight2DDesc l = artfLight2DDefaults();
    l.position = artfVec2(60, 80);
    l.range = 200;
    l.radius = 4;
    artfScene2DAddLight(s, &l);
    ArtfGI2DDesc gi = artfGI2DDefaults();
    gi.scale = 2;
    artfScene2DSetGI(s, &gi);
    int most;
    for (int i = 0; i < 6; ++i) {
        artfSprite2DPlace(s, w, artfVec2(100.f + (float)i * 9.f, 80), (float)i * .2f);
        CHECK(artfRenderer2DDraw(gpu, s));
        artfRenderer2DDraw(cpu, s);
        CHECK(apart2D(artfRenderer2DPixels(cpu), artfRenderer2DPixels(gpu), 2, &most) < .003);
    }
    /* two drawn, then read: the last */
    artfSprite2DPlace(s, w, artfVec2(90, 70), 0);
    artfRenderer2DDraw(gpu, s);
    artfRenderer2DDraw(cpu, s);
    artfSprite2DPlace(s, w, artfVec2(170, 100), 1);
    artfRenderer2DDraw(gpu, s);
    artfRenderer2DDraw(cpu, s);
    uint32_t* read = artfMalloc(W2 * H2 * 4);
    CHECK(artfRenderer2DReadPixels(gpu, read));
    CHECK(apart2D(artfRenderer2DPixels(cpu), read, 2, &most) < .003);
    CHECK(!memcmp(read, artfRenderer2DPixels(gpu), W2 * H2 * 4));

    /* textures: a large one and a small one; then only the small one changes */
    static uint32_t large[128 * 128], small[8 * 8];
    for (int i = 0; i < 128 * 128; ++i) large[i] = 0xFF000000u | (uint32_t)(i * 31 % 256) << 16 | (uint32_t)(i % 256);
    for (int i = 0; i < 64; ++i) small[i] = 0xFF00FF00u;
    ArtfTexture2DDesc ld = {.pixels = large, .width = 128, .height = 128}, sd = {.pixels = small, .width = 4, .height = 4};
    ArtfSprite2DDesc a = artfSprite2DDefaults(), b = artfSprite2DDefaults();
    a.texture = artfScene2DAddTexture(s, &ld);
    a.position = artfVec2(40, 40);
    a.size = artfVec2(40, 40);
    a.unlit = true;
    a.layer = 2;
    b.texture = artfScene2DAddTexture(s, &sd);
    b.position = artfVec2(220, 130);
    b.size = artfVec2(30, 30);
    b.unlit = true;
    b.nearest = true;
    b.layer = 2;
    const ArtfSprite2D aSprite = artfScene2DAddSprite(s, &a);
    artfScene2DAddSprite(s, &b);
    artfRenderer2DDraw(gpu, s);
    uint64_t sent = artf2DVulkanTexelsSent;
    artfRenderer2DDraw(gpu, s);
    CHECK(artf2DVulkanTexelsSent == sent); /* (nothing changed: nothing sent) */
    for (int i = 0; i < 64; ++i) small[i] = 0xFFFF0000u;
    artfScene2DSetTexture(s, b.texture, &sd);
    artfRenderer2DDraw(gpu, s);
    CHECK(artf2DVulkanTexelsSent - sent == 16 + 4 + 1);
    CHECK(artfRenderer2DPixels(gpu)[130 * W2 + 220] == 0xFFFF0000u);
    /* larger: at the end, the large one where it was */
    sent = artf2DVulkanTexelsSent;
    sd.width = sd.height = 8;
    for (int i = 0; i < 64; ++i) small[i] = 0xFF0000FFu;
    artfScene2DSetTexture(s, b.texture, &sd);
    artfRenderer2DDraw(gpu, s);
    artfRenderer2DDraw(cpu, s);
    CHECK(artf2DVulkanTexelsSent - sent == 64 + 16 + 4 + 1);
    CHECK(artfRenderer2DPixels(gpu)[130 * W2 + 220] == 0xFF0000FFu);
    for (int i = 0; i < 2; ++i) artfRenderer2DDraw(gpu, s), artfRenderer2DDraw(cpu, s); /* (what bounces, as many frames on both) */
    CHECK(apart2D(artfRenderer2DPixels(cpu), artfRenderer2DPixels(gpu), 2, &most) < .003);
    /* one goes, another comes: still the CPU's picture */
    artfScene2DRemoveTexture(s, a.texture);
    ld.width = ld.height = 64;
    a.texture = artfScene2DAddTexture(s, &ld);
    artfSprite2DSet(s, aSprite, &a);
    for (int i = 0; i < 3; ++i) artfRenderer2DDraw(gpu, s), artfRenderer2DDraw(cpu, s);
    CHECK(apart2D(artfRenderer2DPixels(cpu), artfRenderer2DPixels(gpu), 2, &most) < .003);
    artfFree(read);
    artfScene2DFree(s);
    artfRenderer2DFree(cpu);
    artfRenderer2DFree(gpu);
#else
    missing("vulkan");
#endif
}

#ifdef _WIN32
__declspec(dllimport) void* __stdcall CreateWindowExA(unsigned long, const char*, const char*, unsigned long, int, int, int, int, void*, void*, void*, void*);
__declspec(dllimport) int __stdcall DestroyWindow(void*);
/* Vulkan into a window: frames are shown, not read back (artfRenderer2DPixels has none), and read back when asked,
 * as the CPU draws them; with no vsync too, and after a resize. */
void test2DWindow(void) {
    void* window = CreateWindowExA(0, "STATIC", "artful-2d", 0xCF0000u, 0, 0, W2 + 40, H2 + 60, NULL, NULL, NULL, NULL);
    if (!window) { missing("window"); return; }
    for (int vsync = 0; vsync < 2; ++vsync) {
        ArtfRenderer2DOptions o = {.width = W2, .height = H2, .backend = ARTF_BACKEND2D_VULKAN, .noVsync = !vsync};
        o.surface.kind = ARTF_SURFACE_WIN32;
        o.surface.a = window;
        ArtfRenderer2D* gpu = artfRenderer2DNew(&o);
        if (!gpu) {
            puts("2d/window: no Vulkan here, or no window, not run");
            break;
        }
        ArtfRenderer2D* cpu = renderer2D(0);
        ArtfScene2D* s = richScene2D();
        for (int i = 0; i < 4; ++i) {
            CHECK(artfRenderer2DDraw(gpu, s));
            artfRenderer2DDraw(cpu, s);
        }
        CHECK(artfRenderer2DPixels(gpu) == NULL);
        uint32_t* read = artfMalloc(W2 * H2 * 4);
        int most;
        CHECK(artfRenderer2DReadPixels(gpu, read));
        CHECK(apart2D(artfRenderer2DPixels(cpu), read, 2, &most) < .003);
        artfRenderer2DResize(gpu, W2 / 2, H2 / 2);
        CHECK(artfRenderer2DDraw(gpu, s));
        CHECK(artfRenderer2DDraw(gpu, s));
        CHECK(artfRenderer2DReadPixels(gpu, read));
        artfFree(read);
        artfScene2DFree(s);
        artfRenderer2DFree(cpu);
        artfRenderer2DFree(gpu);
    }
    DestroyWindow(window);
}
#else
void test2DWindow(void) { missing("window"); }
#endif
