/* artful-2d — the scene: what handles name, textures and their mipmaps, sprites, lights, the camera. */
#include "core/threads.h"
#include "2d/internal2d.h"
#include "core/trig.h"

#include <stdlib.h>

float artf2DLight[256];

void artf2DTables(void) {
    if (artf2DLight[255] != 0) return;
    for (int i = 0; i < 256; ++i) artf2DLight[i] = artfSrgbDecode((float)i / 255.f);
}

/* ---- handles ---- */

static uint32_t handleOf(uint32_t index, const Slot2D* slot) { return (uint32_t)slot->generation << 24 | (index + 1); }

/* The index a handle names among count slots of size stride, or -1 (and a word). */
static long slotIndex(const void* base, size_t count, size_t stride, uint32_t h, const char* what) {
    const uint32_t index = (h & 0xFFFFFFu) - 1;
    if (h && index < count) {
        const Slot2D* slot = (const Slot2D*)((const char*)base + index * stride);
        if (slot->alive && slot->generation == h >> 24) return (long)index;
    }
    if (h) artfLog("artful-2d: %s %u is not there", what, h);
    return -1;
}

/* A free slot of v (whose items start with a Slot2D), or a new one; its index. */
#define TAKE_SLOT(v, out) do { \
        size_t i_ = 0; \
        while (i_ < (v)->len && (v)->items[i_].slot.alive) ++i_; \
        if (i_ == (v)->len) { artfVecReserve((v), (v)->len + 1); memset(&(v)->items[(v)->len], 0, sizeof *(v)->items); ++(v)->len; } \
        uint8_t g_ = (uint8_t)((v)->items[i_].slot.generation + 1); \
        memset(&(v)->items[i_], 0, sizeof *(v)->items); \
        (v)->items[i_].slot.generation = g_ ? g_ : 1; \
        (v)->items[i_].slot.alive = true; \
        (out) = (uint32_t)i_; \
    } while (0)

#define FIND(v, h, what) slotIndex((v).items, (v).len, sizeof *(v).items, h, what)

ArtfScene2D* artfScene2DNew(void) {
    artf2DTables();
    ArtfScene2D* s = ARTF_NEW(ArtfScene2D);
    /* (a scene of its own in the top bits: a new one at a freed one's address does not look like it to a GPU) */
    static _Atomic uint64_t scenes;
    s->textureVersion = (uint64_t)++scenes << 32;
    s->ambient = artfVec3(0, 0, 0);
    s->gi = artfGI2DDefaults();
    s->gi.enabled = false;
    s->backgroundAlpha = 1;
    s->camera.zoom = 1;
    return s;
}

static void textureEmpty(Texture2D* t) {
    for (int l = 0; l < t->levels; ++l) artfFree(t->texels[l]);
    t->levels = 0;
}

/* A scene belongs to one thread at a time: what changes it while another draws it is said (as the 3D scene's). */
static void changing2D(const ArtfScene2D* s, const char* what) {
    const uintptr_t drawing = s->drawingThread;
    if (drawing && drawing != artfThreadId())
        artfLog("artful: %s of a 2D scene changed from another thread while it was drawn: a scene belongs to one thread at a time", what);
}

void artfScene2DFree(ArtfScene2D* s) {
    if (!s) return;
    for (size_t i = 0; i < s->textures.len; ++i) textureEmpty(&s->textures.items[i]);
    for (size_t i = 0; i < s->sprites.len; ++i) artfFree(s->sprites.items[i].points);
    for (size_t i = 0; i < s->occluders.len; ++i) artfFree(s->occluders.items[i].points);
    artfVecFree(&s->textures);
    artfVecFree(&s->sprites);
    artfVecFree(&s->lights);
    artfVecFree(&s->occluders);
    artfFree(s);
}

/* ---- textures ---- */

Texture2D* artf2DTexture(const ArtfScene2D* s, ArtfTexture2D h) {
    const long i = FIND(s->textures, h, "texture");
    return i < 0 ? NULL : &s->textures.items[i];
}

static uint8_t byteOfLight(float v) {
    const float b = artfSrgbEncode(artfClampf(v, 0, 1)) * 255.f + .5f;
    return (uint8_t)b;
}

