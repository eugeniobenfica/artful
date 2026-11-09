/* artful — what is made of the scene's own parts: emitters of particles, decals, and terrain from heights. Nothing
 * here is a backend's: particles are instances of one quad, turned to face the eye as each frame is drawn
 * (artfSceneFaceParticles), and terrain is a mesh; so both draw them, and the same way. */
#include "scene/sceneInternal.h"

#include <string.h>

/* ---- particles ---- */

#define EMITTER_SHADES 8 /* materials between a particle's first color and its last */

typedef struct Particle {
    float position[3], velocity[3];
    float age;       /* seconds; < 0: not alive */
    float size;      /* now */
} Particle;

typedef struct EmitterData {
    ArtfEmitterDesc desc;
    Particle* particles;
    ArtfInstance* instances;
    uint32_t count;
    ArtfMaterial shades[EMITTER_SHADES];
    float owed;      /* particles to spawn, in part */
    uint32_t state;  /* (the same sequence everywhere) */
    bool removed;
} EmitterData;

typedef ARTF_VEC(EmitterData*) EmitterList;

static float randomOf(uint32_t* state) {
    *state = *state * 747796405u + 2891336453u;
    uint32_t x = *state;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    return (float)(x >> 8) * (1.f / 16777216.f);
}

static void emittersFree(void* list) {
    EmitterList* l = list;
    for (size_t i = 0; i < l->len; ++i) {
        artfFree(l->items[i]->particles);
        artfFree(l->items[i]->instances);
        artfFree(l->items[i]);
    }
    artfVecFree(l);
    artfFree(l);
}

static EmitterData* emitterAt(const ArtfScene* s, ArtfEmitter e) {
    const EmitterList* l = s->emitters;
    if (!e || !l || e > l->len || l->items[e - 1]->removed) {
        artfLog("artful: emitter %u is not there", e);
        return NULL;
    }
    return l->items[e - 1];
}

/* (a quad of side 1, shared by every emitter) */
static ArtfMesh quadOf(ArtfScene* s) {
    if (!s->particleQuad) s->particleQuad = artfSceneAddPlane(s, 1.f, 1.f);
    return s->particleQuad;
}

/* A soft round puff, white, its alpha falling off from the middle: what a particle is when it has no texture. */
static ArtfTexture puffOf(ArtfScene* s) {
    if (s->particlePuff) return s->particlePuff;
    enum { SIDE = 64 };
    uint32_t* pixels = artfMalloc(SIDE * SIDE * 4);
    for (int y = 0; y < SIDE; ++y)
        for (int x = 0; x < SIDE; ++x) {
            const float dx = ((float)x + .5f) / SIDE * 2.f - 1.f, dy = ((float)y + .5f) / SIDE * 2.f - 1.f;
            const float r2 = artfMinf(dx * dx + dy * dy, 1.f), a = (1.f - r2) * (1.f - r2);
            pixels[y * SIDE + x] = (uint32_t)(a * 255.f + .5f) << 24 | 0xFFFFFFu;
        }
    s->particlePuff = artfSceneAddTexture(s, pixels, SIDE, SIDE, 0);
    artfFree(pixels);
    return s->particlePuff;
}

static void hide(ArtfScene* s, ArtfInstance i) {
    ArtfTransform none;
    memset(&none, 0, sizeof none);
    artfInstanceSetTransform(s, i, &none);
}

ArtfEmitter artfSceneAddEmitter(ArtfScene* s, const ArtfEmitterDesc* desc) {
    if (!s->emitters) {
        s->emitters = ARTF_NEW(EmitterList);
        s->emittersFree = emittersFree;
    }
    EmitterData* e = ARTF_NEW(EmitterData);
    e->desc = *desc;
    if (e->desc.most == 0) e->desc.most = 1024;
    if (e->desc.life <= 0) e->desc.life = 2.f;
    e->count = e->desc.most;
    e->state = 0x9E3779B9u ^ (uint32_t)((EmitterList*)s->emitters)->len;
    e->particles = artfCalloc(e->count, sizeof *e->particles);
    e->instances = artfCalloc(e->count, sizeof *e->instances);
    for (int k = 0; k < EMITTER_SHADES; ++k) {
        const float t = (float)k / (float)(EMITTER_SHADES - 1);
        ArtfMaterialDesc m = artfMaterialDefaults();
        const ArtfVec3 color = artfVec3Add(desc->color, artfVec3Scale(artfVec3Sub(desc->colorEnd, desc->color), t));
        m.baseColor = color;
        m.emissive = artfVec3Scale(color, desc->glow);
        m.specular = 0;
        m.doubleSided = true;
        m.texture = desc->texture ? desc->texture : puffOf(s);
        m.alphaMode = ARTF_ALPHA_BLEND;
        e->shades[k] = artfSceneAddMaterial(s, &m);
    }
    const ArtfMesh quad = quadOf(s);
    for (uint32_t i = 0; i < e->count; ++i) {
        e->particles[i].age = -1.f;
        e->instances[i] = artfSceneAddInstance(s, quad, e->shades[0], NULL);
        artfInstanceMarkParticle(s, e->instances[i]);
        hide(s, e->instances[i]);
    }
    artfVecPush((EmitterList*)s->emitters, e);
    return (ArtfEmitter)((EmitterList*)s->emitters)->len;
}

