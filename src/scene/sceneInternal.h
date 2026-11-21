/* artful — a scene's memory, as the backends read it. What a GPU reads is laid out here for it, so
 * that a backend copies it as it is. */
#ifndef ARTFUL_SCENE_INTERNAL_H
#define ARTFUL_SCENE_INTERNAL_H

#include "artful/scene.h"
#include "core/trig.h"
#include "core/color.h"
#include <string.h>

/* artfMat4Perspective, with the tangent worked out as the same bytes everywhere (core/trig.h) */
static inline ArtfMat4 artfPerspective(float fovY, float aspect, float near_) {
    const float f = 1.f / artfTan(fovY * .5f);
    ArtfMat4 r = {{f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, 0, -1, 0, 0, near_, 0}};
    return r;
}

#define ARTF_MESHLET_VERTICES 64
#define ARTF_MESHLET_TRIANGLES 124
#define ARTF_MAX_LODS 3
#define ARTF_SHADOW_BIG_VIEWS 16  /* the sun's and the frame's lights': a map's side each */
#define ARTF_SHADOW_VIEWS 64      /* and, below them, smaller ones for lights past the frame's own */
#define ARTF_SHADOW_EXTRA 32      /* lights past the frame's own that can have shadows at once */
#define ARTF_SHADOW_CASCADES 3
#define ARTF_SHADOW_ATLAS_COLUMNS 4
#define ARTF_HIZ_LEVELS 15
/* A background pass's bloom (artfSceneSetBackgroundPass). */
#define ARTF_BLOOM_LEVELS 8
#define ARTF_BLOOM_CEILING 48.f /* what one pixel can give the bloom */
#define ARTF_BLOOM_FADE .85f    /* what each level is worth, of the one below: glow falls off with distance */
#define ARTF_BLOOM_THRESHOLD 2.f /* what a pixel gives the glow is its light past this: a lens's veil comes of what is
                                 * far too bright, not of all there is */
#define ARTF_BLOOM_WHITE .9f    /* how much of the glow's color goes to grey, as a lens's veil does */

/* 16 bytes a vertex, half of what floats take: the position as 16 bits of the mesh's box, the normal
 * folded onto an octahedron, the texture coordinates as halves. */
typedef struct ArtfPackedVertex {
    uint16_t px, py, pz, pad;
    int16_t nx, ny;
    uint16_t u, v;
} ArtfPackedVertex;

/* A small piece of a mesh, culled alone and transformed in the first level of the cache. */
typedef struct ArtfMeshlet {
    uint32_t vertexOffset;   /* into positions and meshletVertices (a multiple of 8) */
    uint32_t triangleOffset; /* into meshletTriangles, in triangles */
    uint32_t vertexCount, triangleCount;
    float center[3], radius;
    /* Faces away from a camera where dot(center - camera, cone) >= coneCutoff * distance + radius.
     * coneCutoff 2: no such camera. */
    float cone[3], coneCutoff;
} ArtfMeshlet;

/* A mesh as a graph of clusters at every level of detail (dag.c, phase 5.5). Each cluster's triangles are indices into
 * the mesh's vertices; self and parent: a ball (center, radius) and an error, in the mesh's units: its own, from the
 * group it was made from (error 0 on the first level), and that of the group it went into (error infinite: none). */
#define ARTF_DAG_LEAST_TRIANGLES 4096 /* artfSceneMeshSetClusters takes no smaller mesh */
typedef struct ArtfDagCluster {
    uint32_t indexOffset, indexCount, level;
    float self[5], parent[5];
} ArtfDagCluster;
typedef struct ArtfDag {
    uint32_t* indices;
    uint32_t indexCount;
    ArtfDagCluster* clusters; /* the first level first */
    uint32_t clusterCount, levels;
} ArtfDag;
bool artfDagBuild(const ArtfVertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount, ArtfDag* out);
void artfDagFree(ArtfDag* d);
/* Bounds are fine from an eye when their error, at the nearest of their ball, is within what lodScale allows (as
 * artfMeshLevel's: e * lodScale <= distance); none (an infinite error) never are. A cluster is drawn when its own are
 * and its parent's are not. */
ARTF_INLINE bool artfDagFine(const float* bounds, const float* eye, float lodScale) {
    if (!(bounds[4] < INFINITY)) return false;
    const float dx = bounds[0] - eye[0], dy = bounds[1] - eye[1], dz = bounds[2] - eye[2];
    return bounds[4] * lodScale <= artfMaxf(sqrtf((dx * dx + dy * dy) + dz * dz) - bounds[3], 0.f);
}
/* The clusters drawn from eye: how many, and which, in order, in chosen (or NULL). */
uint32_t artfDagCut(const ArtfDag* d, const float* eye, float lodScale, uint32_t* chosen);
/* What an instance draws of a cluster of its mesh's graph, in the world: centers by its matrix m (three rows of four),
 * balls and errors by its largest scale; with no level of detail (lodScale 0, or an orthographic camera), the first
 * level. The GPU's meshlet culling does the same (shaders.c). */
ARTF_INLINE bool artfDagFineAt(const float* b, const float* m, float maxScale, const float* eye, float lodScale) {
    if (!(b[4] < 1e30f)) return false;
    const float cx = m[0] * b[0] + m[1] * b[1] + m[2] * b[2] + m[3], cy = m[4] * b[0] + m[5] * b[1] + m[6] * b[2] + m[7], cz = m[8] * b[0] + m[9] * b[1] + m[10] * b[2] + m[11];
    const float dx = cx - eye[0], dy = cy - eye[1], dz = cz - eye[2];
    return b[4] * maxScale * lodScale <= artfMaxf(sqrtf((dx * dx + dy * dy) + dz * dz) - b[3] * maxScale, 0.f);
}
ARTF_INLINE bool artfDagDrawn(const float* bounds, bool first, const float* m, float maxScale, const float* eye, float lodScale) {
    if (!(lodScale > 0)) return first;
    return artfDagFineAt(bounds, m, maxScale, eye, lodScale) && !artfDagFineAt(bounds + 5, m, maxScale, eye, lodScale);
}
/* Of a mesh's meshlets, those of the full mesh: all but a graph's levels past the first. */
#define ARTF_MESH_WHOLE_MESHLETS(m) ((m)->dagBounds ? (m)->dagWhole : (m)->meshletCount)
/* All its indices: a graph's levels too. */
#define ARTF_MESH_ALL_INDICES(m) ((m)->dagBounds ? (m)->dagIndexCount : (m)->indexCount)

/* Lights sampled (ArtfShadowDesc.sampled, phase 5.7), the same on both backends (shaders.c's sampledSpv): of a pixel,
 * from its place on the screen and the frame's number, eight lights drawn of the n that may reach it (the frame's own,
 * then its cluster's list), one kept by weighted reservoir sampling on what each would give unshadowed (artfSampledWeigh:
 * luminance x fade^2 x spot x n.l), and the weight that makes it all of them on average: n x the sum / (8 x its own). */
#define ARTF_SAMPLED_DRAWS 8
ARTF_INLINE uint32_t artfSampledHash(uint32_t x) {
    x = (x ^ x >> 16) * 0x7FEB352Du;
    x = (x ^ x >> 15) * 0x846CA68Bu;
    return x ^ x >> 16;
}
ARTF_INLINE uint32_t artfSampledSeed(uint32_t x, uint32_t y, uint32_t frame) { return artfSampledHash(x ^ artfSampledHash(y ^ artfSampledHash(frame))); }
ARTF_INLINE float artfSampledWeigh(const float* position, float radius, const float* color, const float* direction, float spotCut, float spotScale, const float* p, const float* n) {
    const float d[3] = {position[0] - p[0], position[1] - p[1], position[2] - p[2]};
    const float d2 = (d[0] * d[0] + d[1] * d[1]) + d[2] * d[2];
    const float fade = artfMaxf(1.f - d2 / (radius * radius), 0.f);
    const float inv = 1.f / sqrtf(artfMaxf(d2, 1e-8f));
    const float l[3] = {d[0] * inv, d[1] * inv, d[2] * inv};
    const float spot = artfClampf((-((l[0] * direction[0] + l[1] * direction[1]) + l[2] * direction[2]) - spotCut) * spotScale, 0.f, 1.f);
    const float ndl = artfMaxf((n[0] * l[0] + n[1] * l[1]) + n[2] * l[2], 0.f);
    const float luminance = (.2126f * color[0] + .7152f * color[1]) + .0722f * color[2];
    return ((luminance * (fade * fade)) * spot) * ndl;
}
/* What a mesh that bends was, by the order of its packed vertices. */
typedef struct ArtfSkinData {
    float* rest;       /* position and normal: six floats a vertex */
    uint16_t* joints;  /* eight a vertex; NULL: it has only targets */
    float* weights;
    float* morphs;     /* of each target, six floats a vertex: what it adds */
    float* morphReach; /* the farthest each target moves a vertex */
    uint32_t morphCount;
    float restLow[3], restHigh[3];
} ArtfSkinData;

/* How an instance's copy of such a mesh stands: what was asked, which may not be in its vertices yet. */
typedef struct ArtfPose {
    float* joints;     /* twelve floats each */
    uint32_t jointCount;
    float* weights;    /* of the targets */
    uint64_t applied;  /* the deformVersion its vertices are at */
} ArtfPose;

typedef struct ArtfMeshData {
    ArtfPackedVertex* vertices;
    uint32_t vertexCount;
    uint32_t* indices;
    uint32_t indexCount;
    /* a position is its 16 bits / 65535 * scale + offset */
    float scale[3], offset[3];
    float center[3], radius;
    ArtfMeshlet* meshlets;
    uint32_t meshletCount;
    uint32_t* meshletVertices;  /* index into vertices */
    uint8_t* meshletTriangles;  /* three local indices each */
    float* positions;           /* by meshlet, in blocks: all x, all y, all z, padded to 8 */
    uint32_t positionStride;    /* floats between the x, the y and the z of a meshlet's block */
    uint32_t* colors;           /* BGRA, of each vertex; NULL: all white */
    bool hasAlpha;              /* a color is below full alpha */
    ArtfSkinData* skin;         /* NULL: it does not bend */
    bool ownsSkin;              /* copies of a mesh for instances share it */
    ArtfPose pose;
    bool removed;               /* its place waits for another mesh, with nothing in it */
    uint32_t generation;        /* a new one in this place, or what a GPU holds of it went or came (residency) */
    uint32_t place;             /* a new mesh in this place: the top byte of its handle (artfMeshHandleOf) */
    uint64_t deformVersion;     /* a pose changed its vertices */
    /* Its lighter versions, the lightest last: meshes of the scene like any other. */
    uint32_t lods[ARTF_MAX_LODS];
    float lodErrors[ARTF_MAX_LODS];
    uint32_t lodCount;
    bool isLod;
    /* As a graph of clusters (artfSceneMeshSetClusters): one meshlet a cluster, those of the first level (the full mesh)
     * first, dagWhole of them; indexCount counts only theirs, dagIndexCount all. dagBounds: of each meshlet, its own
     * bounds and its parent's (ArtfDagCluster's self and parent). NULL: not so. */
    float* dagBounds;
    uint32_t dagWhole, dagIndexCount;
    /* What bends has levels apart (scale.c), each with a skin of its own: on the mesh given, those meshes; on an
     * instance's copy of it, the copies of them that instance takes in turn. home: on each such copy, the copy of the
     * whole + 1, which holds the list. The backends never see these: an instance's mesh is the level it takes. */
    uint32_t skinLods[ARTF_MAX_LODS];
    float skinLodErrors[ARTF_MAX_LODS];
    uint32_t skinLodCount, home;
    /* On an instance's copy of the whole, while the instance draws a level: the level it took its pose from (+ 1), at
     * that level's deformVersion. Rays meet the whole (artfSceneSkinWholesPosed). */
    uint32_t posedFrom;
    uint64_t posedAt;
    /* Residency (scale.c): of this mesh's levels (0 the whole, k its lods[k - 1]), the first that is held: those before
     * it are drawn as that one. evicted: this mesh's own triangles are not on the GPU, nor, if it can be read again,
     * in memory. */
    uint32_t residentFrom;
    bool evicted;
    bool streamed;  /* it can be read again (artfSceneAddMeshStreamed) */
} ArtfMeshData;

/* As a graph's (meshBuild.c): its indices, meshlets and bounds from the graph's. */
void artfMeshSetDag(ArtfMeshData* m, const ArtfDag* d);

/* The error of a mesh's level i, as both backends choose by it: 0, always taken, where the levels before it are not
 * held. */
ARTF_INLINE float artfMeshLevelError(const ArtfMeshData* m, uint32_t i) { return i < m->residentFrom ? 0.f : m->lodErrors[i]; }

typedef struct ArtfTextureData {
    int width, height, mipCount;
    uint32_t* pixels;      /* the mipmaps, the largest first; NULL until blocks are turned into them */
    uint32_t offsets[16];  /* of each mipmap, in pixels */
    uint32_t generation;   /* a new one in this place */
    uint32_t place;        /* how many have been in this place: the top byte of its handle (artfTextureHandleOf) */
    int residentMip;       /* what is drawn from */
    bool removed;
    uint32_t format;       /* what Vulkan calls its blocks; 0: plain pixels */
    uint8_t* blocks;
    size_t blockBytes;
    uint32_t blockOffsets[16];
    ArtfTextureReadFn read; /* not NULL: pixels holds only what is resident, and offsets are from there */
    void* readUser;
    /* Its mipmaps are means of light (what a color is), not of bytes (what data are): artfSceneTextureRoles says,
     * by what uses it. Only the mipmaps Artful made itself: not a KTX2 file's, nor those a program reads. */
    bool light;
    bool roleSaid;          /* that it is used both ways was said */
} ArtfTextureData;

/* The GPU's instance: 64 bytes. */
typedef struct ArtfInstanceData {
    float m[12];
    uint32_t mesh;      /* index */
    uint32_t material;  /* index */
    float maxScale;     /* what the mesh's radius grows by */
    uint16_t opacity;   /* of 65535 */
    uint8_t flags;      /* ARTF_INSTANCE_ */
    uint8_t translucent; /* a layer: what is behind it shows */
} ArtfInstanceData;

