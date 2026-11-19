/* artful-2d — shadows. Each light that casts keeps, for each direction from it, how far its light goes before an
 * outline stops it: a line of numbers, made by walking each edge of each outline across the directions it spans. The
 * sun's is the same across its light: for each line along it, where the first outline is.
 *
 * Soft as the light is wide, as PCSS does it (Fernando, 2005): a first look either side of a pixel's direction finds
 * how far what is in the way is on average; how far the pixel is past that says how wide its penumbra is; and a share
 * of the directions across that width, each between two of the map's, is how much of the light it sees.
 *
 * An outline's own pixels are lit as if it were not there (the face of a wall the light is on): with the nearest
 * outline in each direction, the map keeps whose it is and the nearest of any other, and a pixel of that outline
 * reads the other. */
#include "2d/internal2d.h"

#include <float.h>

static inline void put(ShadowMap2D* m, int i, float t, uint32_t owner) {
    if (owner == m->owner[i]) {
        if (t < m->near_[i]) m->near_[i] = t;
    } else if (t < m->near_[i]) {
        m->past[i] = m->near_[i];
        m->near_[i] = t;
        m->owner[i] = owner;
    } else if (t < m->past[i])
        m->past[i] = t;
}

/* An edge from a to b, relative to a point light: over each direction whose bin's middle it spans. */
static void edgeAround(ShadowMap2D* m, float ax, float ay, float bx, float by, uint32_t owner, const float* binCos, const float* binSin) {
    const int bins = m->bins;
    const float perBin = (float)bins / (2.f * PI2D);
    const float ta = artf2DAtan2(ay, ax);
    float span = artf2DAtan2(by, bx) - ta;
    if (span > PI2D) span -= 2.f * PI2D;
    else if (span < -PI2D) span += 2.f * PI2D;
    const float ba = (ta + PI2D) * perBin, bb = ba + span * perBin;
    const float lo = artfMinf(ba, bb), hi = artfMaxf(ba, bb);
    const int first = (int)ceilf(lo - .5f), last = (int)floorf(hi - .5f);
    if (last - first >= bins / 2) return; /* (an edge through the light itself) */
    const float ex = bx - ax, ey = by - ay, axe = ax * ey - ay * ex;
    for (int b = first; b <= last; ++b) {
        const int i = b & (bins - 1);
        const float den = binCos[i] * ey - binSin[i] * ex;
        if (den == 0) continue;
        const float t = axe / den;
        if (t > 0) put(m, i, t, owner);
    }
}

/* An edge across the sun's light: u across it, t along it. */
static void edgeAcross(ShadowMap2D* m, float ua, float ta, float ub, float tb, uint32_t owner) {
    if (ub < ua) {
        float s = ua; ua = ub; ub = s;
        s = ta; ta = tb; tb = s;
    }
    const float ba = (ua - m->uMin) * m->binsPerUnit, bb = (ub - m->uMin) * m->binsPerUnit;
    const int first = artfMaxi((int)ceilf(ba - .5f), 0), last = artfMini((int)floorf(bb - .5f), m->bins - 1);
    const float slope = ub > ua ? (tb - ta) / (ub - ua) : 0;
    for (int b = first; b <= last; ++b) {
        const float u = m->uMin + ((float)b + .5f) / m->binsPerUnit;
        put(m, b, ta + (u - ua) * slope, owner);
    }
}

void artf2DShadowBuild(Frame2D* f, Lamp2D* lamp, const float* binCos, const float* binSin) {
    ShadowMap2D* m = lamp->shadow;
    for (int i = 0; i < m->bins; ++i) {
        m->near_[i] = m->past[i] = FLT_MAX;
        m->owner[i] = 0;
    }
    const bool sun = lamp->kind == ARTF_LIGHT2D_SUN;
    /* (the sun's light goes along (gx, gy); across it is (-gy, gx)) */
    const float gx = -lamp->dirX, gy = -lamp->dirY;
    const float planar = sqrtf(gx * gx + gy * gy);
    const float ax_ = planar > 0 ? gx / planar : 1, ay_ = planar > 0 ? gy / planar : 0;
    for (size_t o = 0; o < f->outlines.len; ++o) {
        const Outline2D* ol = &f->outlines.items[o];
        if (!sun) {
            const float cx = artfClampf(lamp->x, ol->x0, ol->x1) - lamp->x, cy = artfClampf(lamp->y, ol->y0, ol->y1) - lamp->y;
            if (cx * cx + cy * cy >= lamp->range2) continue;
        }
        const ArtfVec2* p = f->outlinePoints.items + ol->start;
        const int edges = ol->closed ? ol->count : ol->count - 1;
        for (int e = 0; e < edges; ++e) {
            const ArtfVec2 a = p[e], b = p[(e + 1) % ol->count];
            if (sun) edgeAcross(m, -ay_ * a.x + ax_ * a.y, ax_ * a.x + ay_ * a.y, -ay_ * b.x + ax_ * b.y, ax_ * b.x + ay_ * b.y, ol->owner);
            else edgeAround(m, a.x - lamp->x, a.y - lamp->y, b.x - lamp->x, b.y - lamp->y, ol->owner, binCos, binSin);
        }
    }
}