/* The next level down: each texel the mean of the four (or fewer) above, in light, weighed by alpha. */
static void mipDown(Texture2D* t, int l) {
    const int w = t->width[l - 1], h = t->height[l - 1];
    const int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
    const uint32_t* src = t->texels[l - 1];
    uint32_t* dst = artfMalloc((size_t)nw * nh * 4);
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x) {
            float sum[4] = {0, 0, 0, 0};
            int n = 0;
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = artfMini(x * 2 + dx, w - 1), sy = artfMini(y * 2 + dy, h - 1);
                    const uint32_t p = src[(size_t)sy * w + sx];
                    const float a = (float)(p >> 24) / 255.f;
                    for (int c = 0; c < 3; ++c) {
                        const int byte = p >> (16 - 8 * c) & 255;
                        sum[c] += t->normalMap ? (float)byte : artf2DLight[byte] * a;
                    }
                    sum[3] += a;
                    ++n;
                }
            uint32_t out = (uint32_t)(sum[3] / (float)n * 255.f + .5f) << 24;
            for (int c = 0; c < 3; ++c) {
                uint32_t byte;
                if (t->normalMap) byte = (uint32_t)(sum[c] / (float)n + .5f);
                else byte = sum[3] > 0 ? byteOfLight(sum[c] / sum[3]) : 0;
                out |= byte << (16 - 8 * c);
            }
            dst[(size_t)y * nw + x] = out;
        }
    t->texels[l] = dst;
    t->width[l] = nw;
    t->height[l] = nh;
}

static void textureFill(Texture2D* t, const ArtfTexture2DDesc* d) {
    textureEmpty(t);
    /* (the scene's textureVersion moves where this is called) */
    t->normalMap = d->normalMap;
    const int w = artfMaxi(d->width, 1), h = artfMaxi(d->height, 1), stride = d->stride ? d->stride : d->width;
    uint32_t* top = artfMalloc((size_t)w * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t p = d->pixels && d->width > 0 && d->height > 0 ? d->pixels[(size_t)y * stride + x] : 0xFFFFFFFFu;
            const uint32_t a = p >> 24;
            if (d->premultiplied && !d->normalMap && a < 255) {
                /* (to straight: its bytes are sRGB times alpha) */
                uint32_t q = a << 24;
                for (int c = 0; c < 24; c += 8) {
                    const uint32_t v = a ? ((p >> c & 255) * 255 + a / 2) / a : 0;
                    q |= (v > 255 ? 255 : v) << c;
                }
                p = q;
            }
            top[(size_t)y * w + x] = p;
        }
    t->texels[0] = top;
    t->width[0] = w;
    t->height[0] = h;
    t->levels = 1;
    while (t->levels < MIPS2D && (t->width[t->levels - 1] > 1 || t->height[t->levels - 1] > 1)) {
        mipDown(t, t->levels);
        ++t->levels;
    }
}

ArtfTexture2D artfScene2DAddTexture(ArtfScene2D* s, const ArtfTexture2DDesc* desc) {
    changing2D(s, "a texture");
    uint32_t i;
    TAKE_SLOT(&s->textures, i);
    textureFill(&s->textures.items[i], desc);
    s->textures.items[i].version = ++s->textureVersion;
    return handleOf(i, &s->textures.items[i].slot);
}

void artfScene2DSetTexture(ArtfScene2D* s, ArtfTexture2D h, const ArtfTexture2DDesc* desc) {
    changing2D(s, "a texture");
    Texture2D* t = artf2DTexture(s, h);
    if (!t) return;
    textureFill(t, desc);
    t->version = ++s->textureVersion;
}

void artfScene2DRemoveTexture(ArtfScene2D* s, ArtfTexture2D h) {
    changing2D(s, "a texture");
    Texture2D* t = artf2DTexture(s, h);
    if (!t) return;
    textureEmpty(t);
    t->slot.alive = false;
    ++s->textureVersion;
}

/* ---- sprites ---- */

ArtfSprite2DDesc artfSprite2DDefaults(void) {
    ArtfSprite2DDesc d = {0};
    d.size = artfVec2(1, 1);
    d.color = artfVec3(1, 1, 1);
    d.opacity = 1;
    d.roughness = .5f;
    d.lightMask = 0xFFFFFFFFu;
    return d;
}

static void spriteFill(Sprite2D* sp, const ArtfSprite2DDesc* d) {
    artfFree(sp->points);
    sp->points = NULL;
    sp->desc = *d;
    const int n = d->points ? artfMaxi(d->pointCount, 0) : 0;
    if (n) {
        sp->points = artfMalloc(sizeof(ArtfVec2) * n);
        memcpy(sp->points, d->points, sizeof(ArtfVec2) * n);
    }
    sp->desc.points = sp->points;
    sp->desc.pointCount = n;
}

ArtfSprite2D artfScene2DAddSprite(ArtfScene2D* s, const ArtfSprite2DDesc* desc) {
    changing2D(s, "a sprite");
    uint32_t i;
    TAKE_SLOT(&s->sprites, i);
    Sprite2D* sp = &s->sprites.items[i];
    spriteFill(sp, desc);
    sp->order = s->nextOrder++;
    return handleOf(i, &sp->slot);
}