enum {
    ARTF_INSTANCE_UNIFORM = 1, /* its scale is the same on the three axes: cones can be used */
    ARTF_INSTANCE_CUTOUT = 2,  /* its material has holes, or a back: it cannot go with the plain ones */
    ARTF_INSTANCE_PARTICLE = 4 /* an emitter's: not traced, casts nothing, and turns to face the eye */
};

/* The GPU's material: 144 bytes. */
#define ARTF_MATERIAL_ROWS 9
typedef struct ArtfMaterialData {
    float baseColor[3], specular;
    float emissive[3], roughness;
    uint32_t texture;
    float opacity, cutoff;
    uint32_t flags;     /* ARTF_MATERIAL_ */
    float ior, metallic, normalScale, occlusion;
    uint32_t normalMap, surfaceMap, occlusionMap, graph;
    float params[4][4];
} ArtfMaterialData;

enum { ARTF_MATERIAL_MASK = 1, ARTF_MATERIAL_BLEND = 2, ARTF_MATERIAL_DOUBLE = 4, ARTF_MATERIAL_UNLIT = 8 };

/* What a material has past glTF's core: a clear coat, sheen, and the rest of glTF's extensions that light reads. Apart
 * from ArtfMaterialData, whose layout Metal's shaders were made for: on the GPU these follow all of those, in the same
 * buffer (ArtfFrameData.materials says where). Textures are 0 when there are none: white. */
typedef struct ArtfMaterialMore {
    float coat, coatRoughness, sheenRoughness;
    uint32_t flags;                     /* ARTF_MORE_: what it has */
    float sheen[3], subsurfaceRadius;
    uint32_t coatMap, coatRoughnessMap, coatNormalMap, sheenMap;
    uint32_t sheenRoughnessMap;
    float coatNormalScale, anisotropy, iridescence;
    float specularColor[3], iridescenceIor;  /* specularColor: 1 when it has none */
    float anisotropyDirection[2], iridescenceThickness, pad0; /* the direction: its cosine and sine */
    float subsurface[3];
    uint32_t specularMap;               /* KHR_materials_specular's: how much (alpha) */
    float absorb[3];                    /* traced, through its inside: kept is 2^-(absorb x distance) */
    uint32_t specularColorMap;          /* and of what color (as a base color's) */
    /* KHR_materials_iridescence's and KHR_materials_anisotropy's textures, and the film's least thickness */
    uint32_t iridescenceMap, iridescenceThicknessMap, anisotropyMap;
    float iridescenceThicknessMin;
    float reactive, pad3[3];            /* ArtfMaterialDesc.reactive */
} ArtfMaterialMore;
#define ARTF_MATERIAL_MORE_ROWS 10
enum {
    ARTF_MORE_COAT = 1, ARTF_MORE_SHEEN = 2, ARTF_MORE_SPECULAR = 4, ARTF_MORE_ANISOTROPY = 8, ARTF_MORE_IRIDESCENCE = 16,
    ARTF_MORE_SUBSURFACE = 32, ARTF_MORE_MAPS = 64 /* the coat's or sheen's textures */, ARTF_MORE_VOLUME = 128,
    ARTF_MORE_DRAWN = 127 /* what drawing reads: all but the volume, which is traced */
};
/* It needs where u and v go: a coat's normal map, or highlights drawn out. */
#define ARTF_MORE_SPECULAR_MAPS(m) ((m)->specularMap || (m)->specularColorMap)
#define ARTF_MORE_FILM_MAPS(m) ((m)->iridescenceMap || (m)->iridescenceThicknessMap)
#define ARTF_MORE_TANGENT(m) ((m)->coatNormalMap || ((m)->flags & ARTF_MORE_ANISOTROPY))
ARTF_INLINE bool artfMaterialLayered(const ArtfMaterialMore* m) { return (m->flags & ARTF_MORE_DRAWN) != 0; }


/* A material of the program's own: steps on four floats, each reading those before it (graph.c). */
#define ARTF_GRAPH_NODES 256
#define ARTF_GRAPH_OUTPUTS 6

enum {
    ARTF_OP_CONST, ARTF_OP_UV, ARTF_OP_POSITION, ARTF_OP_NORMAL, ARTF_OP_TO_EYE, ARTF_OP_BASE, ARTF_OP_PARAM, ARTF_OP_TEXTURE,
    ARTF_OP_ADD, ARTF_OP_SUB, ARTF_OP_MUL, ARTF_OP_DIV, ARTF_OP_MIN, ARTF_OP_MAX, ARTF_OP_DOT, ARTF_OP_FLOOR, ARTF_OP_FRACT,
    ARTF_OP_ABS, ARTF_OP_SQRT, ARTF_OP_SIN, ARTF_OP_COS, ARTF_OP_MIX, ARTF_OP_IF, ARTF_OP_X, ARTF_OP_Y, ARTF_OP_Z, ARTF_OP_W, ARTF_OP_VEC
};

typedef struct ArtfGraphNode {
    uint8_t op;
    uint16_t a, b, c;   /* the steps it reads, from 1; a parameter's index; b: a texture */
    float v[4];
} ArtfGraphNode;

typedef struct ArtfGraphData {
    ArtfGraphNode* nodes;
    uint32_t count;
    uint32_t outputs[ARTF_GRAPH_OUTPUTS]; /* steps, from 1; 0: as the material says */
} ArtfGraphData;

typedef struct ArtfGraphInput {
    float u, v, world[3], normal[3], toEye[3], base[3];
} ArtfGraphInput;

/* In: what the material says. Out: what the graph made of it. */
typedef struct ArtfGraphResult {
    float color[3], emissive[3], normal[3], roughness, metallic, alpha;
} ArtfGraphResult;

/* Both, for eight pixels side by side: they go in as their last four rows of inputs, and out in the rest. */
typedef struct ArtfGraphLanes {
    float u[8], v[8], world[3][8], normalIn[3][8], toEye[3][8], base[3][8];
    float color[3][8], emissive[3][8], normal[3][8], roughness[8], metallic[8], alpha[8];
} ArtfGraphLanes;

/* A point light has no direction: its cone then lets all through. */
typedef struct ArtfLightData {
    float position[3], radius;
    float color[3], spotScale;  /* 1 / (the cosines of the cone's inside and outside apart) */
    float direction[3], spotCut; /* where it goes to; the cosine of the outside */
} ArtfLightData;

/* Light that bounced (trace.c): a grid of probes. First, of each, four vec4s: the light from all around and whether the
 * probe counts (its w: 0 when it is inside something, ARTF_PROBE_INSIDE), then of red, green and blue how it changes
 * with the normal (what a surface facing n takes, over pi, is c0 + (r . n, g . n, b . n)); after all of those, of each, how far it sees, in a map of ARTF_PROBE_SIDE x ARTF_PROBE_SIDE directions
 * folded onto an octahedron around +z, as artfProbeFold: the mean and the mean of the square of each, two floats a
 * texel, by rows. (Apart, so that what every pixel reads of eight probes stays together.) A point the probe cannot see
 * is told by them (Chebyshev, as Majercik et al.'s probes do), so that light does not go through walls. */
#define ARTF_PROBE_SIDE 8
#define ARTF_PROBE_MAP (ARTF_PROBE_SIDE * ARTF_PROBE_SIDE * 2)
#define ARTF_PROBE_FLOATS (16 + ARTF_PROBE_MAP) /* all of a probe's, in both places */
/* A probe is inside something when more than a quarter of its rays meet backs (Majercik et al., 2021): it counts for
 * nothing where light is taken from the probes, and it is followed again only every ARTF_PROBE_INSIDE_EVERY steps (each
 * on its own step, by its number), so its rays are not spent. */
#define ARTF_PROBE_INSIDE_EVERY 8u
/* The group a probe's rays are of, in sampling.h's sequences: each probe's own, by its number; step k's rays are k x rays
 * onwards (Sobol's points, scrambled: the plan's 2.2). */
#define ARTF_PROBE_SEQUENCE 200u
/* What a probe keeps of what it had, when what its rays bring is far from it: as much less as the change in light (of
 * the larger, and a hundredth) is past a quarter, twice over, at most four fifths less; so a lamp turned on, or a door
 * opened, is followed in fewer steps, and the noise of a still one is not. */
ARTF_INLINE float artfProbeKeep(float keep, float now, float had) {
    const float change = fabsf(now - had) / (artfMaxf(now, had) + .01f);
    return keep * (1.f - artfClampf((change - .25f) * 2.f, 0.f, .8f));
}

/* A direction of length 1 onto the octahedron around +z: where it falls in the probe's map, from 0 to 1 each way. */
ARTF_INLINE void artfProbeFold(const float* d, float* u, float* v) {
    const float sum = fabsf(d[0]) + fabsf(d[1]) + fabsf(d[2]) + 1e-20f; /* (none: the middle) */
    float a = d[0] / sum, b = d[1] / sum;
    if (d[2] < 0) {
        const float fa = (1.f - fabsf(b)) * (a >= 0 ? 1.f : -1.f);
        b = (1.f - fabsf(a)) * (b >= 0 ? 1.f : -1.f);
        a = fa;
    }
    *u = a * .5f + .5f;
    *v = b * .5f + .5f;
}
typedef struct ArtfIndirect {
    bool on;
    ArtfIndirectDesc desc;
    int dims[3];
    float origin[3], spacing[3];
    /* With follow: the cell the volume starts at, in cells of the world, and where that cell's probe is kept (each
     * axis, base modulo dims): a cell (x, y, z) of the volume is kept at artfIndirectSlot. Without: none, and all 0. */
    int base[3], wrap[3];
    int cascades;       /* volumes (follow): each after the one before in probes, its cells twice as far apart */
    float* probes;      /* dims product x cascades x ARTF_PROBE_FLOATS: all the volumes' first 16 floats, then their maps */
    float* next;        /* what this step makes */
    uint32_t frames;
    uint64_t version;   /* a step changed them */
    uint32_t serial;    /* artfSceneSetIndirect's calls: a GPU that gathers the probes itself starts over when it changes */
} ArtfIndirect;
/* All the probes, of all the volumes; and of one. */
ARTF_INLINE uint32_t artfIndirectPer(const ArtfIndirect* g) { return (uint32_t)(g->dims[0] * g->dims[1] * g->dims[2]); }
ARTF_INLINE uint32_t artfIndirectCount(const ArtfIndirect* g) { return artfIndirectPer(g) * (uint32_t)(g->cascades > 0 ? g->cascades : 1); }
/* (a / 2^k, rounded down, for any a) */
ARTF_INLINE int artfFloorShift(int a, int k) { return a >= 0 ? a >> k : -((-a + (1 << k) - 1) >> k); }
/* Volume k's corner, spacing and wrap: the first's as the grid has them; each after, with follow, from the first's
 * cell of the camera in whole cells (the same integers on the GPU), its cells 2^k as far apart. */
ARTF_INLINE void artfIndirectCascade(const ArtfIndirect* g, int k, float* origin, float* spacing, int* wrap) {
    for (int a = 0; a < 3; ++a) {
        if (!k) {
            origin[a] = g->origin[a];
            spacing[a] = g->spacing[a];
            wrap[a] = g->wrap[a];
            continue;
        }
        const int half = (g->dims[a] - 1) / 2, base = artfFloorShift(g->base[a] + half, k) - half;
        spacing[a] = g->spacing[a] * (float)(1 << k);
        origin[a] = (float)base * spacing[a];
        wrap[a] = ((base % g->dims[a]) + g->dims[a]) % g->dims[a];
    }
}
/* Where the probe of a volume's cell (x, y, z) is kept: each axis turned by its wrap (follow), as the GPU's. */
ARTF_INLINE uint32_t artfIndirectSlotOf(const ArtfIndirect* g, const int* wrap, int x, int y, int z) {
    const int c[3] = {x + wrap[0], y + wrap[1], z + wrap[2]};
    int s[3];
    for (int k = 0; k < 3; ++k) s[k] = c[k] >= g->dims[k] ? c[k] - g->dims[k] : c[k];
    return (uint32_t)((s[2] * g->dims[1] + s[1]) * g->dims[0] + s[0]);
}
ARTF_INLINE uint32_t artfIndirectSlot(const ArtfIndirect* g, int x, int y, int z) { return artfIndirectSlotOf(g, g->wrap, x, y, z); }
/* What the probe kept at index (of all the volumes') is of: its volume, the volume's cell there now (cell), the
 * volume's corner and spacing (where), and the world's cell as its stamp (1 + its cell's coordinates, each modulo 256,
 * in a float: exact). A probe whose stamp is not its cell's is new there: it starts over. The world's cell is the
 * volume's corner over its spacing, rounded, plus the volume's cell, on both backends. */
ARTF_INLINE float artfIndirectStampAt(const ArtfIndirect* g, uint32_t index, int* cell, float* origin, float* spacing) {
    const uint32_t per = artfIndirectPer(g), slot = index % per;
    int wrap[3];
    artfIndirectCascade(g, (int)(index / per), origin, spacing, wrap);
    const int sx = (int)(slot % (uint32_t)g->dims[0]), sy = (int)(slot / (uint32_t)g->dims[0] % (uint32_t)g->dims[1]), sz = (int)(slot / (uint32_t)(g->dims[0] * g->dims[1]));
    const int s[3] = {sx, sy, sz};
    uint32_t code = 0;
    for (int k = 0; k < 3; ++k) {
        cell[k] = s[k] >= wrap[k] ? s[k] - wrap[k] : s[k] - wrap[k] + g->dims[k];
        const int world = (int)floorf(origin[k] / spacing[k] + .5f) + cell[k];
        code |= (uint32_t)(world & 255) << (8 * k);
    }
    return (float)(code + 1u);
}
/* How far inside volume k a point is, as a share to take from it: 1 from a cell and a half in, 0 at half a cell. */
ARTF_INLINE float artfIndirectInside(const ArtfIndirect* g, const float* origin, const float* spacing, const float* q) {
    float least = 1e30f;
    for (int a = 0; a < 3; ++a) {
        const float at = (q[a] - origin[a]) / spacing[a];
        least = fminf(least, fminf(at, (float)(g->dims[a] - 1) - at));
    }
    return fminf(fmaxf(least - .5f, 0.f), 1.f);
}
/* With follow: the volume around where the camera is now (artfRendererDraw, before each frame). */
void artfIndirectFollow(ArtfScene* s);