void artfEmitterSet(ArtfScene* s, ArtfEmitter emitter, const ArtfEmitterDesc* desc) {
    EmitterData* e = emitterAt(s, emitter);
    if (!e) return;
    const uint32_t most = e->desc.most;
    e->desc = *desc;
    e->desc.most = most; /* (its particles are made: their number stays) */
    if (e->desc.life <= 0) e->desc.life = 2.f;
}

void artfSceneRemoveEmitter(ArtfScene* s, ArtfEmitter emitter) {
    EmitterData* e = emitterAt(s, emitter);
    if (!e) return;
    for (uint32_t i = 0; i < e->count; ++i) artfSceneRemoveInstance(s, e->instances[i]);
    e->removed = true;
}

/* A particle's transform, facing the eye: the quad's x to the eye's right, its face (+y) toward the eye, its z down the
 * eye's up. */
static void face(ArtfScene* s, ArtfInstance instance, const Particle* p, const ArtfVec3* right, const ArtfVec3* forward, const ArtfVec3* up) {
    ArtfTransform m;
    for (int r = 0; r < 3; ++r) {
        m.m[r * 4] = (&right->x)[r] * p->size;
        m.m[r * 4 + 1] = -(&forward->x)[r] * p->size;
        m.m[r * 4 + 2] = -(&up->x)[r] * p->size;
        m.m[r * 4 + 3] = p->position[r];
    }
    artfInstanceSetTransform(s, instance, &m);
}

static void eyeFrame(const ArtfScene* s, ArtfVec3* right, ArtfVec3* forward, ArtfVec3* up) {
    const ArtfCamera* c = &s->camera;
    *forward = artfVec3Normalize(artfVec3Sub(c->target, c->position));
    *right = artfVec3Normalize(artfVec3Cross(*forward, c->up));
    *up = artfVec3Cross(*right, *forward);
}

void artfSceneFaceParticles(ArtfScene* s) {
    const EmitterList* l = s->emitters;
    if (!l || (!s->particlesMoved && !memcmp(&s->particleCamera, &s->camera, sizeof s->camera))) return;
    s->particlesMoved = false;
    s->particleCamera = s->camera;
    ArtfVec3 right, forward, up;
    eyeFrame(s, &right, &forward, &up);
    for (size_t k = 0; k < l->len; ++k) {
        const EmitterData* e = l->items[k];
        if (e->removed) continue;
        for (uint32_t i = 0; i < e->count; ++i)
            if (e->particles[i].age >= 0) face(s, e->instances[i], &e->particles[i], &right, &forward, &up);
    }
}

