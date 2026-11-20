/* artful — a mesh as a graph of clusters at every level of detail (phase 5.5), as Nanite has it, without its METIS:
 *
 *   the mesh is cut into clusters of at most ARTF_MESHLET_VERTICES vertices and ARTF_MESHLET_TRIANGLES triangles,
 *   grown across shared edges; clusters that share the most edges go in groups of up to eight (one left alone joins
 *   its closest neighbor's); each group is simplified to half its triangles with its border held (simplify.c holds an
 *   edge that only one of the triangles given has: given a group's triangles alone, that is the group's border), and
 *   what is left is cut into clusters again. Again, a level at a time, until no group can lose a sixth of its
 *   triangles.
 *
 * A cluster has two bounds, each a ball and an error in the mesh's units: its own (where the group it was made from
 * is, and how far that group's simplification and all before it moved the surface; 0 on the first level) and its
 * parent's (that of the group it went into, and what that group's simplification added; none past the last). A group's
 * ball holds its clusters' own balls and its error is at least theirs, so going up a cluster's line the bounds only
 * grow: from any eye, exactly one cluster of each line has its own bounds seen as fine and its parent's not
 * (artfDagFine), and those clusters together are the whole surface, with no cracks, since the borders between them
 * were held as they were. */
#include "scene/sceneInternal.h"
#include "scene/simplifyInternal.h"

#include <stdlib.h>

/* ---- clusters, grown across edges ---- */

typedef struct Edges {
    uint64_t* keys; /* of each corner's edge: its two places, the lower first */
    uint32_t* order;
} Edges;

/* The first vertex at each place: what is at one place is one, for edges (seams and hard edges split vertices). */
static uint32_t* placesOf(const ArtfVertex* v, uint32_t count) {
    uint32_t* order = artfMalloc((size_t)count * 4 + 4);
    uint32_t* places = artfMalloc((size_t)count * 4 + 4);
    for (uint32_t i = 0; i < count; ++i) order[i] = i;
    /* (an insertion of each into a hash of its bits: positions are compared exactly) */
    uint32_t size = 1;
    while (size < count * 2) size <<= 1;
    uint32_t* table = artfMalloc((size_t)size * 4);
    memset(table, 0xFF, (size_t)size * 4);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t bits[3];
        memcpy(bits, &v[i].px, 12);
        uint32_t h = (bits[0] * 73856093u) ^ (bits[1] * 19349663u) ^ (bits[2] * 83492791u);
        for (h &= size - 1;; h = (h + 1) & (size - 1)) {
            const uint32_t at = table[h];
            if (at == UINT32_MAX) { table[h] = i; places[i] = i; break; }
            if (v[at].px == v[i].px && v[at].py == v[i].py && v[at].pz == v[i].pz) { places[i] = at; break; }
        }
    }
    artfFree(table);
    artfFree(order);
    return places;
}

static uint64_t edgeKey(uint32_t a, uint32_t b) { return a < b ? (uint64_t)a << 32 | b : (uint64_t)b << 32 | a; }

typedef struct Built {
    ARTF_VEC(uint32_t) indices;
    ARTF_VEC(ArtfDagCluster) clusters;
} Built;

typedef struct Corner { uint64_t key; uint32_t triangle; } Corner;
static int compareCorners(const void* a, const void* b) {
    const Corner *x = a, *y = b;
    if (x->key != y->key) return x->key < y->key ? -1 : 1;
    return x->triangle < y->triangle ? -1 : x->triangle > y->triangle;
}

/* Cuts triangles (indices into the mesh's vertices) into clusters, each grown from the first triangle left across
 * edges to those that add the fewest vertices, and appends them; returns how many. */
