/* artful — what is made of the scene's own parts: emitters of particles, and terrain from heights. Nothing here is a
 * backend's: particles are instances of one quad, turned to face the eye at each step, and terrain is a mesh; so both
 * draw them, and the same way. */
#include "scene/sceneInternal.h"

#include <string.h>

/* ---- particles ---- */

#define EMITTER_SHADES 8 /* materials between a particle's first color and its last */

typedef struct Particle {
    float position[3], velocity[3];
    float age;       /* seconds; < 0: not alive */
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
    /* the eye's frame: each faces it */
    const ArtfCamera* c = &s->camera;
    const ArtfVec3 forward = artfVec3Normalize(artfVec3Sub(c->target, c->position));
    const ArtfVec3 right = artfVec3Normalize(artfVec3Cross(forward, c->up));
    const ArtfVec3 up = artfVec3Cross(right, forward);
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
        const float size = d->size + (d->sizeEnd - d->size) * t;
        /* the quad's x to the eye's right, its face (+y) toward the eye, its z down the eye's up */
        ArtfTransform m;
        for (int r = 0; r < 3; ++r) {
            m.m[r * 4] = (&right.x)[r] * size;
            m.m[r * 4 + 1] = -(&forward.x)[r] * size;
            m.m[r * 4 + 2] = -(&up.x)[r] * size;
            m.m[r * 4 + 3] = p->position[r];
        }
        artfInstanceSetTransform(s, e->instances[i], &m);
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
