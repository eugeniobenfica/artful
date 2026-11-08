/* artful — lighter versions of a mesh, for what is far away. Edge collapse by quadric error (Garland
 * and Heckbert, 1997), keeping the vertices that are left as they are: a collapse moves one vertex
 * onto another, so that no new vertex, normal or texture coordinate has to be made up.
 *
 * A vertex on an edge that only one triangle has, counting vertices at one place as one, stays where it is:
 * that holds the mesh's borders. So does a vertex that is split, where a texture or a hard edge makes two or
 * more at one place. Seams are the exception: two vertices at one place, with the same on the next along the
 * cut, go together: both sides collapse along it at once, onto vertices that are also at one place, so
 * the cut stays closed however far it goes. */
#include "scene/simplifyInternal.h"

#include <stdlib.h>

typedef struct Quadric { double a2, ab, ac, ad, b2, bc, bd, c2, cd, d2, weight; } Quadric;

typedef struct Candidate {
    float cost;
    uint32_t from, to;
    uint32_t fromVersion, toVersion;
} Candidate;

typedef struct Simplifier {
    uint32_t vertexCount, triangleCount;
    double* positions;   /* in a box of size 1: the error is then a fraction of the mesh */
    Quadric* quadrics;
    uint32_t* corners;   /* the vertex at each corner of each triangle */
    uint32_t* heads;     /* of each vertex: its first corner + 1 */
    uint32_t* tails;     /* and its last */
    uint32_t* nexts;     /* of each corner: the next of its vertex + 1 */
    uint8_t* triangleDead;
    uint8_t* vertexDead;
    uint8_t* locked;
    uint32_t* twins;     /* the other vertex at this place, from 1, on a seam; 0: none */
    uint32_t* places;    /* the first vertex at the same place */
    uint32_t* into;      /* the vertex this one has become, from 1; 0: itself */
    uint32_t* versions;
    uint32_t* marks;     /* which search last met this vertex */
    float* errors;       /* how far what this vertex stands for may have moved */
    uint32_t stamp;
    ARTF_VEC(Candidate) heap;
} Simplifier;

static void quadricAdd(Quadric* q, const Quadric* o) {
    q->a2 += o->a2; q->ab += o->ab; q->ac += o->ac; q->ad += o->ad; q->b2 += o->b2;
    q->bc += o->bc; q->bd += o->bd; q->c2 += o->c2; q->cd += o->cd; q->d2 += o->d2;
    q->weight += o->weight;
}

/* The squared distance to the planes it holds, on average. */
static double quadricError(const Quadric* q, const double* p) {
    const double x = p[0], y = p[1], z = p[2];
    const double e = q->a2 * x * x + 2 * q->ab * x * y + 2 * q->ac * x * z + 2 * q->ad * x + q->b2 * y * y + 2 * q->bc * y * z + 2 * q->bd * y + q->c2 * z * z + 2 * q->cd * z + q->d2;
    return (e > 0 ? e : 0) / (q->weight > 1e-20 ? q->weight : 1e-20);
}

/* ---- a heap of the cheapest collapses ---- */

static bool cheaper(const Candidate* a, const Candidate* b) {
    if (a->cost != b->cost) return a->cost < b->cost;
    return a->from != b->from ? a->from < b->from : a->to < b->to; /* (the same result on every system) */
}