static uint32_t clusterize(Built* out, const uint32_t* places, const uint32_t* indices, uint32_t triangleCount, uint32_t vertexCount, uint32_t level) {
    /* edges to triangles */
    Corner* corners = artfMalloc((size_t)triangleCount * 3 * sizeof *corners + 1);
    for (uint32_t t = 0; t < triangleCount; ++t)
        for (int k = 0; k < 3; ++k) corners[t * 3 + k] = (Corner){edgeKey(places[indices[t * 3 + k]], places[indices[t * 3 + (k + 1) % 3]]), t};
    qsort(corners, (size_t)triangleCount * 3, sizeof *corners, compareCorners);
    uint8_t* taken = artfCalloc(triangleCount + 1, 1);
    uint32_t* seen = artfCalloc(vertexCount + 1, 4);  /* stamp of the cluster that holds the vertex */
    uint32_t* queued = artfCalloc(triangleCount + 1, 4);
    ARTF_VEC(uint32_t) frontier = {0};
    uint32_t made = 0, next = 0, stamp = 0;
    while (true) {
        while (next < triangleCount && taken[next]) ++next;
        if (next >= triangleCount) break;
        ++stamp;
        ArtfDagCluster c;
        memset(&c, 0, sizeof c);
        c.indexOffset = (uint32_t)out->indices.len;
        c.level = level;
        uint32_t vertices = 0;
        frontier.len = 0;
        artfVecPush(&frontier, next);
        queued[next] = stamp;
        while (frontier.len && c.indexCount / 3 < ARTF_MESHLET_TRIANGLES) {
            /* of the triangles at the edge of the cluster, the one that adds the fewest vertices (the first so) */
            size_t best = SIZE_MAX;
            int bestFresh = 4;
            for (size_t i = 0; i < frontier.len; ++i) {
                const uint32_t t = frontier.items[i];
                int fresh = 0;
                for (int k = 0; k < 3; ++k) fresh += seen[indices[t * 3 + k]] != stamp;
                if (fresh < bestFresh) { bestFresh = fresh; best = i; }
            }
            const uint32_t t = frontier.items[best];
            frontier.items[best] = frontier.items[--frontier.len];
            if (vertices + (uint32_t)bestFresh > ARTF_MESHLET_VERTICES) break;
            taken[t] = 1;
            for (int k = 0; k < 3; ++k) {
                const uint32_t v = indices[t * 3 + k];
                if (seen[v] != stamp) { seen[v] = stamp; ++vertices; }
                artfVecPush(&out->indices, v);
            }
            c.indexCount += 3;
            /* its neighbors across its edges */
            for (int k = 0; k < 3; ++k) {
                const Corner key = {edgeKey(places[indices[t * 3 + k]], places[indices[t * 3 + (k + 1) % 3]]), 0};
                size_t lo = 0, hi = (size_t)triangleCount * 3;
                while (lo < hi) {
                    const size_t mid = (lo + hi) / 2;
                    if (corners[mid].key < key.key) lo = mid + 1;
                    else hi = mid;
                }
                for (size_t i = lo; i < (size_t)triangleCount * 3 && corners[i].key == key.key; ++i) {
                    const uint32_t u = corners[i].triangle;
                    if (!taken[u] && queued[u] != stamp) { queued[u] = stamp; artfVecPush(&frontier, u); }
                }
            }
        }
        artfVecPush(&out->clusters, c);
        ++made;
    }
    artfVecFree(&frontier);
    artfFree(queued);
    artfFree(seen);
    artfFree(taken);
    artfFree(corners);
    return made;
}

/* The ball around a cluster's vertices: the middle of their box, as finishMeshlet has it. */
static void ballOf(const ArtfVertex* v, const uint32_t* indices, uint32_t count, float* ball) {
    float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (uint32_t i = 0; i < count; ++i) {
        const float* p = &v[indices[i]].px;
        for (int c = 0; c < 3; ++c) { lo[c] = artfMinf(lo[c], p[c]); hi[c] = artfMaxf(hi[c], p[c]); }
    }
    for (int c = 0; c < 3; ++c) ball[c] = (lo[c] + hi[c]) * .5f;
    float r2 = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const float* p = &v[indices[i]].px;
        const float dx = p[0] - ball[0], dy = p[1] - ball[1], dz = p[2] - ball[2];
        r2 = artfMaxf(r2, dx * dx + dy * dy + dz * dz);
    }
    ball[3] = sqrtf(r2) * 1.0001f;
}

/* A ball that holds those given (centers and radii, four floats each). */
static void ballAround(const float* const* balls, uint32_t count, float* out) {
    float lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (uint32_t i = 0; i < count; ++i)
        for (int c = 0; c < 3; ++c) { lo[c] = artfMinf(lo[c], balls[i][c] - balls[i][3]); hi[c] = artfMaxf(hi[c], balls[i][c] + balls[i][3]); }
    for (int c = 0; c < 3; ++c) out[c] = (lo[c] + hi[c]) * .5f;
    float r = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const float dx = balls[i][0] - out[0], dy = balls[i][1] - out[1], dz = balls[i][2] - out[2];
        r = artfMaxf(r, sqrtf(dx * dx + dy * dy + dz * dz) + balls[i][3]);
    }
    out[3] = r * 1.0001f;
}

/* ---- groups: clusters that share the most edges, DAG_GROUP at a time ---- */

#define DAG_GROUP 8

typedef struct Link { uint32_t a, b, shared; } Link;
static int compareLinks(const void* x, const void* y) {
    const Link *p = x, *q = y;
    if (p->a != q->a) return p->a < q->a ? -1 : 1;
    return p->b < q->b ? -1 : p->b > q->b;
}
typedef struct Owned { uint64_t key; uint32_t cluster; } Owned;
static int compareOwned(const void* x, const void* y) {
    const Owned *p = x, *q = y;
    if (p->key != q->key) return p->key < q->key ? -1 : 1;
    return p->cluster < q->cluster ? -1 : p->cluster > q->cluster;
}