/* Light from all around a surface at p facing n, from the probes near it, over pi. */
void artfIndirectAt(const ArtfIndirect* g, const float* p, const float* n, float* out);

/* The lights a frame holds itself: those past them are listed by the squares of the frame they reach. */
#define ARTF_FRAME_LIGHTS 16
#define ARTF_LIGHT_TILE 64       /* a square's side, in pixels: the CPU's tiles */
#define ARTF_LIGHT_SLICES 16     /* each square of the frame, in slices of depth: its clusters */
#define ARTF_CLUSTER_ENTRIES (1u << 22) /* the most the clusters list, all together (16 MB) */

/* What a frame needs to know, as the GPU's uniform block. Everything lit is in linear light. */
typedef struct ArtfFrameData {
    float viewProj[16];
    float planes[5][4]; /* left, right, bottom, top, near: inside where dot(plane, p) + w >= 0 */
    float cameraPos[4];
    float toSun[4];
    float sunColor[4];
    float sky[4];
    float ground[4];
    float background[4];
    uint32_t lightCount, instanceCount;
    float lodScale;     /* a mesh of error e fits at distance d when e * lodScale <= d; 0: lod is off */
    uint32_t hizLevels; /* the GPU's depth pyramid */
    ArtfLightData lights[ARTF_FRAME_LIGHTS];
    float focalX, focalY, nearW;
    uint32_t meshCount;
    uint32_t hiz[ARTF_HIZ_LEVELS][4]; /* of each level: where it starts, its width and its height */
    float look[4];      /* exposure (0: none), the fog's density, the sun's size, how long a level's fade is: its share of the distance */
    float fog[4];       /* its color, and the frame's width */
    /* Toward a pixel at (x, y) of clip space: eye[2] + x eye[0] + y eye[1]. (sky[3]: the environment's light, 0 when
     * there is none; ground[3]: 1 when it is shown.) */
    float eye[3][4];
    /* What comes after the frame (ArtfPostDesc): bloom, its threshold, occlusion, its radius; reflections, volumetric,
     * scattering, temporal (1 or 0); focus, aperture, and this frame's jitter, in pixels. */
    float post[3][4];
    float previous[16]; /* the frame before's viewProj, with no jitter: where a point was */
    /* Light that bounced: the grid's corner and strength (0: off); how far apart its probes are; how many each way. */
    float indirect[3][4];
    /* The look (ArtfPostDesc): the tone map, auto exposure (1 or 0), log2 of its key, its speed; log2 of its least and
     * most, saturation, contrast; temperature, tint, vignette, chromatic. */
    float grade[3][4];
    /* How many materials there are: on the GPU, where their ArtfMaterialMore begin, in rows. Fog by height: where it
     * is as dense as artfSceneSetFog says, and how fast it thins above that (0: it does not). */
    uint32_t materialCount;
    float fogBase, fogFalloff, motionBlur; /* motionBlur: ArtfPostDesc.lens's, the share of a frame */
    float lightSizes[ARTF_FRAME_LIGHTS]; /* of each light the frame holds, the radius of its ball (artfLightSetSize) */
    /* What is too small is not drawn (artfSceneSetCullSize): a ball of radius r at distance d is, when r * cullScale < d.
     * 0: all of it is. */
    float cullScale;
    float moving;       /* 1: something moved since the frame before, and the GPU has how (ArtfMotion); 0: the view alone */
    float view;         /* what the frame shows (ArtfView): 0, the picture */
    float gradeShown;   /* 1: grading in what a screen shows (ArtfLookDesc.gradeShown); 0: in ACEScct (artfGradeCct) */
    /* More of the look (ArtfPostDesc): lift, and 1 when there is grading (lift, gamma, gain or a wheel); 1 / gamma, the
     * grain; gain, the frame's number (for the grain); the shadows', midtones' and highlights' colors, after them the
     * look-up table's side (0: none), how much of it, and the flare; the histogram's darkest and brightest shares
     * left out, the speed down, and 1 when the histogram is on. */
    float grade2[7][4];
    /* An orthographic camera: how deep its depth goes (far - near), and where it ends, along the view; 0: perspective.
     * Its depth is (far - t) / (far - near) of t along the view, where a perspective's is near / t: 1 at near, 0 at far,
     * even across a triangle as w is 1. The eye's rows (eye) then hold the view's half width and half height, in the
     * world, and all rays go along eye[2], each from cameraPos + x eye[0] + y eye[1]. Before them, the slices of depth
     * the lights past the frame's own are listed in (artfSceneLightSlices): a point t along the view is in slice
     * floor(log2(t) slices[0] + slices[1]), held to 0 and ARTF_LIGHT_SLICES - 1. */
    float slices[2], ortho[2];
    /* Rays while drawing: ARTF_TRACED_SHADOWS and REFLECTIONS; which lights cast (ARTF_TRACED_SUN and bits 0 to 15);
     * how far the pattern of a pixel's rays turns this frame; rays to a light with a size. */
    uint32_t traced, tracedCasts, tracedFrame, tracedRays;
    /* Of each light the frame holds, four rows: a shape's way along and half its length (0: none); which way a
     * rectangle faces, and half its width (0: a tube); where its profile's vertical angles start, and its handle's number
     * + 1 if it has one (0: none); where horizontal ones start. */
    float lightShapes[ARTF_FRAME_LIGHTS][4][4];
    /* The frame shown, when the scene is drawn smaller (ArtfRendererOptions.renderScale): its width and height (the
     * scene's own when it is not), how much to sharpen it (ArtfPostDesc.screen.sharpen), and the number of this frame, for
     * patterns that turn from one frame to the next (soft shadows, with temporal antialiasing). */
    float output[4];
    /* Aerial perspective (ArtfSkyDesc.aerial): what the air takes of each color, a unit of the scene; what it adds
     * toward the eye, over what it takes: Rayleigh's part, times 1 + mu^2, and its haze's g; haze's, times that over
     * Cornette and Shanks's (1 + g^2 - 2 g mu)^1.5; the sky's, the same all ways, and 1 when it is on (0: none). */
    float aerial[4][4];
    /* 1 when a material is reactive (ArtfMaterialDesc.reactive): the instance each pixel shows goes after the frame
     * then, as when something moves; 0 else. How far the trees' cuts may be from their meshes (ArtfTrace.coarseReach).
     * With shadows from rays, how many of the lights past the frame's own cast, the first of artfSceneExtraLights'.
     * 1 when lights are sampled (ArtfShadowDesc.sampled); 0 else. */
    float more[4];
    /* Layers by transmittance (artfLayerSlice): 1 over how far along the view the nearest layer's front is, and the
     * slices to each doubling of that from there; 0 and 0 when there are no layers. */
    float layers[4];
} ArtfFrameData;

/* The pixel a point falls on (as artfTracedPlace finds it): x and y. */
ARTF_INLINE void artfSampledPixel(const ArtfFrameData* f, const float* p, uint32_t* x, uint32_t* y) {
    const float* m = f->viewProj;
    const float cx = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], cy = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
    const float cw = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
    const float inv = 1.f / artfMaxf(cw, 1e-6f);
    *x = (uint32_t)artfClampf(floorf((cx * inv * .5f + .5f) * f->fog[3]), 0.f, f->fog[3] - 1.f);
    *y = (uint32_t)artfClampf(floorf((.5f - cy * inv * .5f) * f->eye[1][3]), 0.f, f->eye[1][3] - 1.f);
}


/* What the air between the eye and a point this far away (in the scene's units), toward which mu is the cosine to the
 * sun, does to its light: keeps keep of each color and adds add (ArtfFrameData.aerial). The same arithmetic in the
 * lanes, lane by lane, and on the GPU (shaders.c's aerialSpv). */
ARTF_INLINE void artfAerialOf(const ArtfFrameData* f, float distance, float mu, float* keep, float* add) {
    const float g = f->aerial[1][3];
    const float k = (1.f + g * g) - 2.f * g * mu, mie = 1.f / (k * sqrtf(k)), mu2 = 1.f + mu * mu;
    for (int c = 0; c < 3; ++c) {
        keep[c] = artfExp2Fast(-(f->aerial[0][c] * distance) * 1.44269504f);
        add[c] = (mu2 * (f->aerial[1][c] + f->aerial[2][c] * mie) + f->aerial[3][c]) * (1.f - keep[c]);
    }
}

/* What a view other than the lit one does to a frame, on both backends: nothing after the frame (no bloom, exposure,
 * fog, grading or what follows motion), and black behind the scene. */
void artfFrameForView(ArtfFrameData* f, int view);

/* What views show, as colors of the frame (light is their square), the same on the GPU: for a number, by a hash; for
 * a level of detail; and a count, red to yellow to white. */
ARTF_INLINE void artfViewColor(uint32_t k, float out[3]) {
    uint32_t h = (k + 1u) * 0x9E3779B1u; /* (+ 1: the first is not grey) */
    h ^= h >> 15;
    h *= 0x85EBCA77u;
    h ^= h >> 13;
    for (int c = 0; c < 3; ++c) out[c] = (float)(96u + ((h >> (8 * c)) & 255u) % 160u) * (1.f / 255.f);
}
ARTF_INLINE void artfViewLevel(uint32_t level, float out[3]) {
    for (int c = 0; c < 3; ++c) out[c] = level == 0 || (uint32_t)c + 1 == level ? 1.f : .25f;
}
ARTF_INLINE void artfViewHeat(uint32_t count, float out[3]) {
    static const uint32_t steps[3] = {32, 8, 2};
    for (int c = 0; c < 3; ++c) out[c] = (float)(count * steps[c] < 255u ? count * steps[c] : 255u) * (1.f / 255.f);
}
/* Of depth: white at the eye, half at this far. */
#define ARTF_VIEW_DEPTH_HALF 10.f
/* Of motion: grey for what stands still (a byte's 128: not between two), and how far from it a pixel of motion goes. */
#define ARTF_VIEW_MOTION_STILL (128.f / 255.f)
#define ARTF_VIEW_MOTION_SCALE (1.f / 32.f)

ARTF_INLINE bool artfOrtho(const ArtfFrameData* f) { return f->ortho[0] > 0; }
/* How far a thing is, for its size on screen: in an orthographic view, all are as at 1 (focalY is then 2 / height). */
ARTF_INLINE float artfSeenDistance(const ArtfFrameData* f, float distance) { return f->ortho[0] > 0 ? 1.f : distance; }
/* Distance along the view from depth as the frame's planes keep it: 1 / w, or the orthographic depth. */
ARTF_INLINE float artfDepthAlong(const ArtfFrameData* f, float depth) { return f->ortho[0] > 0 ? f->ortho[1] - depth * f->ortho[0] : 1.f / depth; }
/* Where the eye is for a point of the world: the camera, or in an orthographic view the place on its plane behind it. */
ARTF_INLINE void artfEyeOf(const ArtfFrameData* f, const float* p, float* eye) {
    if (f->ortho[0] > 0) {
        const float along = (p[0] - f->cameraPos[0]) * f->eye[2][0] + (p[1] - f->cameraPos[1]) * f->eye[2][1] + (p[2] - f->cameraPos[2]) * f->eye[2][2];
        for (int k = 0; k < 3; ++k) eye[k] = p[k] - f->eye[2][k] * along;
    } else {
        for (int k = 0; k < 3; ++k) eye[k] = f->cameraPos[k];
    }
}

/* Of a pixel's second word of surface (what the GPU's surface target and the CPU's tiles leave when motion is
 * followed): with this bit, where it was a frame ago, in quarter pixels of the scene's size, from where it is, its
 * jitter taken out; 15 bits a way, x then y, each + 16384. Without, the instance that shows + 1 (0: none). What bones
 * and targets bend moves by its vertices, so it says this. */
#define ARTF_MOTION_VECTOR 0x80000000u
ARTF_INLINE uint32_t artfMotionPack(float dx, float dy) {
    const float x = artfClampf(dx * 4.f + 16384.5f, 0, 32767.f), y = artfClampf(dy * 4.f + 16384.5f, 0, 32767.f);
    return ARTF_MOTION_VECTOR | (uint32_t)x | (uint32_t)y << 15;
}
ARTF_INLINE void artfMotionUnpack(uint32_t word, float* dx, float* dy) {
    *dx = ((float)(word & 32767u) - 16384.f) * .25f;
    *dy = ((float)(word >> 15 & 32767u) - 16384.f) * .25f;
}

/* A ball of this radius, this far from the eye, is too small to draw (ArtfFrameData.cullScale). */
ARTF_INLINE bool artfTooSmall(const ArtfFrameData* f, float radius, float distance) { return f->cullScale > 0 && radius * f->cullScale < artfSeenDistance(f, distance); }
/* How wide a light the frame holds is, for soft shadows: its ball and the most of its shape. */
ARTF_INLINE float artfLightSoftSize(const ArtfFrameData* f, uint32_t i) { return f->lightSizes[i] + artfMaxf(f->lightShapes[i][0][3], f->lightShapes[i][1][3]); }
/* Of the lights a frame of the scene holds, one has a shape or a profile (lights.c). */
bool artfSceneShapedLights(const ArtfScene* s);
/* It has a shape or a profile. */
ARTF_INLINE bool artfLightShaped(const ArtfFrameData* f, uint32_t i) { return f->lightShapes[i][0][3] > 0 || f->lightShapes[i][2][3] > 0; }


/* For tests: false makes the CPU's paths only bounce into light from all around, not aim at it as well (trace.c). */
extern bool artfTraceAim;
/* For tests: false makes the CPU's paths read textures at their finest, with no cone (trace.c's texelCone). */
extern bool artfTraceCones;
extern int artfTraceLightWays; /* (for tests: lights with a size found both ways, weighed (MIS): 0; aimed at alone: 1; bounced into alone: 2) */
/* For tests: false makes the CPU's rasterizer leave highlights as narrow as their roughness, where the normal turns
 * over a pixel (kernels.inc's turnAt and shading.inc's Widened). */
extern bool artfWidenHighlights;