void artfSprite2DSet(ArtfScene2D* s, ArtfSprite2D h, const ArtfSprite2DDesc* desc) {
    changing2D(s, "a sprite");
    const long i = FIND(s->sprites, h, "sprite");
    if (i >= 0) spriteFill(&s->sprites.items[i], desc);
}

void artfSprite2DPlace(ArtfScene2D* s, ArtfSprite2D h, ArtfVec2 position, float rotation) {
    changing2D(s, "a sprite");
    const long i = FIND(s->sprites, h, "sprite");
    if (i < 0) return;
    s->sprites.items[i].desc.position = position;
    s->sprites.items[i].desc.rotation = rotation;
}

void artfScene2DRemoveSprite(ArtfScene2D* s, ArtfSprite2D h) {
    changing2D(s, "a sprite");
    const long i = FIND(s->sprites, h, "sprite");
    if (i < 0) return;
    Sprite2D* sp = &s->sprites.items[i];
    artfFree(sp->points);
    sp->points = NULL;
    sp->slot.alive = false;
}

/* ---- occluders ---- */

ArtfOccluder2D artfScene2DAddOccluder(ArtfScene2D* s, const ArtfVec2* points, int count, bool closed) {
    changing2D(s, "an occluder");
    uint32_t i;
    TAKE_SLOT(&s->occluders, i);
    Occluder2D* o = &s->occluders.items[i];
    o->count = points ? artfMaxi(count, 0) : 0;
    if (o->count) {
        o->points = artfMalloc(sizeof(ArtfVec2) * o->count);
        memcpy(o->points, points, sizeof(ArtfVec2) * o->count);
    }
    o->closed = closed;
    return handleOf(i, &o->slot);
}

void artfScene2DRemoveOccluder(ArtfScene2D* s, ArtfOccluder2D h) {
    changing2D(s, "an occluder");
    const long i = FIND(s->occluders, h, "occluder");
    if (i < 0) return;
    artfFree(s->occluders.items[i].points);
    s->occluders.items[i].points = NULL;
    s->occluders.items[i].slot.alive = false;
}

/* ---- lights ---- */

ArtfLight2DDesc artfLight2DDefaults(void) {
    ArtfLight2DDesc d = {0};
    d.color = artfVec3(1, 1, 1);
    d.intensity = 1;
    d.range = 256;
    d.coneInner = .4f;
    d.coneOuter = .6f;
    d.shadows = true;
    d.mask = 0xFFFFFFFFu;
    return d;
}

ArtfLight2D artfScene2DAddLight(ArtfScene2D* s, const ArtfLight2DDesc* desc) {
    changing2D(s, "a light");
    uint32_t i;
    TAKE_SLOT(&s->lights, i);
    s->lights.items[i].desc = *desc;
    return handleOf(i, &s->lights.items[i].slot);
}

void artfLight2DSet(ArtfScene2D* s, ArtfLight2D h, const ArtfLight2DDesc* desc) {
    changing2D(s, "a light");
    const long i = FIND(s->lights, h, "light");
    if (i >= 0) s->lights.items[i].desc = *desc;
}

void artfScene2DRemoveLight(ArtfScene2D* s, ArtfLight2D h) {
    changing2D(s, "a light");
    const long i = FIND(s->lights, h, "light");
    if (i >= 0) s->lights.items[i].slot.alive = false;
}

/* ---- the scene's own ---- */

void artfScene2DSetAmbient(ArtfScene2D* s, ArtfVec3 color) {
    changing2D(s, "the ambient light");
    s->ambient = color;
}

ArtfGI2DDesc artfGI2DDefaults(void) {
    ArtfGI2DDesc d = {.enabled = true, .scale = 4, .intensity = 1, .bounce = .5f};
    return d;
}

void artfScene2DSetGI(ArtfScene2D* s, const ArtfGI2DDesc* desc) {
    changing2D(s, "gi");
    if (!desc) {
        s->gi.enabled = false;
        return;
    }
    s->gi = *desc;
    const int scale = desc->scale;
    s->gi.scale = scale >= 8 ? 8 : scale >= 4 ? 4 : scale >= 2 ? 2 : 1;
    if (scale <= 0) s->gi.scale = 4;
    s->gi.bounce = artfClampf(desc->bounce, 0, .95f);
}

void artfScene2DSetPost(ArtfScene2D* s, const ArtfPost2DDesc* desc) {
    changing2D(s, "what comes after");
    if (desc) s->post = *desc;
    else memset(&s->post, 0, sizeof s->post);
}