/* Groups of the clusters from first to first + count: group[i] for each; returns how many groups. */
static uint32_t groupClusters(const Built* b, const uint32_t* places, uint32_t first, uint32_t count, uint32_t* group) {
    /* which clusters each edge has */
    size_t corners = 0;
    for (uint32_t i = 0; i < count; ++i) corners += b->clusters.items[first + i].indexCount;
    Owned* owned = artfMalloc(corners * sizeof *owned + 1);
    size_t n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const ArtfDagCluster* c = &b->clusters.items[first + i];
        const uint32_t* ix = b->indices.items + c->indexOffset;
        for (uint32_t k = 0; k < c->indexCount; ++k) owned[n++] = (Owned){edgeKey(places[ix[k]], places[ix[k / 3 * 3 + (k + 1) % 3]]), i};
    }
    qsort(owned, n, sizeof *owned, compareOwned);
    /* links: two clusters that share an edge, once for each edge */
    ARTF_VEC(Link) links = {0};
    for (size_t i = 0; i < n;) {
        size_t j = i + 1;
        while (j < n && owned[j].key == owned[i].key) ++j;
        for (size_t x = i; x < j; ++x)
            for (size_t y = x + 1; y < j; ++y)
                if (owned[x].cluster != owned[y].cluster) {
                    artfVecPush(&links, ((Link){owned[x].cluster, owned[y].cluster, 1}));
                    artfVecPush(&links, ((Link){owned[y].cluster, owned[x].cluster, 1}));
                }
        i = j;
    }
    artfFree(owned);
    qsort(links.items, links.len, sizeof *links.items, compareLinks);
    /* (each pair once, with how many edges it shares) */
    size_t kept = 0;
    for (size_t i = 0; i < links.len; ++i) {
        if (kept && links.items[kept - 1].a == links.items[i].a && links.items[kept - 1].b == links.items[i].b) ++links.items[kept - 1].shared;
        else links.items[kept++] = links.items[i];
    }
    links.len = kept;
    uint32_t* firstLink = artfMalloc(((size_t)count + 1) * 4);
    for (uint32_t i = 0, l = 0; i <= count; ++i) {
        while (l < links.len && links.items[l].a < i) ++l;
        firstLink[i] = l;
    }
    for (uint32_t i = 0; i < count; ++i) group[i] = UINT32_MAX;
    uint32_t groups = 0;
    uint32_t members[DAG_GROUP];
    uint32_t* sizes = artfCalloc((size_t)count + 1, 4);
    for (uint32_t i = 0; i < count; ++i) {
        if (group[i] != UINT32_MAX) continue;
        uint32_t size = 0;
        members[size++] = i;
        group[i] = groups;
        while (size < DAG_GROUP) {
            /* the cluster left that shares the most edges with the group (the first so) */
            uint32_t best = UINT32_MAX, bestShared = 0;
            for (uint32_t m = 0; m < size; ++m)
                for (uint32_t l = firstLink[members[m]]; l < firstLink[members[m] + 1]; ++l) {
                    const uint32_t other = links.items[l].b;
                    if (group[other] != UINT32_MAX) continue;
                    uint32_t shared = 0;
                    for (uint32_t q = 0; q < size; ++q)
                        for (uint32_t r = firstLink[members[q]]; r < firstLink[members[q] + 1]; ++r)
                            if (links.items[r].b == other) shared += links.items[r].shared;
                    if (shared > bestShared || (shared == bestShared && other < best)) { bestShared = shared; best = other; }
                }
            if (best == UINT32_MAX) break;
            members[size++] = best;
            group[best] = groups;
        }
        sizes[groups++] = size;
    }
    /* one left alone, which could not be simplified, goes into the group of the neighbor it shares the most edges with */
    for (uint32_t i = 0; i < count; ++i) {
        if (sizes[group[i]] != 1) continue;
        uint32_t best = UINT32_MAX, bestShared = 0;
        for (uint32_t l = firstLink[i]; l < firstLink[i + 1]; ++l)
            if (links.items[l].shared > bestShared) { bestShared = links.items[l].shared; best = links.items[l].b; }
        if (best == UINT32_MAX) continue;
        sizes[group[i]] = 0;
        group[i] = group[best];
        ++sizes[group[best]];
    }
    artfFree(sizes);
    artfFree(firstLink);
    artfVecFree(&links);
    return groups;
}

/* ---- the graph ---- */