/* (artfLog2Fast, artfExp2Fast and sRGB: core/color.h) */
/* What fog by height makes of the fog's density at a height: as much as that, times this. */
ARTF_INLINE float artfFogHeight(const ArtfFrameData* f, float y) {
    if (!(f->fogFalloff > 0)) return 1.f;
    return artfMinf(artfExp2Fast((f->fogBase - y) * f->fogFalloff * 1.44269504f), 16.f);
}
/* How each instance moved since the last step (motion.c): of each, twelve floats, the rows of what takes a point
 * of the world from where it is to where it was. A renderer keeps its own. */
typedef struct ArtfMotionPose {
    float* was;         /* where each of its mesh's vertices was, by the slots of its meshlets (positions'), three floats */
    float* back;        /* from where each is to where it was at the step before */
    uint32_t slots;
    uint32_t mesh;      /* the scene's mesh they are of + 1 (an instance that bends takes its levels in turn) */
    uint64_t version;   /* the mesh's deformVersion they are at */
} ArtfMotionPose;
typedef struct ArtfMotion {
    float* at;          /* by handle: where each was at the last step */
    uint8_t* there;     /* by handle: it was */
    ArtfMotionPose* poses; /* by handle: what bones and targets moved, for those that bend */
    size_t handles;     /* room for so many */
    float* motion;      /* by instance */
    uint8_t* bent;      /* by instance: a pose moved its vertices since the last step */
    const float** back; /* by instance, with vertices: its pose's back, where bent; else NULL */
    size_t count, capacity;
    uint64_t scene, version; /* version: the motion changed */
    bool moving;
} ArtfMotion;
/* vertices: what each vertex of what bends did, from the meshes' positions on the CPU (back); else only which bent. */
void artfMotionStep(ArtfMotion* m, const ArtfScene* s, bool vertices);
void artfMotionFree(ArtfMotion* m);

/* sin and cos of any angle by polynomials, within 4e-6: the same bytes everywhere, as the C library's are not. */
ARTF_INLINE float artfSinFast(float x) {
    x -= 6.28318531f * (float)(int32_t)(x * .159154943f + (x < 0 ? -.5f : .5f));
    if (x > 1.57079633f) x = 3.14159265f - x;
    else if (x < -1.57079633f) x = -3.14159265f - x;
    const float x2 = x * x;
    return x * (1.f - x2 * (.16666667f - x2 * (.0083333333f - x2 * (.00019841270f - x2 * .0000027557319f))));
}
ARTF_INLINE float artfCosFast(float x) { return artfSinFast(x + 1.57079633f); }
/* acos and atan2 by polynomials (Abramowitz and Stegun's 4.4.45, within 7e-5; a fit of atan within 1e-5), the same in
 * the shaders: for a light's profile. */
ARTF_INLINE float artfAcosFast(float x) {
    const float a = artfMinf(fabsf(x), 1.f);
    const float r = sqrtf(1.f - a) * (1.5707288f + a * (-.2121144f + a * (.0742610f - a * .0187293f)));
    return x < 0 ? 3.14159265f - r : r;
}
ARTF_INLINE float artfAtan2Fast(float y, float x) {
    const float ax = fabsf(x), ay = fabsf(y), big = artfMaxf(ax, ay), small = artfMinf(ax, ay);
    const float z = small / artfMaxf(big, 1e-30f), z2 = z * z;
    float r = z * (.99997726f + z2 * (-.33262347f + z2 * (.19354346f + z2 * (-.11643287f + z2 * (.05265332f - z2 * .01172120f)))));
    r = ay > ax ? 1.57079633f - r : r;
    r = x < 0 ? 3.14159265f - r : r;
    return y < 0 ? -r : r;
}

/* The shading model (shading/shading.inc), in floats: artfShade..., for trace.c and for what the kernels work out
 * lane by lane. */
typedef struct ArtfShade3 { float x, y, z; } ArtfShade3;
ARTF_INLINE float artfShadeDot(ArtfShade3 a, ArtfShade3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
ARTF_INLINE ArtfShade3 artfShadeNormalize(ArtfShade3 a) {
    const float l = 1.f / sqrtf(artfMaxf(artfShadeDot(a, a), 1e-30f));
    return (ArtfShade3){a.x * l, a.y * l, a.z * l};
}
#define S_FN(name) artfShade##name
#define S_DEF ARTF_INLINE
#define S_SELF
#define S_PASS
#define S_F float
#define S_V3 ArtfShade3
#define S_FLAG bool
#define S_WIDE 0
#define S_C(x) (x)
#define S_ADD(a, b) ((a) + (b))
#define S_SUB(a, b) ((a) - (b))
#define S_MUL(a, b) ((a) * (b))
#define S_DIV(a, b) ((a) / (b))
#define S_MIN(a, b) artfMinf(a, b)
#define S_MAX(a, b) artfMaxf(a, b)
#define S_SQRT(a) sqrtf(a)
#define S_GT(a, b) ((a) > (b))
#define S_SELECT(c, a, b) ((c) ? (a) : (b))
#define S_PICK(c, a, b) ((c) ? (a) : (b))
#define S_V3_OF(x, y, z) ((ArtfShade3){x, y, z})
#define S_X(v) (v).x
#define S_Y(v) (v).y
#define S_Z(v) (v).z
#define S_DOT3(a, b) artfShadeDot(a, b)
#define S_NORMALIZE(a) artfShadeNormalize(a)
#define S_LOG2(a) artfLog2Fast(a)
#define S_EXP2(a) artfExp2Fast(a)
#define S_COS(a) artfCosFast(a)
#define S_CLAMP(a, lo, hi) artfClampf(a, lo, hi)
#include "shading/shading.inc"

/* Linearly transformed cosines for the highlight (src/shading/ltc.inc, made by tools/ltc/fit.py): of each roughness,
 * from ARTF_LTC_MIN_ROUGH to 1, and sqrt(1 - n . v), from 0 to 1, ARTF_LTC_SIZE each way, five numbers: M^-1's a, b,
 * c, d, and the lobe's integral. In scene/lights.c; on the GPU, in the lights' buffer past the profiles. */
#define ARTF_LTC_SIZE 32
#define ARTF_LTC_MIN_ROUGH .06f
#define ARTF_LTC_FLOATS (ARTF_LTC_SIZE * ARTF_LTC_SIZE * 5)
extern const float artfLtcTable[ARTF_LTC_FLOATS];
/* Where a roughness and n . v are in it: the cell's corner (x along roughness, y along the angle) and how far past. */
ARTF_INLINE void artfLtcPlace(float rough, float ndv, int* x, int* y, float* tx, float* ty) {
    const float fx = (artfClampf(rough, ARTF_LTC_MIN_ROUGH, 1.f) - ARTF_LTC_MIN_ROUGH) * ((float)(ARTF_LTC_SIZE - 1) / (1.f - ARTF_LTC_MIN_ROUGH));
    const float fy = sqrtf(artfClampf(1.f - ndv, 0.f, 1.f)) * (float)(ARTF_LTC_SIZE - 1);
    *x = artfMini((int)fx, ARTF_LTC_SIZE - 2);
    *y = artfMini((int)fy, ARTF_LTC_SIZE - 2);
    *tx = fx - (float)*x;
    *ty = fy - (float)*y;
}
/* Its five numbers there, bilinear. */
ARTF_INLINE void artfLtcAt(float rough, float ndv, float out[5]) {
    int x, y;
    float tx, ty;
    artfLtcPlace(rough, ndv, &x, &y, &tx, &ty);
    const float* c00 = artfLtcTable + (x * ARTF_LTC_SIZE + y) * 5;
    const float* c10 = c00 + ARTF_LTC_SIZE * 5;
    for (int k = 0; k < 5; ++k) {
        const float a = c00[k] + (c10[k] - c00[k]) * tx, b = c00[k + 5] + (c10[k + 5] - c00[k + 5]) * tx;
        out[k] = a + (b - a) * ty;
    }
}

/* Grading in ACEScct (the plan's 2.1), of the light before the tone curve, in place: into ACES's AP1 colors (from
 * linear sRGB, Bradford's D65 to D60), into ACEScct (a log2 with a straight toe), then lift, gamma and gain, the wheels
 * and the table (ArtfFrameData.grade2), as the shown grading's, and back. The GPU's final pass does the same
 * (postShaders.inc's gradeCct). lut: the scene's, or NULL. */
ARTF_INLINE float artfCctOf(float x) { return x <= .0078125f ? 10.5402377f * x + .0729055342f : (artfLog2Fast(x) + 9.72f) * (1.f / 17.52f); }
ARTF_INLINE float artfCctLinear(float c) { return c <= .155251142f ? (c - .0729055342f) * (1.f / 10.5402377f) : artfExp2Fast(c * 17.52f - 9.72f); }
ARTF_INLINE void artfGradeCct(const ArtfFrameData* f, const float* lut, float* v) {
    const bool curves = f->grade2[0][3] > 0;
    const int size = (int)f->grade2[3][3];
    if (!curves && !(size >= 2 && lut)) return;
    const float ap1[3] = {.6130974f * v[0] + .3395231f * v[1] + .0473795f * v[2], .0701937f * v[0] + .9163539f * v[1] + .0134524f * v[2],
                          .0206156f * v[0] + .1095698f * v[1] + .8698147f * v[2]};
    float c[3];
    for (int k = 0; k < 3; ++k) c[k] = artfCctOf(ap1[k]);
    if (curves) {
        for (int k = 0; k < 3; ++k) {
            const float o = f->grade2[2][k] * (c[k] + f->grade2[0][k] * (1.f - c[k]));
            c[k] = f->grade2[1][k] == 1 ? o : o > 0 ? artfExp2Fast(artfLog2Fast(o) * f->grade2[1][k]) : 0.f;
        }
        const float l = artfClampf(.2126f * c[0] + .7152f * c[1] + .0722f * c[2], 0.f, 1.f), rest = 1.f - l;
        const float dark = rest * rest, middle = 2.f * l * rest, bright = l * l;
        for (int k = 0; k < 3; ++k) c[k] *= dark * f->grade2[3][k] + middle * f->grade2[4][k] + bright * f->grade2[5][k];
    }
    if (size >= 2 && lut) {
        float t[3];
        int at[3];
        for (int k = 0; k < 3; ++k) {
            const float q = artfClampf(c[k], 0.f, 1.f) * (float)(size - 1);
            at[k] = artfMini((int)q, size - 2);
            t[k] = q - (float)at[k];
        }
        const float amount = f->grade2[4][3];
        for (int k = 0; k < 3; ++k) {
#define LUT_AT(dx, dy, dz) lut[(((size_t)(at[2] + (dz)) * (size_t)size + (size_t)(at[1] + (dy))) * (size_t)size + (size_t)(at[0] + (dx))) * 3 + (size_t)k]
            const float a = LUT_AT(0, 0, 0) + (LUT_AT(1, 0, 0) - LUT_AT(0, 0, 0)) * t[0], b = LUT_AT(0, 1, 0) + (LUT_AT(1, 1, 0) - LUT_AT(0, 1, 0)) * t[0];
            const float d = LUT_AT(0, 0, 1) + (LUT_AT(1, 0, 1) - LUT_AT(0, 0, 1)) * t[0], e = LUT_AT(0, 1, 1) + (LUT_AT(1, 1, 1) - LUT_AT(0, 1, 1)) * t[0];
#undef LUT_AT
            const float nearer = a + (b - a) * t[1], farther = d + (e - d) * t[1];
            c[k] += (nearer + (farther - nearer) * t[2] - c[k]) * amount;
        }
    }
    for (int k = 0; k < 3; ++k) c[k] = artfMaxf(artfCctLinear(c[k]), 0.f);
    v[0] = 1.7048587f * c[0] - .6217160f * c[1] - .0832542f * c[2];
    v[1] = -.1300768f * c[0] + 1.1407357f * c[1] - .0105536f * c[2];
    v[2] = -.0239640f * c[0] - .1289755f * c[1] + 1.1530393f * c[2];
    for (int k = 0; k < 3; ++k) v[k] = v[k] > 0 ? artfMinf(v[k], 65504.f) : 0.f;
}

/* A light's profile (artfLightSetProfile): vertical angles from 0 to 180 degrees, ends included, in rows by horizontal
 * angles from 0 to 360. */
#define ARTF_PROFILE_V 96
#define ARTF_PROFILE_H 16
#define ARTF_PROFILE_FLOATS (ARTF_PROFILE_V * ARTF_PROFILE_H)
/* What it gives toward dir, from the light: axis is where vertical angles start, ref where horizontal ones do;
 * bilinear, round and round. */
ARTF_INLINE float artfProfileAt(const float* table, const float* axis, const float* ref, const float* dir) {
    const float side[3] = {axis[1] * ref[2] - axis[2] * ref[1], axis[2] * ref[0] - axis[0] * ref[2], axis[0] * ref[1] - axis[1] * ref[0]};
    const float x = artfAcosFast(dir[0] * axis[0] + dir[1] * axis[1] + dir[2] * axis[2]) * ((float)(ARTF_PROFILE_V - 1) / 3.14159265f);
    float y = artfAtan2Fast(dir[0] * side[0] + dir[1] * side[1] + dir[2] * side[2], dir[0] * ref[0] + dir[1] * ref[1] + dir[2] * ref[2]) * ((float)ARTF_PROFILE_H / 6.28318531f);
    y = y < 0 ? y + (float)ARTF_PROFILE_H : y;
    const float fx = artfMinf(floorf(x), (float)(ARTF_PROFILE_V - 2)), fy = floorf(y), tx = x - fx, ty = y - fy;
    const int i = (int)fx, j0 = (int)fy % ARTF_PROFILE_H, j1 = (j0 + 1) % ARTF_PROFILE_H;
    const float* r0 = table + j0 * ARTF_PROFILE_V + i;
    const float* r1 = table + j1 * ARTF_PROFILE_V + i;
    const float a = r0[0] + (r0[1] - r0[0]) * tx, b = r1[0] + (r1[1] - r1[0]) * tx;
    return a + (b - a) * ty;
}

/* A light with a shape (artfLightSetShape), for a surface at p facing n, seen along view (toward the eye), this rough.
 * shape: along and half its length; facing and half its width (0: a tube). ball: a tube's radius; reach2: 1 / the
 * light's radius squared. Out: toward its
 * point nearest p, which its diffuse light comes from, and that light's share (a rectangle's light goes one way, and
 * less to the side); toward its point nearest the mirror ray, which its highlight comes from, and what that keeps as it
 * is spread over as long and as wide as the shape looks (Karis, 2013); the square of the distance to the first point,
 * which its light fades with. Those are what a clear coat, sheen and what is under the surface take, and a ball's or
 * a point's light. A shape's own light is over all of it (shading.inc's AreaLight): lambert, its diffuse light (n . l
 * and fade in it); glossy, its highlight, which the Fresnel at toHighlight and the surface's scale multiply. */
typedef struct ArtfShapeLit { float toDiffuse[3], diffuse, toHighlight[3], highlight, distance2, lambert, glossy; } ArtfShapeLit;
ARTF_INLINE void artfShapeLight(const float* shape, const float* center, float ball, float reach2, const float* p, const float* n, const float* view, float rough, ArtfShapeLit* out) {
    const float* u = shape;
    const float* f = shape + 4;
    const float hl = shape[3], hw = shape[7];
    const float d[3] = {center[0] - p[0], center[1] - p[1], center[2] - p[2]};
    const float side[3] = {f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]};
    const float along = artfClampf(-(d[0] * u[0] + d[1] * u[1] + d[2] * u[2]), -hl, hl);
    const float across = hw > 0 ? artfClampf(-(d[0] * side[0] + d[1] * side[1] + d[2] * side[2]), -hw, hw) : 0.f;
    const float before = -(d[0] * f[0] + d[1] * f[1] + d[2] * f[2]); /* how far p is in front of a rectangle */
    float q[3], h[3];
    for (int k = 0; k < 3; ++k) q[k] = d[k] + u[k] * along + side[k] * across;
    const float q2 = artfMaxf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2], 1e-8f), qi = 1.f / sqrtf(q2);
    for (int k = 0; k < 3; ++k) out->toDiffuse[k] = q[k] * qi;
    out->diffuse = hw > 0 ? artfMaxf(before, 0) * qi : 1.f;
    out->distance2 = q2;
    const float twice = 2.f * (n[0] * view[0] + n[1] * view[1] + n[2] * view[2]);
    const float r[3] = {n[0] * twice - view[0], n[1] * twice - view[1], n[2] * twice - view[2]};
    if (hw > 0) {
        /* where the mirror ray meets the rectangle's plane, kept on it; the nearest point when it goes away */
        const float toward = r[0] * f[0] + r[1] * f[1] + r[2] * f[2];
        const float t = toward < -1e-4f ? before / -toward : 0.f;
        float e[3];
        for (int k = 0; k < 3; ++k) e[k] = r[k] * t - d[k];
        const float a = toward < -1e-4f ? artfClampf(e[0] * u[0] + e[1] * u[1] + e[2] * u[2], -hl, hl) : along;
        const float b = toward < -1e-4f ? artfClampf(e[0] * side[0] + e[1] * side[1] + e[2] * side[2], -hw, hw) : across;
        for (int k = 0; k < 3; ++k) h[k] = d[k] + u[k] * a + side[k] * b;
    } else {
        /* the point of the line nearest the mirror ray, then of the ball around it */
        float l0[3], ld[3];
        for (int k = 0; k < 3; ++k) {
            l0[k] = d[k] - u[k] * hl;
            ld[k] = u[k] * (2.f * hl);
        }
        const float rl = r[0] * ld[0] + r[1] * ld[1] + r[2] * ld[2], r0 = r[0] * l0[0] + r[1] * l0[1] + r[2] * l0[2];
        const float t = artfClampf((r0 * rl - (l0[0] * ld[0] + l0[1] * ld[1] + l0[2] * ld[2])) / artfMaxf(ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2] - rl * rl, 1e-8f), 0, 1);
        for (int k = 0; k < 3; ++k) h[k] = l0[k] + ld[k] * t;
        const float hr = h[0] * r[0] + h[1] * r[1] + h[2] * r[2];
        float c[3];
        for (int k = 0; k < 3; ++k) c[k] = r[k] * hr - h[k];
        const float share = artfMinf(ball / artfMaxf(sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]), 1e-6f), 1.f);
        for (int k = 0; k < 3; ++k) h[k] += c[k] * share;
    }
    const float distance = sqrtf(artfMaxf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2], 1e-8f)), hi = 1.f / distance;
    for (int k = 0; k < 3; ++k) out->toHighlight[k] = h[k] * hi;
    const float a = rough * rough, longer = artfMinf(a + hl * .5f * hi, 1.f);
    const float wider = artfMinf(a + (hw > 0 ? hw : ball) * .5f * hi, 1.f);
    out->highlight = a / longer * (hw > 0 ? a / wider : (a / wider) * (a / wider));
    out->highlight = hw > 0 && before <= 0 ? 0.f : out->highlight;
    /* the light of all of it */
    float ltc[5];
    artfLtcAt(rough, artfMaxf(n[0] * view[0] + n[1] * view[1] + n[2] * view[2], 0.f), ltc);
    ArtfShade3 toward;
    const ArtfShade3 dd = {d[0], d[1], d[2]}, uu = {u[0], u[1], u[2]}, ff = {f[0], f[1], f[2]}, nn = {n[0], n[1], n[2]}, vv = {view[0], view[1], view[2]};
    artfShadeAreaLight(dd, uu, hl, ff, hw, ball, reach2, nn, vv, ltc[0], ltc[1], ltc[2], ltc[3], ltc[4], &out->lambert, &out->glossy, &toward);
    out->toHighlight[0] = toward.x;
    out->toHighlight[1] = toward.y;
    out->toHighlight[2] = toward.z;
}

