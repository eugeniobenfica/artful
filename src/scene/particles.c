/* artful — emitters of many particles (ArtfEmitterDesc.parallel), as Niagara's GPU emitters: a buffer of particles, not
 * instances, each moved alone, so that all can be moved at once, by a compute shader (shaders.c's artfShaderSwarm) or
 * on the CPU's workers, with the same arithmetic.
 *
 * What makes them the same everywhere is that nothing depends on order. The scene decides, at each step, where those
 * born go: places one after another around the buffer, from where the last went, as many as are free (it knows when
 * each dies, without the particles). A particle's random numbers come from its birth's number by a hash. Each step is
 * kept as rows of numbers; what draws applies the steps it has not seen yet to its own copy of the particles, and turns
 * each to face the eye as an instance of the quad, through the materials of its colors, which all backends draw as any
 * other layer. */
#include "scene/sceneInternal.h"
#include "core/threads.h"

#include <stdlib.h>

struct ArtfSwarm {
    ArtfEmitterDesc desc;
    uint32_t places, seed, material, quad;
    double clock;          /* seconds stepped */
    double* deathAt;       /* by place: when what is there dies */
    uint32_t head;         /* where the next is born */
    uint32_t births;       /* how many were born, ever (a 32-bit number, wrapping) */
    float owed;
    ARTF_VEC(float) rows;  /* ARTF_SWARM_ROWS vec4s for each step kept, the oldest first */
    uint64_t stepCount, oldest;
    float* cpu;            /* the CPU's copy, at cpuAt steps */
    uint64_t cpuAt;
};

#define STEP_FLOATS (ARTF_SWARM_ROWS * 4)