bool artfDagBuild(const ArtfVertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount, ArtfDag* out) {
    memset(out, 0, sizeof *out);
    if (!vertexCount || indexCount < 3) return false;
    uint32_t* places = placesOf(vertices, vertexCount);
    Built b = {0};
    uint32_t first = 0, count = clusterize(&b, places, indices, indexCount / 3, vertexCount, 0);
    for (uint32_t i = 0; i < count; ++i) {
        ArtfDagCluster* c = &b.clusters.items[i];
        ballOf(vertices, b.indices.items + c->indexOffset, c->indexCount, c->self);
        c->self[4] = 0;
        c->parent[4] = INFINITY;
    }
    uint32_t level = 0;
    uint32_t* local = artfMalloc((size_t)vertexCount * 4);
    memset(local, 0xFF, (size_t)vertexCount * 4);
    ARTF_VEC(uint32_t) gathered = {0};
    ARTF_VEC(uint32_t) back = {0};
    ARTF_VEC(ArtfVertex) groupVertices = {0};
    while (count > 1) {
        uint32_t* group = artfMalloc((size_t)count * 4);
        const uint32_t groups = groupClusters(&b, places, first, count, group);
        const uint32_t nextFirst = (uint32_t)b.clusters.len;
        const float** balls = artfMalloc((size_t)count * sizeof *balls);
        ++level;
        for (uint32_t g = 0; g < groups; ++g) {
            /* its triangles, with the group's vertices alone */
            gathered.len = back.len = groupVertices.len = 0;
            uint32_t members = 0;
            float childError = 0;
            for (uint32_t i = 0; i < count; ++i) {
                if (group[i] != g) continue;
                const ArtfDagCluster* c = &b.clusters.items[first + i];
                balls[members++] = c->self;
                childError = artfMaxf(childError, c->self[4]);
                for (uint32_t k = 0; k < c->indexCount; ++k) {
                    const uint32_t v = b.indices.items[c->indexOffset + k];
                    if (local[v] == UINT32_MAX) {
                        local[v] = (uint32_t)back.len;
                        artfVecPush(&back, v);
                        artfVecPush(&groupVertices, vertices[v]);
                    }
                    artfVecPush(&gathered, local[v]);
                }
            }
            const uint32_t triangles = (uint32_t)gathered.len / 3, target = triangles / 2;
            ArtfSimplified simplified = {0};
            const uint32_t made = members > 1 ? artfSimplify(groupVertices.items, (uint32_t)groupVertices.len, gathered.items, (uint32_t)gathered.len, &target, 1, &simplified) : 0;
            for (size_t i = 0; i < back.len; ++i) local[back.items[i]] = UINT32_MAX;
            if (!made || simplified.indexCount / 3 * 6 > triangles * 5) {
                /* (it cannot go far enough: these clusters are as coarse as they get) */
                if (made) artfFree(simplified.indices);
                continue;
            }
            float bounds[5];
            ballAround(balls, members, bounds);
            bounds[4] = childError + simplified.error;
            for (uint32_t i = 0; i < count; ++i)
                if (group[i] == g) memcpy(b.clusters.items[first + i].parent, bounds, sizeof bounds);
            for (uint32_t k = 0; k < simplified.indexCount; ++k) simplified.indices[k] = back.items[simplified.indices[k]];
            const uint32_t from = (uint32_t)b.clusters.len;
            clusterize(&b, places, simplified.indices, simplified.indexCount / 3, vertexCount, level);
            for (uint32_t i = from; i < (uint32_t)b.clusters.len; ++i) {
                memcpy(b.clusters.items[i].self, bounds, sizeof bounds);
                b.clusters.items[i].parent[4] = INFINITY;
            }
            artfFree(simplified.indices);
        }
        artfFree(balls);
        artfFree(group);
        first = nextFirst;
        count = (uint32_t)b.clusters.len - nextFirst;
    }
    artfVecFree(&gathered);
    artfVecFree(&back);
    artfVecFree(&groupVertices);
    artfFree(local);
    artfFree(places);
    out->indices = b.indices.items;
    out->indexCount = (uint32_t)b.indices.len;
    out->clusters = b.clusters.items;
    out->clusterCount = (uint32_t)b.clusters.len;
    out->levels = level + 1;
    return true;
}

void artfDagFree(ArtfDag* d) {
    artfFree(d->indices);
    artfFree(d->clusters);
    memset(d, 0, sizeof *d);
}

uint32_t artfDagCut(const ArtfDag* d, const float* eye, float lodScale, uint32_t* chosen) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->clusterCount; ++i) {
        const ArtfDagCluster* c = &d->clusters[i];
        if (artfDagFine(c->self, eye, lodScale) && !artfDagFine(c->parent, eye, lodScale)) {
            if (chosen) chosen[n] = i;
            ++n;
        }
    }
    return n;
}