/* The same for a light that may have no shape: a ball's or a point's, as shading.inc's BallNearest but for its
 * rounding. */
ARTF_INLINE void artfLightPlace(const float* shape, const float* center, float ball, float reach2, const float* p, const float* n, const float* view, float rough, ArtfShapeLit* out) {
    if (shape[3] > 0) {
        artfShapeLight(shape, center, ball, reach2, p, n, view, rough, out);
        return;
    }
    out->lambert = out->glossy = 0;
    const float d[3] = {center[0] - p[0], center[1] - p[1], center[2] - p[2]};
    const float d2 = artfMaxf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2], 1e-8f), inv = 1.f / sqrtf(d2);
    for (int k = 0; k < 3; ++k) out->toDiffuse[k] = out->toHighlight[k] = d[k] * inv;
    out->diffuse = out->highlight = 1.f;
    out->distance2 = d2;
    if (ball > 0) {
        const float twice = 2.f * (n[0] * view[0] + n[1] * view[1] + n[2] * view[2]);
        const float r[3] = {n[0] * twice - view[0], n[1] * twice - view[1], n[2] * twice - view[2]};
        const float along = d[0] * r[0] + d[1] * r[1] + d[2] * r[2];
        float c[3], h[3];
        for (int k = 0; k < 3; ++k) c[k] = r[k] * along - d[k];
        const float share = artfMinf(ball / artfMaxf(sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]), 1e-6f), 1.f);
        for (int k = 0; k < 3; ++k) h[k] = d[k] + c[k] * share;
        const float hi = 1.f / sqrtf(artfMaxf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2], 1e-12f));
        for (int k = 0; k < 3; ++k) out->toHighlight[k] = h[k] * hi;
        const float a = rough * rough, kept = a / artfMinf(a + ball * .5f * inv, 1.f);
        out->highlight = kept * kept;
    }
}

/* What a thin film makes of what a surface reflects head on, seen at cos (n . v): artfShadeIridescence, in place. */
ARTF_INLINE void artfIridescence(float* f0, float cos, float amount, float ior, float thickness) {
    const ArtfShade3 c = artfShadeIridescence((ArtfShade3){f0[0], f0[1], f0[2]}, cos, amount, ior, thickness);
    f0[0] = c.x;
    f0[1] = c.y;
    f0[2] = c.z;
}

#define ARTF_METER_X 32 /* auto exposure's grid */
#define ARTF_METER_Y 18
#define ARTF_HISTOGRAM_X 64 /* with a histogram: a grid twice as dense, */
#define ARTF_HISTOGRAM_Y 36
#define ARTF_HISTOGRAM_BINS 64 /* into bins of stops from -12 to 12 */
#define ARTF_LUT_MAX 65 /* the largest side of a look-up table */

/* A step of the probes: their rays, from what a frame says of light (needs artfSceneTraceSync). */
typedef struct ArtfPool ArtfPool;
void artfSceneIndirectStep(ArtfScene* s, const ArtfFrameData* frame, ArtfPool* pool);

/* Lights past the frame's own, in handle order: how many, into out (ARTF_MAX_LIGHTS of room). */
uint32_t artfSceneExtraLights(const ArtfScene* s, const ArtfFrameData* frame, ArtfLightData* out);
/* How depth along the view is cut into ARTF_LIGHT_SLICES for those lights: from the nearest any reaches to the farthest,
 * each slice as many times deeper as the one before; into frame->slices. */
void artfSceneLightSlices(ArtfFrameData* frame, const ArtfLightData* lights, uint32_t count);
/* The slice a point t along the view is in (as the GPU finds it, but for how log2 rounds there). */
ARTF_INLINE float artfLightSliceAt(const ArtfFrameData* f, float t) { return log2f(t > 1e-30f ? t : 1e-30f) * f->slices[0] + f->slices[1]; }
/* Of each square of the frame (ARTF_LIGHT_TILE a side, by rows), which of those lights reach it, in their order (their
 * places in what artfSceneExtraLights gave):
 * - tiles (the CPU's; NULL: not made): of each square, a count, then the places; squares x (1 + count).
 * - clusters (the GPU's; NULL: not made): of each square, of each of its slices (artfSceneLightSlices), where its list
 *   starts and how long it is, two numbers; after them all, the lists. Into *clusters, made larger when it must be
 *   (*room: how many numbers it holds); how many it took, returned.
 * A light past the frame adds nothing where it does not reach, so the two give the same picture, unless the clusters
 * would list more than ARTF_CLUSTER_ENTRIES: how many places were left out, into omitted
 * (ArtfFrameStats.lightsOmitted). */
size_t artfSceneLightTiles(const ArtfFrameData* frame, int width, int height, const ArtfLightData* lights, uint32_t count, uint32_t* tiles, uint32_t** clusters, size_t* room, uint32_t* omitted);

/* Moves the frame within a pixel, by the n-th of Halton's points, for temporal antialiasing: viewProj and post[2]. */
void artfFrameJitter(ArtfFrameData* frame, uint32_t n, int width, int height);

/* One square of the shadow atlas: a place seen from a light. Clip space: x and y from -1 to 1, depth from 0 to
 * 1. y grows with the map's rows. */
typedef struct ArtfShadowView {
    float m[16];     /* by rows */
    float info[4];   /* its corner in the atlas, in texels; how wide a texel is (per unit of distance, for a
                      * lamp); how much depth is forgiven */
    float place[4];  /* a lamp's position and reach; for the sun, where it is, and reach 0 */
    float size[4];   /* its side, in texels */
} ArtfShadowView;

/* All the shadows of a frame: the GPU's uniform block. */
typedef struct ArtfShadowFrame {
    float splits[4];    /* where each of the sun's maps ends, from the eye */
    uint32_t sun[4];    /* its first view + 1 (0: it casts none), how many, a big map's side, the atlas's height */
    uint32_t lights[ARTF_FRAME_LIGHTS][4]; /* by the frame's lights: first view + 1, and views << 8; 0: none */
    ArtfShadowView views[ARTF_SHADOW_VIEWS];
    uint32_t extra[ARTF_SHADOW_EXTRA][4];  /* the same, by the first of the lights past them (artfSceneExtraLights) */
} ArtfShadowFrame;

/* Says when a map would come out as it was: this view, and all that is not an instance, are the same. */
uint64_t artfShadowViewKey(const ArtfScene* s, const ArtfShadowView* view, uint32_t side);
/* Did an instance change, since seen changes had been made, where this view reaches? */
bool artfShadowViewTouched(const ArtfScene* s, const ArtfShadowView* view, uint32_t side, uint64_t seen);
#define ARTF_SHADOW_CHANGES 1024 /* more than this since a frame, and all maps are drawn */

/* What a frame of a background pass needs, as the GPU's uniform block (binding 11): the renderer's part, then what the
 * pass's frame wrote, from byte 256 on. */
typedef struct ArtfBackgroundBlock {
    uint32_t size[4]; /* width, height, bloom levels, 0 */
    float image[4];   /* exposure, bloom, 0, 0 */
    uint32_t bloom[ARTF_BLOOM_LEVELS][4]; /* of each level: where it starts, its width and its height */
    uint32_t unused[(256 - 32 - ARTF_BLOOM_LEVELS * 16) / 4];
    uint8_t params[ARTF_BACKGROUND_PARAMS];
} ArtfBackgroundBlock;
#define ARTF_BACKGROUND_PARAMS_AT 16 /* in vec4s */

/* What rays read (trace.c), laid out for the GPU as well. A node with count 0 has its children at first
 * and first + 1; any other has that many triangles, or instances, from first on. */
#define ARTF_TRACE_STACK 48

typedef struct ArtfTraceNode {
    float lo[3];
    uint32_t first;
    float hi[3];
    uint32_t count;
} ArtfTraceNode;

/* A node of a mesh's tree as rays walk it (phase 5.6): four ways, from two levels of the tree above (ArtfTraceNode's,
 * which stays for refitting), so a quarter of the nodes, each twice as large. Its box, and its children's boxes in 8
 * bits of it (low rounded down, high rounded up: lo + q x scale holds each); each child is ARTF_WIDE_NONE, an inner
 * node's index, or a leaf: ARTF_WIDE_LEAF | (its count - 1) << 28 | its first triangle. Four vec4s on the GPU: lo and
 * child 0; scale and child 1; the boxes' first 16 bytes; their last 8, and children 2 and 3. */