static uint32_t hashOf(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/* The k-th random number of a birth, 0 to 1: integers only, so every processor and the GPU agree. */
static float randomAt(uint32_t seed, uint32_t birth, uint32_t k) { return (float)(hashOf(seed ^ hashOf(birth * 16u + k)) >> 8) * (1.f / 16777216.f); }

static uint32_t bitsOf(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static float floatOf(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* ---- a wind that swirls: as extras.c's, the same everywhere ---- */

static float latticeOf(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    uint32_t h = (uint32_t)x * 0x8DA6B343u ^ (uint32_t)y * 0xD8163841u ^ (uint32_t)z * 0xCB1AB31Fu ^ seed;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return (float)(h >> 8) * (2.f / 16777216.f) - 1.f;
}

static float noiseOf(float x, float y, float z, uint32_t seed) {
    const float fx = (float)(int32_t)(x + 65536.f) - 65536.f, fy = (float)(int32_t)(y + 65536.f) - 65536.f, fz = (float)(int32_t)(z + 65536.f) - 65536.f;
    const int32_t ix = (int32_t)fx, iy = (int32_t)fy, iz = (int32_t)fz;
    float tx = x - fx, ty = y - fy, tz = z - fz;
    tx = tx * tx * (3.f - 2.f * tx);
    ty = ty * ty * (3.f - 2.f * ty);
    tz = tz * tz * (3.f - 2.f * tz);
    float c[2][2];
    for (int j = 0; j < 2; ++j)
        for (int k = 0; k < 2; ++k) {
            const float a = latticeOf(ix, iy + j, iz + k, seed), b = latticeOf(ix + 1, iy + j, iz + k, seed);
            c[j][k] = a + (b - a) * tx;
        }
    const float near_ = c[0][0] + (c[1][0] - c[0][0]) * ty, far = c[0][1] + (c[1][1] - c[0][1]) * ty;
    return near_ + (far - near_) * tz;
}

/* ---- one particle ---- */

/* Rows of a step: 0 where they are born, give or take area; 1 how they leave, give or take spread times its speed;
 * 2 gravity, drag; 3 seconds, life, turbulence, 1 / its size; 4 ground (1 or 0), its height, bounce, what friction
 * leaves; 5 spin, and as bits the first place born, how many, the first's birth; 6 as bits places, seed. */
void artfSwarmMove(float* p, const float* step, uint32_t place) {
    const uint32_t head = bitsOf(step[21]), count = bitsOf(step[22]), first = bitsOf(step[23]), places = bitsOf(step[24]), seed = bitsOf(step[25]);
    const float seconds = step[12];
    const uint32_t j = (place + places - head) % places;
    if (j < count) {
        const uint32_t birth = first + j;
        float r[9];
        for (uint32_t k = 0; k < 9; ++k) r[k] = randomAt(seed, birth, k);
        for (int k = 0; k < 3; ++k) {
            p[k] = step[k] + (r[3 + k] * 2.f - 1.f) * step[3];
            p[4 + k] = step[4 + k] + (r[k] * 2.f - 1.f) * step[7];
        }
        /* (born somewhere within this step, not all at its start) */
        p[3] = r[6] * seconds;
        const float spin = step[20];
        p[7] = spin != 0 ? r[7] * 6.28318531f : 0.f;
        p[8] = spin != 0 ? (r[8] * 2.f - 1.f) * spin : 0.f;
        p[9] = floatOf(birth);
        p[10] = p[11] = 0;
    }
    if (!(p[3] >= 0)) return;
    p[3] += seconds;
    if (p[3] >= step[13]) {
        p[3] = -1.f;
        return;
    }
    if (step[14] != 0) {
        /* (three noises of the place, offset from each other: a push that varies smoothly, and swirls) */
        const float x = p[0] * step[15], y = p[1] * step[15], z = p[2] * step[15];
        for (int k = 0; k < 3; ++k) p[4 + k] += noiseOf(x + 31.4f * (float)k, y, z, 0x51ED270Bu + (uint32_t)k) * step[14] * seconds;
    }
    const float slow = 1.f / (1.f + step[11] * seconds);
    for (int k = 0; k < 3; ++k) {
        p[4 + k] += step[8 + k] * seconds;
        p[4 + k] *= slow;
        p[k] += p[4 + k] * seconds;
    }
    if (step[16] != 0 && p[1] < step[17]) {
        /* off the ground: what went into it comes back, as much as it bounces; along it, less its friction */
        p[1] = step[17] + (step[17] - p[1]) * step[18];
        if (p[5] < 0) p[5] = -p[5] * step[18];
        p[4] *= step[19];
        p[6] *= step[19];
    }
    p[7] += p[8] * seconds;
}

/* Rows of facing: 0 the eye's right, the size when born; 1 where it looks, the size when it dies; 2 its up, the life;
 * 3 the opacity when born and when it dies, and as bits the first material and the quad. */
void artfSwarmFace(const float* particles, const float* face, uint32_t first, uint32_t end, ArtfInstanceData* out) {
    const uint32_t material = bitsOf(face[14]), quad = bitsOf(face[15]);
    for (uint32_t k = first; k < end; ++k) {
        const float* p = particles + (size_t)k * ARTF_SWARM_FLOATS;
        ArtfInstanceData* o = &out[k - first];
        memset(o, 0, sizeof *o);
        o->mesh = quad;
        o->material = material;
        o->flags = ARTF_INSTANCE_UNIFORM | ARTF_INSTANCE_CUTOUT | ARTF_INSTANCE_PARTICLE;
        o->translucent = 1;
        if (!(p[3] >= 0)) continue;
        const float t = p[3] / face[11];
        const float size = face[3] + (face[7] - face[3]) * t;
        const float c = p[7] != 0 ? artfCosFast(p[7]) : 1.f, sn = p[7] != 0 ? artfSinFast(p[7]) : 0.f;
        /* the quad's x to the eye's right, its face (+y) toward the eye, its z down the eye's up; turned by its angle */
        for (int r = 0; r < 3; ++r) {
            const float x = face[r], z = face[8 + r];
            o->m[r * 4] = (x * c + z * sn) * size;
            o->m[r * 4 + 1] = -face[4 + r] * size;
            o->m[r * 4 + 2] = -(z * c - x * sn) * size;
            o->m[r * 4 + 3] = p[r];
        }
        const int shade = (int)(t * (float)ARTF_SWARM_SHADES);
        o->material = material + (uint32_t)artfClampi(shade, 0, ARTF_SWARM_SHADES - 1);
        o->maxScale = fabsf(size);
        const float opacity = artfClampf(face[12] + (face[13] - face[12]) * t, 0, 1);
        o->opacity = (uint16_t)(opacity * 65535.f + .5f);
    }
}

/* ---- an emitter ---- */

ArtfSwarm* artfSwarmNew(ArtfScene* s, const ArtfEmitterDesc* desc, uint32_t index, uint32_t material, uint32_t quad) {
    (void)s;
    ArtfSwarm* w = ARTF_NEW(ArtfSwarm);
    w->desc = *desc;
    if (w->desc.life <= 0) w->desc.life = 2.f;
    w->places = desc->most ? desc->most : 1024;
    w->seed = hashOf(0x9E3779B9u ^ index * 0x85EBCA6Bu);
    w->material = material;
    w->quad = quad;
    w->deathAt = artfCalloc(w->places, sizeof *w->deathAt);
    for (uint32_t i = 0; i < w->places; ++i) w->deathAt[i] = -1.0;
    return w;
}

void artfSwarmFree(ArtfSwarm* w) {
    if (!w) return;
    artfFree(w->deathAt);
    artfVecFree(&w->rows);
    artfFree(w->cpu);
    artfFree(w);
}

void artfSwarmSet(ArtfSwarm* w, const ArtfEmitterDesc* desc) {
    w->desc = *desc;
    w->desc.most = w->places;
    w->desc.parallel = true;
    if (w->desc.life <= 0) w->desc.life = 2.f;
}

void artfSwarmStep(ArtfScene* s, ArtfSwarm* w, float seconds, uint32_t burst) {
    (void)s;
    if (!(seconds >= 0)) return;
    const ArtfEmitterDesc* d = &w->desc;
    /* where they are born: the places that are free, one after another from the last */
    w->owed += d->rate * seconds + (float)burst;
    uint32_t count = 0;
    while (count < w->places && w->owed >= 1.f && w->deathAt[(w->head + count) % w->places] <= w->clock) {
        ++count;
        w->owed -= 1.f;
    }
    if (w->owed > 1.f) w->owed = 1.f; /* (none free: what was owed is not kept) */
    const float speed = artfVec3Length(d->velocity);
    const float rows[STEP_FLOATS] = {
        d->position.x, d->position.y, d->position.z, d->area,
        d->velocity.x, d->velocity.y, d->velocity.z, d->spread * speed,
        d->gravity.x, d->gravity.y, d->gravity.z, d->drag,
        seconds, d->life, d->turbulence, 1.f / (d->turbulenceSize > 0 ? d->turbulenceSize : 1.f),
        d->ground ? 1.f : 0.f, d->groundHeight, artfClampf(d->bounce, 0, 1), 1.f - artfClampf(d->friction, 0, 1),
        d->spin, floatOf(w->head), floatOf(count), floatOf(w->births),
        floatOf(w->places), floatOf(w->seed), 0, 0,
        0, 0, 0, 0};
    /* when each dies: what its age will be at the end of this step, against its life */
    for (uint32_t j = 0; j < count; ++j) {
        const float age = randomAt(w->seed, w->births + j, 6) * seconds + seconds;
        w->deathAt[(w->head + j) % w->places] = w->clock + seconds + ((double)d->life - (double)age);
    }
    w->head = (w->head + count) % w->places;
    w->births += count;
    w->clock += seconds;
    /* kept for what draws, up to so many */
    if (w->stepCount - w->oldest >= ARTF_SWARM_STEPS) {
        memmove(w->rows.items, w->rows.items + STEP_FLOATS, (w->rows.len - STEP_FLOATS) * sizeof(float));
        w->rows.len -= STEP_FLOATS;
        ++w->oldest;
    }
    artfVecReserve(&w->rows, w->rows.len + STEP_FLOATS);
    memcpy(w->rows.items + w->rows.len, rows, sizeof rows);
    w->rows.len += STEP_FLOATS;
    ++w->stepCount;
}

uint32_t artfSwarmAlive(const ArtfSwarm* w) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < w->places; ++i) n += w->deathAt[i] > w->clock;
    return n;
}

uint32_t artfSwarmPlaces(const ArtfSwarm* w) { return w->places; }
uint64_t artfSwarmStepCount(const ArtfSwarm* w) { return w->stepCount; }
uint64_t artfSwarmOldest(const ArtfSwarm* w) { return w->oldest; }
const float* artfSwarmStepRows(const ArtfSwarm* w, uint64_t step) { return w->rows.items + (size_t)(step - w->oldest) * STEP_FLOATS; }
uint32_t artfSwarmMaterial(const ArtfSwarm* w) { return w->material; }

void artfSwarmFaceRows(const ArtfScene* s, const ArtfSwarm* w, float* rows) {
    const ArtfCamera* c = &s->camera;
    const ArtfVec3 forward = artfVec3Normalize(artfVec3Sub(c->target, c->position));
    const ArtfVec3 right = artfVec3Normalize(artfVec3Cross(forward, c->up));
    const ArtfVec3 up = artfVec3Cross(right, forward);
    const ArtfEmitterDesc* d = &w->desc;
    const float out[16] = {right.x, right.y, right.z, d->size, forward.x, forward.y, forward.z, d->sizeEnd, up.x, up.y, up.z, d->life,
                           d->opacity, d->opacityEnd, floatOf(w->material), floatOf(w->quad)};
    memset(rows, 0, STEP_FLOATS * sizeof *rows);
    memcpy(rows, out, sizeof out);
}

/* ---- the CPU's copy ---- */

typedef struct CpuWork { ArtfSwarm* w; uint64_t from; bool fresh; uint32_t jobs; } CpuWork;

static void cpuJob(void* user, uint32_t index, uint32_t worker) {
    (void)worker;
    const CpuWork* c = user;
    ArtfSwarm* w = c->w;
    const uint32_t first = (uint32_t)((uint64_t)w->places * index / c->jobs), end = (uint32_t)((uint64_t)w->places * (index + 1) / c->jobs);
    for (uint32_t place = first; place < end; ++place) {
        float* p = w->cpu + (size_t)place * ARTF_SWARM_FLOATS;
        if (c->fresh) {
            memset(p, 0, ARTF_SWARM_FLOATS * sizeof *p);
            p[3] = -1.f;
        }
        for (uint64_t step = c->from; step < w->stepCount; ++step) artfSwarmMove(p, artfSwarmStepRows(w, step), place);
    }
}

const float* artfSwarmCpu(ArtfSwarm* w, ArtfPool* pool) {
    const bool fresh = !w->cpu || w->cpuAt < w->oldest;
    if (!w->cpu) w->cpu = artfMalloc((size_t)w->places * ARTF_SWARM_FLOATS * sizeof *w->cpu);
    if (fresh || w->cpuAt < w->stepCount) {
        CpuWork c = {w, fresh ? w->oldest : w->cpuAt, fresh, 64};
        if (pool) artfPoolRun(pool, cpuJob, &c, c.jobs);
        else for (uint32_t i = 0; i < c.jobs; ++i) cpuJob(&c, i, 0);
        w->cpuAt = w->stepCount;
    }
    return w->cpu;
}