/* Lit (1) or not (0), at bin i, for a pixel at depth d of the outline owner. */
static inline float litAt(const ShadowMap2D* m, int i, bool wraps, float d, uint32_t owner) {
    if (wraps) i &= m->bins - 1; /* (a lamp's bins are a power of two) */
    else if (i < 0 || i >= m->bins) return 1;
    const float t = owner && m->owner[i] == owner ? m->past[i] : m->near_[i];
    return t >= d ? 1.f : 0.f;
}

static inline float depthAt(const ShadowMap2D* m, int i, bool wraps, uint32_t owner) {
    if (wraps) i &= m->bins - 1; /* (a lamp's bins are a power of two) */
    else if (i < 0 || i >= m->bins) return FLT_MAX;
    return owner && m->owner[i] == owner ? m->past[i] : m->near_[i];
}

/* Between two bins. */
static inline float litBetween(const ShadowMap2D* m, float x, bool wraps, float d, uint32_t owner) {
    const float fl = floorf(x), w = x - fl;
    const int i = (int)fl;
    return litAt(m, i, wraps, d, owner) * (1.f - w) + litAt(m, i + 1, wraps, d, owner) * w;
}

enum { SEARCH = 8, FILTER = 16 };

float artf2DShadowSee(const Lamp2D* lamp, float px, float py, uint32_t owner) {
    const ShadowMap2D* m = lamp->shadow;
    const bool sun = lamp->kind == ARTF_LIGHT2D_SUN;
    float c, d, perUnit; /* the pixel's place in bins, its depth, and bins for a pixel across at its depth */
    if (sun) {
        const float gx = -lamp->dirX, gy = -lamp->dirY, planar = sqrtf(gx * gx + gy * gy);
        if (planar <= 0) return 1;
        const float ax = gx / planar, ay = gy / planar;
        c = (-ay * px + ax * py - m->uMin) * m->binsPerUnit - .5f;
        d = ax * px + ay * py;
        perUnit = m->binsPerUnit;
    } else {
        const float vx = px - lamp->x, vy = py - lamp->y;
        d = sqrtf(vx * vx + vy * vy);
        const float perRadian = (float)m->bins / (2.f * PI2D);
        c = (artf2DAtan2(vy, vx) + PI2D) * perRadian - .5f;
        perUnit = d > 0 ? perRadian / d : perRadian;
    }
    const float bias = 1.5f + (sun ? 0 : d * (2.f * PI2D / (float)m->bins));
    const float dd = d - bias;
    const bool wraps = !sun;
    if (lamp->radius <= 0) return litBetween(m, c, wraps, dd, owner);

    /* how wide the light looks: the sun's angle, or a lamp's width over the distance */
    const float limit = (float)m->bins * .125f;
    const float reach = sun ? lamp->radius * d * perUnit : lamp->radius * perUnit;
    const float search = artfClampf(reach, 1.f, limit);
    float sum = 0;
    int blockers = 0;
    for (int k = 0; k < SEARCH; ++k) {
        const float x = c + search * (((float)k + .5f) * (2.f / SEARCH) - 1.f);
        const float t = depthAt(m, (int)floorf(x + .5f), wraps, owner);
        if (t < dd) {
            sum += t;
            ++blockers;
        }
    }
    if (!blockers) return litBetween(m, c, wraps, dd, owner);
    const float blocker = artfMaxf(sum / (float)blockers, 1.f);
    /* (a lamp of radius r, an edge at b and a pixel at d: the penumbra is r (d - b) / b across, at d) */
    const float wide = sun ? lamp->radius * (d - blocker) * perUnit : lamp->radius * (d - blocker) / blocker * perUnit;
    const float half = artfClampf(wide, .5f, limit);
    float seen = 0;
    for (int k = 0; k < FILTER; ++k) seen += litBetween(m, c + half * (((float)k + .5f) * (2.f / FILTER) - 1.f), wraps, dd, owner);
    return seen * (1.f / FILTER);
}