void artfEmitterStep(ArtfScene* s, ArtfEmitter emitter, float seconds) {
    EmitterData* e = emitterAt(s, emitter);
    if (!e || seconds < 0) return;
    const ArtfEmitterDesc* d = &e->desc;
    /* born, then moved */
    e->owed += d->rate * seconds;
    for (uint32_t i = 0; i < e->count && e->owed >= 1.f; ++i) {
        Particle* p = &e->particles[i];
        if (p->age >= 0) continue;
        e->owed -= 1.f;
        float stray[3];
        for (int k = 0; k < 3; ++k) stray[k] = randomOf(&e->state) * 2.f - 1.f;
        const float speed = artfVec3Length(d->velocity);
        for (int k = 0; k < 3; ++k) {
            p->position[k] = (&d->position.x)[k] + (randomOf(&e->state) * 2.f - 1.f) * d->area;
            p->velocity[k] = (&d->velocity.x)[k] + stray[k] * d->spread * speed;
        }
        /* (born somewhere within this step, not all at its start) */
        p->age = randomOf(&e->state) * seconds;
    }
    if (e->owed > 1.f) e->owed = 1.f; /* (none free: what was owed is not kept) */
    /* the eye's frame: each faces it (and again as it is drawn, if the eye moved) */
    ArtfVec3 right, forward, up;
    eyeFrame(s, &right, &forward, &up);
    s->particlesMoved = true;
    for (uint32_t i = 0; i < e->count; ++i) {
        Particle* p = &e->particles[i];
        if (p->age < 0) continue;
        p->age += seconds;
        if (p->age >= d->life) {
            p->age = -1.f;
            hide(s, e->instances[i]);
            continue;
        }
        for (int k = 0; k < 3; ++k) {
            p->velocity[k] += (&d->gravity.x)[k] * seconds;
            p->velocity[k] *= 1.f / (1.f + d->drag * seconds);
            p->position[k] += p->velocity[k] * seconds;
        }
        const float t = p->age / d->life;
        p->size = d->size + (d->sizeEnd - d->size) * t;
        face(s, e->instances[i], p, &right, &forward, &up);
        artfInstanceSetMaterial(s, e->instances[i], e->shades[artfMini((int)(t * EMITTER_SHADES), EMITTER_SHADES - 1)]);
        const float opacity = d->opacity + (d->opacityEnd - d->opacity) * t;
        artfInstanceSetOpacity(s, e->instances[i], artfClampf(opacity, 0, 1));
    }
}

uint32_t artfEmitterAlive(const ArtfScene* s, ArtfEmitter emitter) {
    const EmitterData* e = emitterAt(s, emitter);
    uint32_t n = 0;
    for (uint32_t i = 0; e && i < e->count; ++i) n += e->particles[i].age >= 0;
    return n;
}

/* ---- decals ---- */

ArtfDecal artfSceneAddDecal(ArtfScene* s, const ArtfDecalDesc* desc) {
    for (uint32_t i = 0; i < ARTF_MAX_DECALS; ++i)
        if (!s->decalOn[i]) {
            s->decalOn[i] = true;
            s->decals[i] = *desc;
            return i + 1;
        }
    return 0;
}

void artfDecalSet(ArtfScene* s, ArtfDecal d, const ArtfDecalDesc* desc) {
    if (d && d <= ARTF_MAX_DECALS && s->decalOn[d - 1]) s->decals[d - 1] = *desc;
}

void artfSceneRemoveDecal(ArtfScene* s, ArtfDecal d) {
    if (d && d <= ARTF_MAX_DECALS) s->decalOn[d - 1] = false;
}

/* The rows of a transform's inverse: world to box. false: it has none. */
static bool boxOf(const float* m, float* out) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float k0 = e * i - f * h, k1 = c * h - b * i, k2 = b * f - c * e;
    const float det = a * k0 + d * k1 + g * k2;
    if (det == 0) return false;
    const float inv = 1.f / det;
    out[0] = k0 * inv; out[1] = k1 * inv; out[2] = k2 * inv;
    out[4] = (f * g - d * i) * inv; out[5] = (a * i - c * g) * inv; out[6] = (c * d - a * f) * inv;
    out[8] = (d * h - e * g) * inv; out[9] = (b * g - a * h) * inv; out[10] = (a * e - b * d) * inv;
    for (int r = 0; r < 3; ++r) out[r * 4 + 3] = -(out[r * 4] * m[3] + out[r * 4 + 1] * m[7] + out[r * 4 + 2] * m[11]);
    return true;
}