static void heapPush(Simplifier* s, Candidate c) {
    artfVecPush(&s->heap, c);
    Candidate* h = s->heap.items;
    size_t i = s->heap.len - 1;
    while (i && cheaper(&c, &h[(i - 1) / 2])) {
        h[i] = h[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    h[i] = c;
}

static Candidate heapPop(Simplifier* s) {
    Candidate* h = s->heap.items;
    const Candidate top = h[0], last = h[--s->heap.len];
    const size_t n = s->heap.len;
    size_t i = 0;
    for (;;) {
        size_t child = i * 2 + 1;
        if (child >= n) break;
        if (child + 1 < n && cheaper(&h[child + 1], &h[child])) ++child;
        if (!cheaper(&h[child], &last)) break;
        h[i] = h[child];
        i = child;
    }
    if (n) h[i] = last;
    return top;
}

#define EACH_CORNER(s, v, c) for (uint32_t c##_ = (s)->heads[v], c; c##_ && (c = c##_ - 1, 1); c##_ = (s)->nexts[c])

/* Does only one triangle have this edge, now? */
static bool borderEdge(const Simplifier* s, uint32_t a, uint32_t b) {
    int triangles = 0;
    EACH_CORNER(s, a, c) {
        const uint32_t t = c / 3;
        if (s->triangleDead[t]) continue;
        for (int k = 0; k < 3; ++k) triangles += s->corners[t * 3 + k] == b;
    }
    return triangles == 1;
}

/* An edge along a cut, with its other side. */
static bool seamEdge(const Simplifier* s, uint32_t a, uint32_t b) {
    if (!s->twins[a] || !s->twins[b]) return false;
    return borderEdge(s, a, b) && borderEdge(s, s->twins[a] - 1, s->twins[b] - 1);
}

/* What moving one end of an edge onto the other costs, the cheaper way round. */
static void pushEdge(Simplifier* s, uint32_t a, uint32_t b) {
    const bool seam = s->locked[a] && s->locked[b] && seamEdge(s, a, b);
    const bool canA = !s->locked[a] || seam, canB = !s->locked[b] || seam;
    if (!canA && !canB) return;
    Quadric q = s->quadrics[a];
    quadricAdd(&q, &s->quadrics[b]);
    const double toB = canA ? quadricError(&q, &s->positions[(size_t)b * 3]) : 1e300;
    const double toA = canB ? quadricError(&q, &s->positions[(size_t)a * 3]) : 1e300;
    const uint32_t from = toB <= toA ? a : b, to = toB <= toA ? b : a;
    const Candidate c = {(float)(toB <= toA ? toB : toA), from, to, s->versions[from], s->versions[to]};
    heapPush(s, c);
}

/* Would this collapse keep the surface a surface? An edge has two triangles, or one on a border: a third
 * vertex that both ends see, besides those, would pinch it. And no triangle may turn over. */
static bool collapseAllowed(Simplifier* s, uint32_t from, uint32_t to, int sides) {
    const uint32_t stamp = ++s->stamp;
    EACH_CORNER(s, from, c) {
        const uint32_t t = c / 3;
        if (s->triangleDead[t]) continue;
        for (int k = 0; k < 3; ++k) s->marks[s->corners[t * 3 + k]] = stamp;
    }
    const uint32_t seen = ++s->stamp;
    int shared = 0;
    EACH_CORNER(s, to, c) {
        const uint32_t t = c / 3;
        if (s->triangleDead[t]) continue;
        for (int k = 0; k < 3; ++k) {
            const uint32_t v = s->corners[t * 3 + k];
            if (v != from && v != to && s->marks[v] == stamp) { s->marks[v] = seen; ++shared; }
        }
    }
    if (shared > sides) return false;

    const double* target = &s->positions[(size_t)to * 3];
    EACH_CORNER(s, from, c) {
        const uint32_t t = c / 3;
        if (s->triangleDead[t]) continue;
        const uint32_t k = c % 3, v1 = s->corners[t * 3 + (k + 1) % 3], v2 = s->corners[t * 3 + (k + 2) % 3];
        if (v1 == to || v2 == to) continue; /* (this one goes) */
        const double *p0 = &s->positions[(size_t)from * 3], *p1 = &s->positions[(size_t)v1 * 3], *p2 = &s->positions[(size_t)v2 * 3];
        double e1[3], e2[3], f1[3], n[3], m[3];
        for (int i = 0; i < 3; ++i) { e1[i] = p1[i] - p0[i]; e2[i] = p2[i] - p0[i]; f1[i] = p1[i] - target[i]; }
        double f2[3];
        for (int i = 0; i < 3; ++i) f2[i] = p2[i] - target[i];
        n[0] = e1[1] * e2[2] - e1[2] * e2[1]; n[1] = e1[2] * e2[0] - e1[0] * e2[2]; n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        m[0] = f1[1] * f2[2] - f1[2] * f2[1]; m[1] = f1[2] * f2[0] - f1[0] * f2[2]; m[2] = f1[0] * f2[1] - f1[1] * f2[0];
        const double dot = n[0] * m[0] + n[1] * m[1] + n[2] * m[2];
        const double nn = n[0] * n[0] + n[1] * n[1] + n[2] * n[2], mm = m[0] * m[0] + m[1] * m[1] + m[2] * m[2];
        if (mm < 1e-24 || dot * dot < .04 * nn * mm || dot < 0) return false; /* (turned by more than about 78 degrees) */
    }
    return true;
}

/* How far the surface moved at this collapse: from's distance to the nearest plane of the triangles
 * that take its place. A quadric's cost cannot say this: it is a mean, weighed by area, and comes out
 * far below what can be seen. What from already stood for adds to it, so the error of a level holds
 * for all the collapses that led to it. */
static float collapseError(const Simplifier* s, uint32_t from, uint32_t to) {
    const double* p = &s->positions[(size_t)from * 3];
    const double* q = &s->positions[(size_t)to * 3];
    double nearest = 1e300;
    for (uint32_t link = s->heads[from]; link; link = s->nexts[link - 1]) {
        const uint32_t c = link - 1, t = c / 3;
        if (s->triangleDead[t]) continue;
        const uint32_t k = c % 3, v1 = s->corners[t * 3 + (k + 1) % 3], v2 = s->corners[t * 3 + (k + 2) % 3];
        if (v1 == to || v2 == to) continue;
        const double *a = &s->positions[(size_t)v1 * 3], *b = &s->positions[(size_t)v2 * 3];
        const double ux = a[0] - q[0], uy = a[1] - q[1], uz = a[2] - q[2], vx = b[0] - q[0], vy = b[1] - q[1], vz = b[2] - q[2];
        const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const double length = sqrt(nx * nx + ny * ny + nz * nz);
        if (length <= 0) continue;
        const double d = fabs(nx * (p[0] - q[0]) + ny * (p[1] - q[1]) + nz * (p[2] - q[2])) / length;
        nearest = d < nearest ? d : nearest;
    }
    if (nearest >= 1e300) nearest = sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
    return s->errors[from] + (float)nearest;
}

/* Returns how many triangles went. */
static uint32_t collapse(Simplifier* s, uint32_t from, uint32_t to) {
    uint32_t gone = 0;
    const float moved = collapseError(s, from, to);
    s->errors[to] = artfMaxf(s->errors[to], moved);
    EACH_CORNER(s, from, c) {
        const uint32_t t = c / 3;
        if (s->triangleDead[t]) continue;
        bool both = false;
        for (int k = 0; k < 3; ++k) both = both || s->corners[t * 3 + k] == to;
        if (both) { s->triangleDead[t] = 1; ++gone; }
        else s->corners[c] = to;
    }
    /* from's corners join to's */
    if (s->heads[from]) {
        if (s->tails[to]) s->nexts[s->tails[to] - 1] = s->heads[from];
        else s->heads[to] = s->heads[from];
        s->tails[to] = s->tails[from];
    }
    s->vertexDead[from] = 1;
    quadricAdd(&s->quadrics[to], &s->quadrics[from]);
    ++s->versions[to];

    const uint32_t stamp = ++s->stamp;
    s->marks[to] = stamp;
    EACH_CORNER(s, to, c) {
        const uint32_t t = c / 3;
        if (s->triangleDead[t]) continue;
        for (int k = 0; k < 3; ++k) {
            const uint32_t v = s->corners[t * 3 + k];
            if (s->marks[v] != stamp) { s->marks[v] = stamp; pushEdge(s, to, v); }
        }
    }
    return gone;
}

/* How far a vertex of a cut is from the stretch that takes its place: planes do not see what a cut does. */
static double seamError(const Simplifier* s, uint32_t from, uint32_t to) {
    uint32_t next = UINT32_MAX;
    EACH_CORNER(s, from, c) {
        const uint32_t t = c / 3;
        if (s->triangleDead[t]) continue;
        for (int k = 0; k < 3; ++k) {
            const uint32_t v = s->corners[t * 3 + k];
            if (v != from && v != to && borderEdge(s, from, v)) next = v;
        }
    }
    if (next == UINT32_MAX) return 0;
    const double *p = &s->positions[(size_t)from * 3], *a = &s->positions[(size_t)to * 3], *b = &s->positions[(size_t)next * 3];
    double along = 0, length = 0, d[3];
    for (int i = 0; i < 3; ++i) {
        d[i] = b[i] - a[i];
        along += (p[i] - a[i]) * d[i];
        length += d[i] * d[i];
    }
    const double t = length > 0 ? (along / length < 0 ? 0 : along / length > 1 ? 1 : along / length) : 0;
    double far = 0;
    for (int i = 0; i < 3; ++i) far += (p[i] - a[i] - d[i] * t) * (p[i] - a[i] - d[i] * t);
    return sqrt(far);
}

typedef struct Place { double p[3]; uint32_t vertex; } Place;

static int comparePlaces(const void* a, const void* b) {
    const Place *x = a, *y = b;
    for (int i = 0; i < 3; ++i)
        if (x->p[i] != y->p[i]) return x->p[i] < y->p[i] ? -1 : 1;
    return x->vertex < y->vertex ? -1 : x->vertex > y->vertex;
}

/* What is at one place. Two go along a seam. More than two that face the same way are a point all around which texture
 * coordinates turn, as at a sphere's poles: they become one, which stays. Others stay as they are. */
static void findTwins(Simplifier* s, const ArtfVertex* vertices, uint32_t cornerCount) {
    Place* places = artfMalloc(((size_t)s->vertexCount + 1) * sizeof *places);
    const uint32_t n = s->vertexCount;
    for (uint32_t v = 0; v < n; ++v) {
        memcpy(places[v].p, &s->positions[(size_t)v * 3], 24);
        places[v].vertex = v;
    }
    qsort(places, n, sizeof *places, comparePlaces);
    for (uint32_t i = 0; i < n;) {
        uint32_t j = i + 1;
        while (j < n && !memcmp(places[j].p, places[i].p, 24)) ++j;
        for (uint32_t k = i; k < j; ++k) {
            s->places[places[k].vertex] = places[i].vertex;
            if (j - i > 1) s->locked[places[k].vertex] = 1;
        }
        if (j - i == 2) {
            s->twins[places[i].vertex] = places[i + 1].vertex + 1;
            s->twins[places[i + 1].vertex] = places[i].vertex + 1;
        }
        bool same = j - i > 2;
        const ArtfVertex* first = &vertices[places[i].vertex];
        for (uint32_t k = i + 1; k < j && same; ++k) {
            const ArtfVertex* v = &vertices[places[k].vertex];
            const float dx = v->nx - first->nx, dy = v->ny - first->ny, dz = v->nz - first->nz;
            same = dx * dx + dy * dy + dz * dz < 1e-4f;
        }
        for (uint32_t k = i; k < j && same; ++k) s->into[places[k].vertex] = places[i].vertex + 1;
        i = j;
    }
    artfFree(places);
    for (uint32_t c = 0; c < cornerCount; ++c)
        if (s->into[s->corners[c]]) s->corners[c] = s->into[s->corners[c]] - 1;
    for (uint32_t t = 0; t < s->triangleCount; ++t) {
        const uint32_t* v = &s->corners[t * 3];
        s->triangleDead[t] = v[0] == v[1] || v[1] == v[2] || v[0] == v[2];
    }
}

static int compareEdges(const void* a, const void* b) {
    const uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

static void lockBorders(Simplifier* s) {
    const uint32_t count = s->triangleCount * 3;
    uint64_t* edges = artfMalloc((size_t)count * 8);
    for (uint32_t c = 0; c < count; ++c) {
        const uint32_t a = s->places[s->corners[c]], b = s->places[s->corners[c / 3 * 3 + (c + 1) % 3]];
        edges[c] = a < b ? (uint64_t)a << 32 | b : (uint64_t)b << 32 | a;
    }
    qsort(edges, count, 8, compareEdges);
    for (uint32_t i = 0; i < count;) {
        uint32_t j = i + 1;
        while (j < count && edges[j] == edges[i]) ++j;
        if (j - i == 1 && edges[i] >> 32 != (uint32_t)edges[i]) { s->locked[edges[i] >> 32] = 1; s->locked[(uint32_t)edges[i]] = 1; }
        i = j;
    }
    artfFree(edges);
}

uint32_t artfSimplify(const ArtfVertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount, const uint32_t* targets, uint32_t targetCount, ArtfSimplified* out) {
    Simplifier sim, *s = &sim;
    memset(s, 0, sizeof *s);
    s->vertexCount = vertexCount;
    s->triangleCount = indexCount / 3;
    s->positions = artfMalloc((size_t)vertexCount * 24);
    s->quadrics = artfCalloc(vertexCount, sizeof(Quadric));
    s->corners = artfMalloc((size_t)indexCount * 4);
    s->heads = artfCalloc(vertexCount, 4);
    s->tails = artfCalloc(vertexCount, 4);
    s->nexts = artfCalloc(indexCount, 4);
    s->triangleDead = artfCalloc(s->triangleCount, 1);
    s->vertexDead = artfCalloc(vertexCount, 1);
    s->locked = artfCalloc(vertexCount, 1);
    s->twins = artfCalloc(vertexCount, 4);
    s->places = artfCalloc(vertexCount, 4);
    s->into = artfCalloc(vertexCount, 4);
    s->versions = artfCalloc(vertexCount, 4);
    s->marks = artfCalloc(vertexCount, 4);
    s->errors = artfCalloc(vertexCount, 4);
    memcpy(s->corners, indices, (size_t)indexCount * 4);

    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (uint32_t i = 0; i < vertexCount; ++i) {
        const double p[3] = {vertices[i].px, vertices[i].py, vertices[i].pz};
        for (int k = 0; k < 3; ++k) { lo[k] = p[k] < lo[k] ? p[k] : lo[k]; hi[k] = p[k] > hi[k] ? p[k] : hi[k]; }
    }
    double extent = 0;
    for (int k = 0; k < 3; ++k) extent = hi[k] - lo[k] > extent ? hi[k] - lo[k] : extent;
    if (extent <= 0) extent = 1;
    for (uint32_t i = 0; i < vertexCount; ++i) {
        s->positions[(size_t)i * 3] = (vertices[i].px - lo[0]) / extent;
        s->positions[(size_t)i * 3 + 1] = (vertices[i].py - lo[1]) / extent;
        s->positions[(size_t)i * 3 + 2] = (vertices[i].pz - lo[2]) / extent;
    }

    findTwins(s, vertices, indexCount);
    /* each triangle's plane goes to its vertices, weighed by its area; corners to their lists */
    for (uint32_t t = 0; t < s->triangleCount; ++t) {
        if (s->triangleDead[t]) continue;
        const double *a = &s->positions[(size_t)s->corners[t * 3] * 3], *b = &s->positions[(size_t)s->corners[t * 3 + 1] * 3], *c = &s->positions[(size_t)s->corners[t * 3 + 2] * 3];
        const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const double len = sqrt(nx * nx + ny * ny + nz * nz);
        for (int k = 0; k < 3; ++k) {
            const uint32_t corner = t * 3 + (uint32_t)k, v = s->corners[corner];
            if (s->tails[v]) s->nexts[s->tails[v] - 1] = corner + 1;
            else s->heads[v] = corner + 1;
            s->tails[v] = corner + 1;
        }
        if (len <= 0) continue;
        nx /= len; ny /= len; nz /= len;
        const double d = -(nx * a[0] + ny * a[1] + nz * a[2]), w = len * .5;
        const Quadric q = {nx * nx * w, nx * ny * w, nx * nz * w, nx * d * w, ny * ny * w, ny * nz * w, ny * d * w, nz * nz * w, nz * d * w, d * d * w, w};
        for (int k = 0; k < 3; ++k) quadricAdd(&s->quadrics[s->corners[t * 3 + k]], &q);
    }
    lockBorders(s);
    for (uint32_t c = 0; c < indexCount; ++c) {
        const uint32_t a = s->corners[c], b = s->corners[c / 3 * 3 + (c + 1) % 3];
        if (a < b || s->locked[a] || s->locked[b]) pushEdge(s, a, b);
    }

    uint32_t alive = 0, levels = 0;
    for (uint32_t t = 0; t < s->triangleCount; ++t) alive += !s->triangleDead[t];
    double worst = 0;
    while (levels < targetCount) {
        bool reached = alive <= targets[levels];
        if (!reached && !s->heap.len) {
            /* nothing more can go: this level is as far as the mesh goes, if it is worth having */
            const uint32_t before = levels ? out[levels - 1].indexCount / 3 : s->triangleCount;
            if ((uint64_t)alive * 10 > (uint64_t)before * 7) break;
            targetCount = levels + 1;
            reached = true;
        }
        if (reached) {
            ArtfSimplified* o = &out[levels++];
            o->indices = artfMalloc((size_t)alive * 12);
            o->indexCount = 0;
            for (uint32_t t = 0; t < s->triangleCount; ++t)
                if (!s->triangleDead[t])
                    for (int k = 0; k < 3; ++k) o->indices[o->indexCount++] = s->corners[t * 3 + k];
            o->error = (float)(worst * extent);
            continue;
        }
        const Candidate c = heapPop(s);
        if (s->vertexDead[c.from] || s->vertexDead[c.to] || s->versions[c.from] != c.fromVersion || s->versions[c.to] != c.toVersion) continue;
        if (s->locked[c.from]) {
            /* along a cut: both sides, or neither */
            if (!seamEdge(s, c.from, c.to)) continue;
            const uint32_t otherFrom = s->twins[c.from] - 1, otherTo = s->twins[c.to] - 1;
            if (otherFrom == c.to || otherTo == c.from) continue;
            if (!collapseAllowed(s, c.from, c.to, 1) || !collapseAllowed(s, otherFrom, otherTo, 1)) continue;
            const float moved = s->errors[c.from] + (float)seamError(s, c.from, c.to);
            alive -= collapse(s, c.from, c.to);
            alive -= collapse(s, otherFrom, otherTo);
            s->errors[c.to] = artfMaxf(s->errors[c.to], artfMaxf(moved, s->errors[otherTo]));
            s->errors[otherTo] = s->errors[c.to];
            if ((double)s->errors[c.to] > worst) worst = s->errors[c.to];
            continue;
        }
        if (!collapseAllowed(s, c.from, c.to, 2)) continue;
        alive -= collapse(s, c.from, c.to);
        if ((double)s->errors[c.to] > worst) worst = s->errors[c.to];
    }

    artfFree(s->positions);
    artfFree(s->quadrics);
    artfFree(s->corners);
    artfFree(s->heads);
    artfFree(s->tails);
    artfFree(s->nexts);
    artfFree(s->triangleDead);
    artfFree(s->vertexDead);
    artfFree(s->locked);
    artfFree(s->twins);
    artfFree(s->places);
    artfFree(s->into);
    artfFree(s->versions);
    artfFree(s->marks);
    artfFree(s->errors);
    artfVecFree(&s->heap);
    return levels;
}