#define ARTF_WIDE_NONE 0xFFFFFFFFu
#define ARTF_WIDE_LEAF 0x80000000u
#define ARTF_TRACE_WIDE_STACK 64 /* what a walk of these keeps: three of four children a level, twenty levels deep */
typedef struct ArtfTraceWide {
    float lo[3];
    uint32_t child0;
    float scale[3];
    uint32_t child1;
    uint32_t boxes[6]; /* bytes: child k's low x, y, z and high x, y, z from 6k */
    uint32_t child2, child3;
} ArtfTraceWide;
ARTF_INLINE uint32_t artfWideChild(const ArtfTraceWide* w, int k) { return k == 0 ? w->child0 : k == 1 ? w->child1 : k == 2 ? w->child2 : w->child3; }
ARTF_INLINE uint32_t artfWideByte(const ArtfTraceWide* w, int k, int j) { const int at = 6 * k + j; return w->boxes[at >> 2] >> ((at & 3) * 8) & 255u; }

extern bool artfTraceReinsert;
extern bool artfTracedLiftNone; /* trace.c: rays from what is drawn start off it by the reach floats need alone, not by
                                 * the level's error (tests: to see what that prevents) */ /* trace.c: reinsertion after a mesh's tree is built (tests turn it off to compare) */

typedef struct ArtfTraceVertex {
    float p[3], u;
    float n[3], v;
    uint32_t color;
    float back[3];      /* from where it is to where it was a pose ago */
} ArtfTraceVertex;

typedef struct ArtfTraceTriangle { uint32_t v[3], pad; } ArtfTraceTriangle; /* pad: which of its mesh's triangles */

typedef struct ArtfTraceInstance {
    float toObject[12]; /* the rows of its inverse */
    uint32_t root, material;
    float opacity;
    uint32_t twoSided;  /* bit 0: both sides show; the rest, its mesh's first triangle, for the ray tracing hardware */
} ArtfTraceInstance;

/* Where tracing last saw an instance, by its handle. (A handle used again looks as if it jumped: the filter sees that.) */
typedef struct ArtfTraceSeen { float at[12]; bool there; } ArtfTraceSeen;

/* What a frame's rays start from: the GPU's uniform block. */
typedef struct ArtfTraceFrame {
    float eye[4], right[4], up[4], forward[4]; /* a pixel's ray: forward + x right + y up, x and y in -1..1 */
                                               /* eye[3] 1: orthographic, from eye + x right + y up, forward */
    uint32_t width, height, done, samples;     /* done: what is gathered so far */
    uint32_t bounces, paths, pad[2];           /* pad[0], on the GPU: done, with this frame's samples; pad[1]: the first of a filter's frames */
    float previous[16];                        /* the frame before's view and projection, for the filter */
    uint32_t blue[512][4];                     /* sampling.h's tile of blue noise: two ranks a word, the first low */
} ArtfTraceFrame;

/* What a pixel's first surface was, for the filter: where, which way it faced (packed) and what color. */
typedef struct ArtfTraceGuide {
    float place[4];
    float albedo[4];
    float shift[4];  /* in the world, what bones moved it by since the frame before, backwards */
} ArtfTraceGuide;

uint32_t artfNormalPack(const float* n);

typedef struct ArtfTraceRange {
    uint32_t firstNode, nodeEnd, firstVertex;
    uint32_t firstWide, wideEnd;        /* its nodes four ways */
    float coarseError;                  /* traced by its graph's cut (ArtfTraceDesc.coarse): how far that is, in its units */
    uint32_t firstTriangle, triangleCount; /* its triangles, one after the other: for the ray tracing hardware */
    uint64_t deformVersion;
    bool moved;         /* its vertices have somewhere they were */
} ArtfTraceRange;

typedef struct ArtfTrace {
    ArtfTraceDesc desc;
    ARTF_VEC(ArtfTraceRange) ranges;        /* of each mesh */
    ARTF_VEC(ArtfTraceNode) nodes;          /* of every mesh, one tree after the other */
    ARTF_VEC(ArtfTraceTriangle) triangles;
    ARTF_VEC(ArtfTraceVertex) vertices;
    ARTF_VEC(uint32_t) roots;               /* of each mesh */
    ARTF_VEC(ArtfTraceWide) wide;           /* the trees rays walk: of every mesh, four ways (ArtfTraceInstance.root's) */
    ARTF_VEC(uint32_t) wideFrom;            /* of each of those, the node of nodes each child is (UINT32_MAX: none) */
    ARTF_VEC(uint32_t) wideRoots;           /* of each mesh */
    float coarseReach;                      /* of the instances traced, the farthest a cut is from its mesh, in the world */
    ARTF_VEC(ArtfTraceNode) top;            /* over the instances */
    ARTF_VEC(ArtfTraceInstance) instances;
    ARTF_VEC(uint32_t) instanceMeshes;      /* of each of those, its mesh, */
    ARTF_VEC(float) instanceWorlds;         /* and the rows of where it is: for the ray tracing hardware */
    ARTF_VEC(uint32_t) instanceHandles;     /* and its handle as the program has it, 0 for a particle's: for picking */
    bool held;                              /* a pick made the trees anew: the next frame keeps what moved */
    ARTF_VEC(float) motion;                 /* of each of those, twelve floats: from where a point of it is to where it was */
    ARTF_VEC(ArtfTraceSeen) seen;
    uint64_t motionVersion;
    bool motionLive;                        /* something moved: next frame, if nothing does, it stands still again */
    uint64_t instanceVersion, structureVersion, materialVersion;
    bool particles;             /* the top holds particles: the frame is all traced */
    uint64_t meshVersion, topVersion;       /* for backends that keep a copy */
    uint64_t meshRemovals;
} ArtfTrace;

/* The scene's background pass, and what tells a backend when to take its parts again: each changes when what it says
 * of may have. */
typedef struct ArtfBackground {
    bool on;
    ArtfBackgroundPass pass;
    uint32_t* spirv;      /* the scene's copy */
    uint64_t moduleStamp, dataStamp, streamStamp;
} ArtfBackground;

struct ArtfScene {
    uint64_t id; /* no two scenes of a program share one */
    /* The thread drawing it, while artfRendererDraw does (artfThreadId), else 0: what a change from another thread is
     * told by. Read without a lock, as a check of the rule and not a guarantee. */
    volatile uintptr_t drawingThread;
    ARTF_VEC(ArtfMeshData) meshes;
    ARTF_VEC(ArtfTextureData) textures;     /* 0 is white */
    ARTF_VEC(ArtfMaterialData) materials;   /* 0 is grey */
    ARTF_VEC(ArtfMaterialMore) materialsMore; /* one for each of those */
    uint32_t layeredMaterials;              /* of those, how many have what drawing reads of them (ARTF_MORE_DRAWN) */
    uint32_t moreMaterials;                 /* and how many have anything (tracing reads all) */
    ARTF_VEC(ArtfInstanceData) instances;   /* dense, in no order */
    ARTF_VEC(uint32_t) instanceHandles;     /* of each dense instance */
    ARTF_VEC(uint32_t) handleSlots;         /* of each handle's number - 1: dense index, or the next free number */
    ARTF_VEC(uint8_t) handleGenerations;    /* the top byte of a handle: it changes when one is removed */
    uint32_t freeHandle;                    /* the free list's head; 0: none */
    /* What changed, for backends that keep a copy. */
    uint64_t instanceVersion;   /* a transform or a material */
    uint64_t tracedVersion;     /* the same, of what is not a particle: what tracing and shadows see */
    float shadowChanges[ARTF_SHADOW_CHANGES][4]; /* where an instance was or went, the last ones: a ball */
    uint64_t shadowChangeCount;
    uint64_t structureVersion;  /* instances came or went */
    uint64_t materialVersion;
    uint64_t lodVersion;        /* meshes got lighter versions */
    ARTF_VEC(uint32_t) deforming;           /* meshes that poses change */
    ARTF_VEC(ArtfGraphData) graphs;
    void* models;                           /* gltf.c's, and how to free them: what does not load files carries none */
    void (*modelsFree)(void* models);
    uint32_t translucentCount;  /* instances that are layers: a scene with none pays nothing for them */
    uint64_t meshRemovals, textureVersion;
    ARTF_VEC(uint32_t) freeTextures;
    ARTF_VEC(uint32_t) freeMeshes;
    ArtfResidencyDesc residency;
    size_t residencyCeiling;    /* the GPU's memory ran out for textures: what they may hold at most (artfSceneResidencyPress) */
    int residencyWait;
    size_t decodedTextures;     /* those before this have pixels */
    uint32_t cutoutCount;       /* instances with ARTF_INSTANCE_CUTOUT, likewise */
    ArtfLightData lights[ARTF_MAX_LIGHTS];
    bool lightOn[ARTF_MAX_LIGHTS];
    ArtfVec3 toSun, sunColor, sky, ground, background, fogColor;
    ArtfShadowDesc shadows;
    bool lightCasts[ARTF_MAX_LIGHTS];
    float lightSize[ARTF_MAX_LIGHTS]; /* the radius of the ball of light; 0: a point */
    float lightShape[ARTF_MAX_LIGHTS][8]; /* along and half its length, facing and half its width (artfLightSetShape) */
    float* lightProfile[ARTF_MAX_LIGHTS]; /* ARTF_PROFILE_FLOATS (artfLightSetProfile), or NULL */
    uint64_t profileVersion;              /* a profile changed */
    float backgroundAlpha, exposure, fogDensity, sunSize;
    float fogBase, fogFalloff; /* fog by height */
    float aerial[4][4];        /* ArtfFrameData.aerial's, from artfSceneSetSky (0: none) */
    float cullSize;            /* artfSceneSetCullSize's pixels */
    ArtfCamera camera;
    bool features[ARTF_FEATURE_COUNT];
    float lodError;
    ArtfBackground backgroundPass;
    ArtfTrace trace;
    ArtfPostDesc post;
    ArtfDecalDesc decals[ARTF_MAX_DECALS];
    ARTF_VEC(uint8_t) textureRoles;         /* artfSceneTextureRoles' scratch: of each texture, how it is used */
    bool decalOn[ARTF_MAX_DECALS];
    void* emitters;                         /* extras.c's, and how to free them */
    void (*emittersFree)(void* emitters);
    ArtfMesh particleQuad;
    ArtfTexture particlePuff;
    ArtfCamera particleCamera;              /* what particles last turned to face */
    bool particlesMoved;                    /* stepped since */
    ArtfIndirect indirect;
    float* environment;         /* ARTF_ENV_FLOATS, or NULL: sky and ground */
    float environmentLight;     /* what it is multiplied by */
    bool environmentShown;      /* behind the scene */
    uint64_t environmentVersion;
    float* lut;                 /* ArtfPostDesc.look.lut's copy, lutSize^3 RGB, or NULL */
    size_t lutFloats;
    uint64_t lutVersion;        /* a new one */
    struct ArtfScale* scale;    /* scale.c's: merged meshes, levels of what bends, meshes' residency; NULL: none of it */
};

/* ---- scale.c ---- */

/* What a frame decides before any backend draws, the same for all: which cubes show their merged mesh, which level
 * each instance of what bends takes, which levels of meshes are held. Costs nothing to a scene that uses none. */
void artfSceneScaleStep(ArtfScene* s, int width, int height);
void artfScaleFree(struct ArtfScale* scale);
/* Lighter versions of a mesh that bends, made when it is added; and an instance's copies of them, after its copy of the
 * whole (home). */
void artfSceneSkinLevels(ArtfScene* s, uint32_t mesh);
void artfSceneCopySkinLevels(ArtfScene* s, uint32_t source, uint32_t home);
/* The trees hold whole meshes, not the level drawn: each instance drawing a lighter level of what bends has its copy of
 * the whole posed as that level is, when that changed, for rays to meet. Before poses are applied. */
void artfSceneSkinWholesPosed(ArtfScene* s);
/* The mesh rays meet for an instance's mesh: of what bends, its copy of the whole, whatever level it draws. */
ARTF_INLINE uint32_t artfTracedMesh(const ArtfScene* s, uint32_t mesh) { return s->meshes.items[mesh].home ? s->meshes.items[mesh].home - 1 : mesh; }
/* scene.c's, for scale.c: lighter versions of a mesh (touching no scene); a place for a mesh; an instance by handle,
 * NULL if there is none, said to no one; and an instance that changed, for what keeps copies. */
uint32_t artfSceneMakeLevels(const ArtfMeshData* m, ArtfMeshData* made, float* errors);
uint32_t artfSceneMeshPlace(ArtfScene* s, const ArtfMeshData* m);
ArtfInstanceData* artfSceneInstance(ArtfScene* s, ArtfInstance i);
void artfSceneInstanceChanged(ArtfScene* s, ArtfInstanceData* d);

/* A mesh's handle: its place + 1 in the low 24 bits and its place's generation in the top 8, so that the handle of
 * a mesh removed does not stand for the one put in its place after it (for 255 places over). */
ArtfMesh artfMeshHandleOf(const ArtfScene* s, uint32_t index);
/* A texture's handle, as meshes': its place (an index into the scene's textures, which materials, graphs, decals and the
 * GPU's array hold) in the low 24 bits, and how many have been in that place in the top 8, so that the handle of one
 * removed does not stand for the one that takes its place. A place never taken again has its index for a handle. */
ArtfTexture artfTextureHandleOf(const ArtfScene* s, uint32_t index);
/* The place of a handle: 0 for 0, and for one that is not there (removed, or of a texture that was in its place
 * before), said once in the log by `where`. */
uint32_t artfTextureIndexOf(const ArtfScene* s, ArtfTexture handle, const char* where);
/* A GPU's memory ran out for a texture: with automatic residency, the textures may hold a quarter less than they do
 * now (and less again, each time), and residency looks again at once, the least needed giving up levels first. */
void artfSceneResidencyPress(ArtfScene* s);
/* Its place, or UINT32_MAX when the handle stands for nothing: 0, never given, or of a mesh removed. */
uint32_t artfMeshIndexOf(const ArtfScene* s, ArtfMesh mesh);

/* The mesh drawn in place of this one when its own triangles are not held: its first level that is. */
ARTF_INLINE uint32_t artfMeshHeld(const ArtfScene* s, uint32_t mesh) {
    const ArtfMeshData* m = &s->meshes.items[mesh];
    return m->residentFrom ? m->lods[m->residentFrom - 1] : mesh;
}

/* ---- emitters of many (particles.c) ----
 * A particle is twelve floats, three vec4s, on both backends: where it is and its age (below 0: none); how fast it
 * goes and its angle; how fast it turns, and its birth's number (as bits). A step of an emitter is ARTF_SWARM_ROWS
 * vec4s, as the GPU reads them (particles.c says which). */