uint32_t artfSceneDecals(const ArtfScene* s, const ArtfFrameData* frame, int height, ArtfDecalData* out) {
    uint32_t n = 0;
    /* how wide a pixel is, one unit along the view */
    const float up = sqrtf(frame->eye[1][0] * frame->eye[1][0] + frame->eye[1][1] * frame->eye[1][1] + frame->eye[1][2] * frame->eye[1][2]);
    const float pixel = 2.f * up / (float)artfMaxi(height, 1);
    for (uint32_t i = 0; i < ARTF_MAX_DECALS; ++i) {
        if (!s->decalOn[i]) continue;
        const ArtfDecalDesc* d = &s->decals[i];
        const float* m = d->transform.m;
        ArtfDecalData o;
        memset(&o, 0, sizeof o);
        if (d->opacity <= 0 || !boxOf(m, o.toBox)) continue;
        /* in view: the box's ball against the frame's planes */
        const float sx = sqrtf(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]), sy = sqrtf(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]), sz = sqrtf(m[2] * m[2] + m[6] * m[6] + m[10] * m[10]);
        const float radius = .5f * sqrtf(sx * sx + sy * sy + sz * sz);
        bool inView = true;
        for (int p = 0; p < 5 && inView; ++p) {
            const float* pl = frame->planes[p];
            inView = pl[0] * m[3] + pl[1] * m[7] + pl[2] * m[11] + pl[3] >= -radius;
        }
        if (!inView) continue;
        const uint32_t texture = d->texture < s->textures.len && !s->textures.items[d->texture].removed ? d->texture : 0;
        const ArtfTextureData* t = &s->textures.items[texture];
        /* the mipmap whose texels are about a pixel's size where the box is: as mipLevel in kernels.inc */
        const float along = artfMaxf((m[3] - frame->cameraPos[0]) * frame->eye[2][0] + (m[7] - frame->cameraPos[1]) * frame->eye[2][1] + (m[11] - frame->cameraPos[2]) * frame->eye[2][2], frame->nearW);
        const float texel = artfMaxf(sx, sz) / (float)artfMaxi(t->width, 1), ratio = along * pixel / artfMaxf(texel, 1e-20f), rho2 = ratio * ratio;
        uint32_t bits;
        memcpy(&bits, &rho2, 4);
        const int e = (int)(bits >> 23) - 127, top = artfMaxi(t->mipCount - 1, 0), first = artfMini(t->residentMip, top);
        const int level = artfClampi((e + 1) >> 1, first, top);
        o.color[0] = d->color.x;
        o.color[1] = d->color.y;
        o.color[2] = d->color.z;
        o.opacity = artfClampf(d->opacity, 0, 1);
        o.texture = texture;
        o.roughness = artfClampf(d->roughness, 0, 1);
        o.level = (float)level;
        o.gpuLevel = (float)(level - first);
        out[n++] = o;
    }
    return n;
}

/* ---- terrain ---- */

ArtfMesh artfSceneAddTerrain(ArtfScene* s, const float* heights, int width, int depth, float cell, float height, float uvRepeat) {
    if (!heights || width < 2 || depth < 2) return 0;
    const uint32_t count = (uint32_t)width * (uint32_t)depth;
    ArtfVertex* v = artfCalloc(count, sizeof *v);
    const float ox = -(float)(width - 1) * cell * .5f, oz = -(float)(depth - 1) * cell * .5f;
#define H(x, z) (heights[(size_t)artfClampi(z, 0, depth - 1) * width + artfClampi(x, 0, width - 1)] * height)
    for (int z = 0; z < depth; ++z)
        for (int x = 0; x < width; ++x) {
            ArtfVertex* o = &v[(size_t)z * width + x];
            o->px = ox + (float)x * cell;
            o->py = H(x, z);
            o->pz = oz + (float)z * cell;
            /* the normal, from the slopes across */
            const ArtfVec3 n = artfVec3Normalize(artfVec3(H(x - 1, z) - H(x + 1, z), 2.f * cell, H(x, z - 1) - H(x, z + 1)));
            o->nx = n.x;
            o->ny = n.y;
            o->nz = n.z;
            o->u = (float)x / (float)(width - 1) * uvRepeat;
            o->v = (float)z / (float)(depth - 1) * uvRepeat;
        }
#undef H
    const size_t quads = (size_t)(width - 1) * (depth - 1);
    uint32_t* idx = artfMalloc(quads * 6 * 4);
    size_t at = 0;
    for (int z = 0; z + 1 < depth; ++z)
        for (int x = 0; x + 1 < width; ++x) {
            const uint32_t a = (uint32_t)(z * width + x), b = a + (uint32_t)width;
            /* (as the plane: counter-clockwise seen from above) */
            idx[at++] = a;
            idx[at++] = b;
            idx[at++] = b + 1;
            idx[at++] = a;
            idx[at++] = b + 1;
            idx[at++] = a + 1;
        }
    const ArtfMesh mesh = artfSceneAddMesh(s, v, count, idx, (uint32_t)at);
    artfFree(v);
    artfFree(idx);
    return mesh;
}