void artfScene2DSetBackground(ArtfScene2D* s, ArtfVec3 color, float alpha) {
    changing2D(s, "the background");
    s->background = color;
    s->backgroundAlpha = artfClampf(alpha, 0, 1);
}

void artfScene2DSetCamera(ArtfScene2D* s, const ArtfCamera2D* camera) {
    changing2D(s, "the camera");
    s->camera = *camera;
    if (!(s->camera.zoom > 0)) s->camera.zoom = 1;
}

/* ---- maps between spaces ---- */

Affine2D artf2DMul(Affine2D a, Affine2D b) {
    Affine2D r = {{a.m[0] * b.m[0] + a.m[1] * b.m[3], a.m[0] * b.m[1] + a.m[1] * b.m[4], a.m[0] * b.m[2] + a.m[1] * b.m[5] + a.m[2],
                   a.m[3] * b.m[0] + a.m[4] * b.m[3], a.m[3] * b.m[1] + a.m[4] * b.m[4], a.m[3] * b.m[2] + a.m[4] * b.m[5] + a.m[5]}};
    return r;
}

Affine2D artf2DInverse(Affine2D a) {
    const float det = a.m[0] * a.m[4] - a.m[1] * a.m[3];
    const float k = det != 0 ? 1.f / det : 0;
    Affine2D r = {{a.m[4] * k, -a.m[1] * k, 0, -a.m[3] * k, a.m[0] * k, 0}};
    r.m[2] = -(r.m[0] * a.m[2] + r.m[1] * a.m[5]);
    r.m[5] = -(r.m[3] * a.m[2] + r.m[4] * a.m[5]);
    return r;
}

/* Turned by angle (clockwise on the screen, y down), then moved by (x, y). */
static Affine2D turnThenMove(float angle, float scale, float x, float y) {
    const float c = artfCos(angle) * scale, s = artfSin(angle) * scale;
    Affine2D r = {{c, -s, x, s, c, y}};
    return r;
}

Affine2D artf2DCameraMap(const ArtfCamera2D* c, int width, int height) {
    const float zoom = c->zoom > 0 ? c->zoom : 1;
    /* (the world turns against the camera) */
    const Affine2D turn = turnThenMove(-c->rotation, zoom, (float)width * .5f, (float)height * .5f);
    const Affine2D back = {{1, 0, -c->center.x, 0, 1, -c->center.y}};
    return artf2DMul(turn, back);
}

static float scaleOr1(float v) { return v != 0 ? v : 1; }

Affine2D artf2DSpriteMap(const Sprite2D* sp) {
    const ArtfSprite2DDesc* d = &sp->desc;
    Affine2D r = turnThenMove(d->rotation, 1, d->position.x, d->position.y);
    if (d->shape == ARTF_SHAPE_RECT || d->shape == ARTF_SHAPE_ELLIPSE) {
        const Affine2D off = {{1, 0, -d->pivot.x * d->size.x, 0, 1, -d->pivot.y * d->size.y}};
        return artf2DMul(r, off);
    }
    const Affine2D scale = {{scaleOr1(d->size.x), 0, 0, 0, scaleOr1(d->size.y), 0}};
    return artf2DMul(r, scale);
}

ArtfVec2 artfScene2DToFrame(const ArtfScene2D* s, int frameWidth, int frameHeight, ArtfVec2 world) {
    const Affine2D m = artf2DCameraMap(&s->camera, frameWidth, frameHeight);
    return artf2DApply(&m, world.x, world.y);
}

ArtfVec2 artfScene2DToWorld(const ArtfScene2D* s, int frameWidth, int frameHeight, ArtfVec2 pixel) {
    const Affine2D m = artf2DInverse(artf2DCameraMap(&s->camera, frameWidth, frameHeight));
    return artf2DApply(&m, pixel.x, pixel.y);
}

ArtfSprite2D artfScene2DPick(const ArtfScene2D* s, ArtfVec2 world) {
    const Sprite2D* best = NULL;
    uint32_t bestHandle = 0;
    for (size_t i = 0; i < s->sprites.len; ++i) {
        const Sprite2D* sp = &s->sprites.items[i];
        if (!sp->slot.alive || sp->desc.hidden) continue;
        if (best && (sp->desc.layer < best->desc.layer || (sp->desc.layer == best->desc.layer && sp->order < best->order))) continue;
        const Affine2D toLocal = artf2DInverse(artf2DSpriteMap(sp));
        const ArtfVec2 l = artf2DApply(&toLocal, world.x, world.y);
        if (artf2DCoverAt(sp, l.x, l.y, 1e-6f) >= .5f) {
            best = sp;
            bestHandle = handleOf((uint32_t)i, &sp->slot);
        }
    }
    return bestHandle;
}