#define ARTF_SWARM_FLOATS 12
#define ARTF_SWARM_ROWS 8
#define ARTF_SWARM_STEPS 120 /* steps kept for what draws: one that missed more starts its particles over */
typedef struct ArtfSwarm ArtfSwarm;
ArtfSwarm* artfSwarmNew(ArtfScene* s, const ArtfEmitterDesc* desc, uint32_t index, uint32_t material, uint32_t quad);
void artfSwarmFree(ArtfSwarm* w);
void artfSwarmSet(ArtfSwarm* w, const ArtfEmitterDesc* desc);
void artfSwarmStep(ArtfScene* s, ArtfSwarm* w, float seconds, uint32_t burst);
uint32_t artfSwarmAlive(const ArtfSwarm* w);
/* The emitters of many in a scene, in order: how many, and each. */
uint32_t artfSceneSwarms(const ArtfScene* s, ArtfSwarm** out, uint32_t room);
/* What a backend reads of one: how many places; how many steps it made, and the oldest still kept (one that has seen
 * less than that starts over, empty, from there), each as rows; its first material (of ARTF_SWARM_SHADES, from its
 * first color to its last); and its rows for facing the eye as it is now. */
#define ARTF_SWARM_SHADES 8
uint32_t artfSwarmPlaces(const ArtfSwarm* w);
uint64_t artfSwarmStepCount(const ArtfSwarm* w);
uint64_t artfSwarmOldest(const ArtfSwarm* w);
const float* artfSwarmStepRows(const ArtfSwarm* w, uint64_t step); /* ARTF_SWARM_ROWS * 4 floats */
uint32_t artfSwarmMaterial(const ArtfSwarm* w);
void artfSwarmFaceRows(const ArtfScene* s, const ArtfSwarm* w, float* rows); /* ARTF_SWARM_ROWS * 4 floats */
/* The CPU's own copy of the particles, brought up to date on the pool's workers; and an instance of each place, facing
 * the eye as those rows say (a zero transform where there is none). Both as the GPU does it. */
typedef struct ArtfPool ArtfPool;
const float* artfSwarmCpu(ArtfSwarm* w, ArtfPool* pool);
void artfSwarmFace(const float* particles, const float* face, uint32_t first, uint32_t end, ArtfInstanceData* out);
/* One step of a particle, as the GPU's shader does it. */
void artfSwarmMove(float* particle, const float* step, uint32_t place);

/* ---- light from all around, from a picture (environment.c) ----
 * Maps of an octahedron around +y, folded below, each with a border of one texel: five of light as surfaces of
 * roughness 0, .25, .5, .75 and 1 reflect it, then what a surface facing a way takes from all around (over pi). Three
 * floats a texel. The GPU reads the same floats, and its shaders do the same arithmetic. */
#define ARTF_ENV_LEVELS 6
#define ARTF_ENV_DIFFUSE 5
/* Past the maps, what aims at the first of them by the light each of its texels gives, times the share of all around
 * it covers (environment.c): for each of its 512 rows, 513 sums of its texels from 0 to 1; then 513 of the rows. All
 * 0 when the picture gives no light. */
#define ARTF_ENV_AIM_START (3 * (514 * 514 + 66 * 66 + 34 * 34 + 3 * 18 * 18))
#define ARTF_ENV_FLOATS (ARTF_ENV_AIM_START + 513 * 513)
ARTF_INLINE uint32_t artfEnvSize(int level) { return level == 0 ? 512u : level == 1 ? 64u : level == 2 ? 32u : 16u; }
ARTF_INLINE uint32_t artfEnvStart(int level) {
    uint32_t at = 0;
    for (int i = 0; i < level; ++i) at += 3 * (artfEnvSize(i) + 2) * (artfEnvSize(i) + 2);
    return at;
}

/* Where a direction falls on a map, from 0 to 1. */
ARTF_INLINE void artfEnvPlace(float x, float y, float z, float* u, float* v) {
    const float sum = artfMaxf(fabsf(x) + fabsf(y) + fabsf(z), 1e-20f);
    float a = x / sum, b = z / sum;
    if (y < 0) {
        const float folded = (1.f - fabsf(b)) * (a >= 0 ? 1.f : -1.f);
        b = (1.f - fabsf(a)) * (b >= 0 ? 1.f : -1.f);
        a = folded;
    }
    *u = a * .5f + .5f;
    *v = b * .5f + .5f;
}

/* What a map holds toward a direction, between its four nearest texels. */
ARTF_INLINE void artfEnvSample(const float* env, int level, float x, float y, float z, float* out) {
    const uint32_t n = artfEnvSize(level), w = n + 2;
    const float* m = env + artfEnvStart(level);
    float u, v;
    artfEnvPlace(x, y, z, &u, &v);
    const float tx = artfClampf(u * (float)n + .5f, 0, (float)n), ty = artfClampf(v * (float)n + .5f, 0, (float)n);
    const float cx = floorf(tx), cy = floorf(ty), fx = tx - cx, fy = ty - cy;
    const uint32_t ix = (uint32_t)cx, iy = (uint32_t)cy;
    const float* p00 = m + 3 * (iy * w + ix);
    const float* p10 = p00 + 3;
    const float* p01 = p00 + 3 * w;
    const float* p11 = p01 + 3;
    for (int k = 0; k < 3; ++k) {
        const float top = p00[k] + (p10[k] - p00[k]) * fx, bottom = p01[k] + (p11[k] - p01[k]) * fx;
        out[k] = top + (bottom - top) * fy;
    }
}

/* The direction at a place on a map (0 to 1): artfEnvPlace's way back, unfolded below. */
ARTF_INLINE void artfEnvDirection(float u, float v, float* d) {
    float a = u * 2.f - 1.f, b = v * 2.f - 1.f;
    const float sum = fabsf(a) + fabsf(b);
    float y = 1.f - sum;
    if (sum > 1.f) {
        const float unfolded = (1.f - fabsf(b)) * (a >= 0 ? 1.f : -1.f);
        b = (1.f - fabsf(a)) * (b >= 0 ? 1.f : -1.f);
        a = unfolded;
        y = 1.f - sum;
    }
    const float inverse = 1.f / sqrtf(a * a + y * y + b * b);
    d[0] = a * inverse;
    d[1] = y * inverse;
    d[2] = b * inverse;
}

/* Of sums from 0 to 1 (513 of them), the one of 512 a number falls in: in nine halvings, as the shader's. */
ARTF_INLINE uint32_t artfEnvFind(const float* sums, float u) {
    uint32_t at = 0;
    for (uint32_t step = 256; step; step >>= 1)
        if (sums[at + step] <= u) at += step;
    return at;
}

/* How likely a direction is to be aimed at (artfEnvAim), over directions: a texel's share, over the share of all
 * around it covers. On a map of an octahedron that is (512^2 / 4) / (|x| + |y| + |z|)^3 of a unit direction. */
ARTF_INLINE float artfEnvAimShare(float rowShare, float columnShare, const float* d) {
    const float l1 = fabsf(d[0]) + fabsf(d[1]) + fabsf(d[2]);
    return rowShare * columnShare * 65536.f / (l1 * l1 * l1);
}

/* A direction toward the picture of all around, chosen as much as it gives light, from two numbers in [0, 1); how
 * likely it was (artfEnvAimDensity's). 0: the picture gives none. */
ARTF_INLINE float artfEnvAim(const float* env, float u0, float u1, float* d) {
    const float* aim = env + ARTF_ENV_AIM_START;
    const float* rows = aim + 512 * 513;
    if (!(rows[512] > 0)) return 0.f;
    const uint32_t row = artfEnvFind(rows, u0);
    const float* columns = aim + row * 513;
    const uint32_t column = artfEnvFind(columns, u1);
    const float rowShare = rows[row + 1] - rows[row], columnShare = columns[column + 1] - columns[column];
    /* (where in its texel: what is left of each number, as a share of its texel's) */
    const float across = (u1 - columns[column]) / columnShare, down = (u0 - rows[row]) / rowShare;
    artfEnvDirection(((float)column + across) * (1.f / 512.f), ((float)row + down) * (1.f / 512.f), d);
    return artfEnvAimShare(rowShare, columnShare, d);
}

/* How likely artfEnvAim is to choose this direction (unit), over directions. */
ARTF_INLINE float artfEnvAimDensity(const float* env, const float* d) {
    const float* aim = env + ARTF_ENV_AIM_START;
    const float* rows = aim + 512 * 513;
    if (!(rows[512] > 0)) return 0.f;
    float u, v;
    artfEnvPlace(d[0], d[1], d[2], &u, &v);
    const uint32_t column = (uint32_t)artfMinf(u * 512.f, 511.f), row = (uint32_t)artfMinf(v * 512.f, 511.f);
    const float* columns = aim + row * 513;
    return artfEnvAimShare(rows[row + 1] - rows[row], columns[column + 1] - columns[column], d);
}

/* Light reflected toward a direction by a surface this rough: between the two nearest maps. */
ARTF_INLINE void artfEnvReflected(const float* env, float rough, float x, float y, float z, float* out) {
    const float at = artfClampf(rough, 0, 1) * 4.f, first = artfMinf(floorf(at), 3.f), share = at - first;
    float a[3], b[3];
    artfEnvSample(env, (int)first, x, y, z, a);
    artfEnvSample(env, (int)first + 1, x, y, z, b);
    for (int k = 0; k < 3; ++k) out[k] = a[k] + (b[k] - a[k]) * share;
}

/* Fills in what a frame of that size needs. */
void artfSceneFrame(const ArtfScene* s, int width, int height, ArtfFrameData* out);

/* Fills in this frame's shadows; how many views it has: 0 when there are none. */
uint32_t artfSceneShadowFrame(const ArtfScene* s, const ArtfFrameData* frame, ArtfShadowFrame* out);

/* texture.c. A block's size in bytes, and in texels; turning its format into pixels, for the
 * CPU and for what a GPU does not take; for all of a scene's; and blocks from pixels, BC or ETC2. */
uint32_t artfTextureBlockBytes(uint32_t format);
void artfTextureBlockSize(uint32_t format, int* width, int* height);
void artfDecodeBc7(const uint8_t* block, uint32_t* out);
void artfDecodeAstc(const uint8_t* block, int width, int height, uint32_t* out);
void artfTextureDecode(ArtfTextureData* t);
void artfSceneDecodeTextures(ArtfScene* s);
uint8_t* artfTextureEncode(const ArtfTextureData* t, bool etc, uint32_t* format, size_t* size, uint32_t* offsets);

/* From a to b, of length 1, at a steady speed. */
void artfQuatSlerp(const float* a, const float* b, float t, float* out);

/* Looks at what textures need, as often as was asked. */
void artfSceneResidencyStep(ArtfScene* s, const ArtfFrameData* frame, int height);

ArtfTexture artfSceneStoreTexture(ArtfScene* s, const ArtfTextureData* t);

/* What a material of the program's own says at a place. */
void artfGraphRun(const ArtfScene* s, const ArtfGraphData* g, const ArtfMaterialData* m, const ArtfGraphInput* in, ArtfGraphResult* out);

/* A texel, bilinear and repeating, in the largest mipmap: channels from 0 to 1, as BGRA. */
/* A texture's mipmaps, the largest first, as means of light (sRGB's, as colors are) or of bytes (as data are): what
 * artfPrepareTexture makes with light false. Any thread. */
ArtfPreparedTexture* artfPrepareTextureAs(const uint32_t* pixels, int width, int height, int stride, bool light);
/* What each texture is to what uses it, a color or data, and its mipmaps made again where that changed: a texture that
 * only colors use (a material's color or sheen, a material of the program's own, a decal) is a color, one that anything
 * reads as data (normals, roughness and metal, occlusion, a coat's maps) is data, even when colors use it too (said once).
 * artfRendererDraw calls it before a frame. */
void artfSceneTextureRoles(ArtfScene* s);

/* ---- what takes long to make, kept in a file (cook.c) ---- */
typedef struct ArtfCookBytes { uint8_t* items; size_t len, cap; } ArtfCookBytes;
typedef struct ArtfCookReader { const uint8_t* data; size_t size, at; bool failed; } ArtfCookReader;
void artfCookPut(ArtfCookBytes* b, const void* data, size_t size);
void artfCookPut32(ArtfCookBytes* b, uint32_t v);
void artfCookPutFloat(ArtfCookBytes* b, float v);
bool artfCookGet(ArtfCookReader* r, void* out, size_t size);
uint32_t artfCookGet32(ArtfCookReader* r);
float artfCookGetFloat(ArtfCookReader* r);
/* A mesh as built (not what bends): its arrays, meshlets and positions included. Reading checks that what refers
 * into the arrays stays in them; on failure, nothing is held. */
void artfCookPutMesh(ArtfCookBytes* b, const ArtfMeshData* m);
bool artfCookGetMesh(ArtfCookReader* r, ArtfMeshData* m);
/* What is inside a prepared mesh or texture, and one made of such parts (the parts are its own then). */
void artfPreparedMeshParts(const ArtfPreparedMesh* p, const ArtfMeshData** mesh, const ArtfMeshData** levels, const float** errors, uint32_t* levelCount);
ArtfPreparedMesh* artfPreparedMeshOf(const ArtfMeshData* mesh, const ArtfMeshData* levels, const float* errors, uint32_t levelCount);
const ArtfTextureData* artfPreparedTextureData(const ArtfPreparedTexture* p);
ArtfPreparedTexture* artfPreparedTextureOf(const ArtfTextureData* t);
/* A texture as prepared (plain pixels and their mipmaps), and back; reading checks its sizes. */
void artfCookPutTexture(ArtfCookBytes* b, const ArtfTextureData* t);
bool artfCookGetTexture(ArtfCookReader* r, ArtfTextureData* t);
/* A hash of bytes (FNV-1a, 64 bits): what a cooked file says it was made from. */
uint64_t artfCookHash(const void* data, size_t size);
/* What the first count instances are, and where: what a file was made against. */
uint64_t artfCookInstancesPrint(const ArtfScene* s, uint32_t count);
/* A mesh as built, and its levels, into the scene: what artfSceneAddPreparedMesh does with a prepared one. */
ArtfMesh artfSceneAddBuiltMesh(ArtfScene* s, const ArtfMeshData* mesh, const ArtfMeshData* levels, const float* errors, uint32_t levelCount);
void artfTexelAt(const ArtfTextureData* t, float u, float v, float* out);
void artfTexelAtLevel(const ArtfTextureData* t, int level, float u, float v, float* out);
void artfGraphRunLanes(const ArtfScene* s, const ArtfGraphData* g, const ArtfMaterialData* m, ArtfGraphLanes* io);

/* Brings what rays read up to date with the scene. */
void artfSceneTraceSync(ArtfScene* s);
void artfTraceFini(ArtfTrace* t);
void artfSceneTraceFrame(const ArtfScene* s, const ArtfFrameData* frame, int width, int height, uint32_t done, ArtfTraceFrame* out);
uint64_t artfSceneTraceKey(const ArtfScene* s, const ArtfFrameData* frame);
/* Adds one sample of a pixel to out: the light of what was covered, and 1 in out[3] if it was. */
void artfTraceSample(const ArtfScene* s, const ArtfFrameData* f, const ArtfTraceFrame* c, uint32_t x, uint32_t y, uint32_t sample, float* out, ArtfTraceGuide* guide);

/* Rays while drawing (ArtfShadowDesc.traced, ArtfPostDesc.screen.tracedReflections): ArtfFrameData.traced holds these. The
 * trees are tracing's, kept up to date by artfSceneTraceSync, without particles. */
enum { ARTF_TRACED_SHADOWS = 1, ARTF_TRACED_REFLECTIONS = 2 };
#define ARTF_TRACED_SUN (1u << 16) /* in tracedCasts: the sun casts; bits 0 to 15, the frame's lights */
#define ARTF_TRACED_RAYS 4         /* to a light with a size, from each pixel */
/* Points in a ball, four by four: each four spread through it. A pixel's rays take the four of its place in a 2 x 2
 * square, which turns each frame with temporal antialiasing; so a square takes all sixteen. */
extern const float artfTracedBall[16][3];
/* The 2 x 2 place of where p falls in the frame, turned by the frame: 0 to 3. */
uint32_t artfTracedPlace(const ArtfFrameData* f, const float* p);
/* How much of a light reaches p, of normal n (toward the eye): light -1 the sun, else the frame's light. */
float artfTracedShadow(const ArtfScene* s, const ArtfFrameData* f, const float* p, const float* n, int light);
/* The same, for what an instance draws by a level whose error is drawn (artfDrawnError): its rays start off it by that. */
float artfTracedShadowDrawn(const ArtfScene* s, const ArtfFrameData* f, const float* p, const float* n, int light, float drawn);
/* The same toward a light at `at` of that size (NULL: the sun, of the frame's size): for lights past the frame's own. */
float artfTracedShadowAt(const ArtfScene* s, const ArtfFrameData* f, const float* p, const float* n, const float* at, float size, float drawn);
/* How far what an instance draws may be from its mesh, in the world: 0 for the whole mesh; a lighter level's error times
 * its scale; below 0 for a graph's cut (within distance / lodScale where it is seen). */
ARTF_INLINE float artfDrawnError(const ArtfScene* s, const ArtfFrameData* f, uint32_t base, uint32_t drawn, float maxScale) {
    const ArtfMeshData* m = &s->meshes.items[base];
    for (uint32_t k = 0; k < m->lodCount; ++k)
        if (m->lods[k] == drawn) return artfMeshLevelError(m, k) * maxScale;
    return drawn == base && m->dagBounds && f->lodScale > 0 && !artfOrtho(f) ? -1.f : 0.f;
}
/* What a ray from p (of normal n) along the mirror of the view v finds, strayed by rough through the ball's point
 * `point`, lit simply, into out; false when it finds nothing. */
bool artfTracedReflection(const ArtfScene* s, const ArtfFrameData* f, const float* p, const float* n, const float* v, float rough, uint32_t point, float* out);

/* A layer in view: an instance (dense index) that shows what is behind it, and the mesh it takes. */
typedef struct ArtfLayer {
    uint32_t instance, mesh;
    float distance, radius; /* (its center's along the view, and its ball's) */
} ArtfLayer;

/* The layers in view, the farthest first: both backends draw them in this order, after the rest. out:
 * room for s->translucentCount. */
uint32_t artfSceneLayers(const ArtfScene* s, const ArtfFrameData* frame, ArtfLayer* out);

/* glTF's EXT_meshopt_compression (meshopt.c): 0 when the bytes decoded, less when they did not. Filters: 1
 * octahedral, 2 quaternion, 3 exponential. */
int artfMeshoptDecodeVertices(uint8_t* out, size_t count, size_t stride, const uint8_t* data, size_t size);
int artfMeshoptDecodeTriangles(uint8_t* out, size_t count, size_t indexSize, const uint8_t* data, size_t size);
int artfMeshoptDecodeIndices(uint8_t* out, size_t count, size_t indexSize, const uint8_t* data, size_t size);
void artfMeshoptFilter(uint8_t* data, size_t count, size_t stride, int filter);

/* Fog that light goes through: a grid along the view, of cells ARTF_FROXEL_TILE pixels square, in slices that end at
 * far * ((k + 1) / ARTF_FROXEL_SLICES)^2 along it, far being 4 / density. Each cell holds the light the air adds from
 * the eye to the slice's end, and what it leaves of what is behind; both backends build it and read it alike. A
 * column's slices are one after another: cell (x, y, k) is at ((y * columns + x) * ARTF_FROXEL_SLICES + k). */
#define ARTF_FROXEL_TILE 16
#define ARTF_FROXEL_SLICES 64

/* A decal as both backends read it: 80 bytes, five vec4s. From the world into its box, by rows; its color and opacity;
 * its texture, roughness (0: the surface's), and the mipmap to read, from the largest and from what the GPU holds. */
typedef struct ArtfDecalData {
    float toBox[12];
    float color[3], opacity;
    uint32_t texture;
    float roughness, level, gpuLevel;
} ArtfDecalData;

/* The decals that may show in this frame, with their mipmaps for it: the count, into out (room for ARTF_MAX_DECALS). */
uint32_t artfSceneDecals(const ArtfScene* s, const ArtfFrameData* frame, int height, ArtfDecalData* out);

/* Turns particles to face the camera as it is, if it moved or they did since: what each backend does as it draws. */
void artfSceneFaceParticles(ArtfScene* s);

/* Makes an instance an emitter's particle (ARTF_INSTANCE_PARTICLE). */
void artfInstanceMarkParticle(ArtfScene* s, ArtfInstance i);

/* Fills in what a frame of that size needs of the scene's background pass, calling its frame; bloomPixels: what the
 * bloom's levels take. */
void artfBackgroundFrame(const ArtfScene* s, int width, int height, ArtfBackgroundBlock* out, uint32_t* bloomPixels);
/* Lets go of the pass, as the scene is freed. */
void artfBackgroundFini(ArtfScene* s);

/* Packs and orders a mesh (meshBuild.c); false when no triangle is left. skin: NULL, or one for each vertex. */
bool artfMeshBuild(ArtfMeshData* m, const ArtfVertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount, const ArtfVertexSkin* skin, const ArtfMorphTarget* targets, uint32_t targetCount);
/* A copy of all of a mesh but its skin, which is shared. */
void artfMeshCopy(ArtfMeshData* to, const ArtfMeshData* from);
/* Moves a skinned mesh's vertices to its pose. */
void artfMeshDeform(ArtfMeshData* m);
/* Makes room for a pose in the box its 16 bits stand for: true when the box changed. */
bool artfMeshPoseFit(ArtfMeshData* m);
/* Brings the vertices of all that bends to its pose, for what reads them on the CPU. */
void artfScenePosesApply(ArtfScene* s);
/* Its vertices back, as far as packing kept them: vertexCount of them. */
void artfMeshUnpack(const ArtfMeshData* m, ArtfVertex* out);
void artfMeshFini(ArtfMeshData* m);

ARTF_INLINE float artfHalfToFloat(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 31, man = h & 1023, bits;
    if (exp == 31) bits = sign | 0x7F800000 | man << 13;
    else if (exp) bits = sign | (exp + 112) << 23 | man << 13;
    else if (!man) bits = sign;
    else { /* a denormal */
        exp = 113;
        while (!(man & 1024)) { man <<= 1; --exp; }
        bits = sign | exp << 23 | (man & 1023) << 13;
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

/* The mesh a thing of this size should be drawn with, at this distance. */
ARTF_INLINE uint32_t artfMeshLevel(const ArtfScene* s, uint32_t mesh, float lodScale, float maxScale, float distance) {
    const ArtfMeshData* m = &s->meshes.items[mesh];
    uint32_t chosen = mesh;
    if (lodScale > 0)
        for (uint32_t i = 0; i < m->lodCount; ++i)
            if (artfMeshLevelError(m, i) * maxScale * lodScale <= distance) chosen = m->lods[i];
    return chosen;
}

/* Layers by each pixel's depth (ARTF_FEATURE_LAYERS_SORTED), by transmittance: blended by weight (McGuire and Bavoil,
 * 2013), each surface weighed by what the layers in front of it leave of it, from a grid along the view, as AVBOIT
 * (Drobot, 2025) has it. The grid's columns are ARTF_LAYER_TILE pixels square, and ARTF_LAYER_SLICES deep, by the log
 * of depth, from the nearest layer to the farthest. A surface adds what it takes (its optical depth, in bits) to its
 * cell; its weight is 2 to the minus the mean of what the cells in front of it took. A pixel's sample sums the colors
 * times their weights, the alphas times their weights, and the optical depths: the color is the first over the
 * second, its alpha all that the third takes. When what is in front of a surface is all of its column, that is the
 * same as layers over each other, the farthest first. All of it is in integers of ARTF_LAYER_ONE a unit, so that the
 * order the surfaces come in changes nothing, and the CPU's and the GPU's are the same. */
#define ARTF_LAYER_TILE 8
#define ARTF_LAYER_SLICES 64
#define ARTF_LAYER_ONE 4096.f
#define ARTF_LAYER_SUMS 5 /* of a sample: red, green, blue and alpha, times weights; optical depths */

/* What a surface of alpha a (of 255) takes of what is behind it: -log2(1 - a / 255), a unit ARTF_LAYER_ONE, at most
 * 254's. */
ARTF_INLINE uint32_t artfLayerTaken(uint32_t a) {
    const float left = (float)(255u - (a < 254u ? a : 254u)) * (1.f / 255.f);
    return (uint32_t)(-artfLog2Fast(left) * ARTF_LAYER_ONE + .5f);
}
/* The slice of the grid a surface is in, by 1 over how far along the view it is (its w's: a pixel's 1 / w): layers is
 * the frame's. (In perspective: orthographic views have one slice.) */
ARTF_INLINE uint32_t artfLayerSlice(const float* layers, float inverseW) {
    const float k = artfLog2Fast(layers[0] / inverseW) * layers[1];
    return k > 0 ? (uint32_t)artfMinf(k, (float)(ARTF_LAYER_SLICES - 1)) : 0u;
}
/* A surface's weight, of ARTF_LAYER_ONE, by what the cells of its column in front of it took (front), over the
 * column's samples (ARTF_LAYER_TILE^2 times a pixel's): at least 1. */
ARTF_INLINE uint32_t artfLayerWeight(uint32_t front, uint32_t samples) {
    const float mean = (float)front * (1.f / (ARTF_LAYER_ONE * (float)(samples * ARTF_LAYER_TILE * ARTF_LAYER_TILE)));
    const uint32_t w = (uint32_t)(artfExp2Fast(-mean) * ARTF_LAYER_ONE + .5f);
    return w ? w : 1u;
}
/* A sample's sums, as a color over what is behind (premultiplied, packed as a frame's: alpha << 24, red << 16). */
ARTF_INLINE uint32_t artfLayerResolve(const uint32_t* sums) {
    if (!sums[3]) return 0;
    const float cover = 1.f - artfExp2Fast(-(float)sums[4] * (1.f / ARTF_LAYER_ONE));
    const float scale = cover * 255.f / (float)sums[3];
    uint32_t out = (uint32_t)(cover * 255.f + .5f) << 24;
    for (int k = 0; k < 3; ++k) out |= (uint32_t)artfMinf((float)sums[k] * scale + .5f, 255.f) << (16 - 8 * k);
    return out;
}
/* The frame's layers (ArtfFrameData.layers), for these layers in view. */
void artfLayerRange(ArtfFrameData* frame, const ArtfLayer* layers, uint32_t count);

/* Where a mesh is on its way from one level to the next: 0 if it is not, or else how far, from 1 to 254, with other the
 * level it comes from. chosen is what artfMeshLevel gives. */
#define ARTF_LOD_FADE .15f
ARTF_INLINE uint32_t artfMeshFade(const ArtfScene* s, uint32_t mesh, float lodScale, float maxScale, float distance, float band, uint32_t* other) {
    const ArtfMeshData* m = &s->meshes.items[mesh];
    uint32_t chosen = mesh;
    float limit = 0;
    *other = mesh;
    if (lodScale > 0)
        for (uint32_t i = 0; i < m->lodCount; ++i) {
            const float at = artfMeshLevelError(m, i) * maxScale * lodScale;
            if (at > distance) continue;
            *other = chosen;
            chosen = m->lods[i];
            limit = at;
        }
    if (band <= 0 || limit <= 0 || distance >= limit * (1.f + band)) return 0;
    return (uint32_t)artfClampi((int)((distance - limit) / (limit * band) * 255.f), 1, 254);
}

/* The normal back from the octahedron, of no fixed length. */
ARTF_INLINE ArtfVec3 artfOctUnfold(int16_t ox, int16_t oy) {
    float x = artfMaxf((float)ox / 32767.f, -1.f), y = artfMaxf((float)oy / 32767.f, -1.f);
    float z = 1.f - fabsf(x) - fabsf(y);
    if (z < 0) {
        float fx = (1.f - fabsf(y)) * (x >= 0 ? 1.f : -1.f), fy = (1.f - fabsf(x)) * (y >= 0 ? 1.f : -1.f);
        x = fx;
        y = fy;
    }
    return artfVec3(x, y, z);
}

#endif
