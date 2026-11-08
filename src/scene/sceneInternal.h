/* artful — a scene's memory, as the backends read it. What a GPU reads is laid out here for it, so
 * that a backend copies it as it is. */
#ifndef ARTFUL_SCENE_INTERNAL_H
#define ARTFUL_SCENE_INTERNAL_H

#include "artful/scene.h"

#define ARTF_MESHLET_VERTICES 64
#define ARTF_MESHLET_TRIANGLES 124
#define ARTF_MAX_LODS 3
#define ARTF_SHADOW_VIEWS 16
#define ARTF_SHADOW_CASCADES 3
#define ARTF_SHADOW_ATLAS_COLUMNS 4
#define ARTF_HIZ_LEVELS 15
/* The disc's gas: rings of it, each a row, so that each can turn at the speed of its own orbit. */
#define ARTF_HOLE_MAP_WIDTH 2048
#define ARTF_HOLE_MAP_HEIGHT 1024
#define ARTF_HOLE_DUST_WIDTH 1024 /* where the gas gives less light than it stops: dark lanes */
#define ARTF_HOLE_DUST_HEIGHT 256
#define ARTF_FLUID_WIDTH 1024  /* the disc's gas, as a fluid: around each ring (a power of two) */
#define ARTF_FLUID_HEIGHT 256  /* and rings */
#define ARTF_HOLE_COLORS 256
#define ARTF_HOLE_SKY_WIDTH 1024
#define ARTF_HOLE_SKY_HEIGHT 512
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
    uint32_t generation;        /* a new one in this place */
    uint64_t deformVersion;     /* a pose changed its vertices */
    /* Its lighter versions, the lightest last: meshes of the scene like any other. */
    uint32_t lods[ARTF_MAX_LODS];
    float lodErrors[ARTF_MAX_LODS];
    uint32_t lodCount;
    bool isLod;
} ArtfMeshData;

typedef struct ArtfTextureData {
    int width, height, mipCount;
    uint32_t* pixels;      /* the mipmaps, the largest first; NULL until blocks are turned into them */
    uint32_t offsets[16];  /* of each mipmap, in pixels */
    uint32_t generation;   /* a new one in this place */
    int residentMip;       /* what is drawn from */
    bool removed;
    uint32_t format;       /* what Vulkan calls its blocks; 0: plain pixels */
    uint8_t* blocks;
    size_t blockBytes;
    uint32_t blockOffsets[16];
    ArtfTextureReadFn read; /* not NULL: pixels holds only what is resident, and offsets are from there */
    void* readUser;
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
    ARTF_INSTANCE_CUTOUT = 2   /* its material has holes, or a back: it cannot go with the plain ones */
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

enum { ARTF_MATERIAL_MASK = 1, ARTF_MATERIAL_BLEND = 2, ARTF_MATERIAL_DOUBLE = 4 };

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

/* Light that bounced (trace.c): a grid of probes, each four vec4s: the light from all around, then of red, green and
 * blue how it changes with the normal; what a surface facing n takes, over pi, is c0 + (r . n, g . n, b . n). */
typedef struct ArtfIndirect {
    bool on;
    ArtfIndirectDesc desc;
    int dims[3];
    float origin[3], spacing[3];
    float* probes;      /* dims product x 16 */
    float* next;        /* what this step makes */
    uint32_t frames;
    uint64_t version;   /* a step changed them */
} ArtfIndirect;
ARTF_INLINE uint32_t artfIndirectCount(const ArtfIndirect* g) { return (uint32_t)(g->dims[0] * g->dims[1] * g->dims[2]); }

/* Light from all around a surface at p facing n, from the probes near it, over pi. */
void artfIndirectAt(const ArtfIndirect* g, const float* p, const float* n, float* out);

/* The lights a frame holds itself: those past them are listed by the squares of the frame they reach. */
#define ARTF_FRAME_LIGHTS 16
#define ARTF_LIGHT_TILE 64       /* a square's side, in pixels: the CPU's tiles */
#define ARTF_TILE_LIGHTS 64      /* the most one square lists */

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
} ArtfFrameData;

/* A step of the probes: their rays, from what a frame says of light (needs artfSceneTraceSync). */
typedef struct ArtfPool ArtfPool;
void artfSceneIndirectStep(ArtfScene* s, const ArtfFrameData* frame, ArtfPool* pool);

/* Lights past the frame's own, in handle order: how many, into out (ARTF_MAX_LIGHTS of room). */
uint32_t artfSceneExtraLights(const ArtfScene* s, ArtfLightData* out);
/* Of each square of the frame (ARTF_LIGHT_TILE a side, by rows), which of those lights reach it: a count, then up to
 * ARTF_TILE_LIGHTS of their places in what artfSceneExtraLights gave. lists: squares x (1 + ARTF_TILE_LIGHTS). */
void artfSceneLightTiles(const ArtfFrameData* frame, int width, int height, const ArtfLightData* lights, uint32_t count, uint32_t* lists);

/* Moves the frame within a pixel, by the n-th of Halton's points, for temporal antialiasing: viewProj and post[2]. */
void artfFrameJitter(ArtfFrameData* frame, uint32_t n, int width, int height);

/* One square of the shadow atlas: a place seen from a light. Clip space: x and y from -1 to 1, depth from 0 to
 * 1. y grows with the map's rows. */
typedef struct ArtfShadowView {
    float m[16];     /* by rows */
    float info[4];   /* its corner in the atlas, in texels; how wide a texel is (per unit of distance, for a
                      * lamp); how much depth is forgiven */
    float place[4];  /* a lamp's position and reach; for the sun, where it is, and reach 0 */
} ArtfShadowView;

/* All the shadows of a frame: the GPU's uniform block. */
typedef struct ArtfShadowFrame {
    float splits[4];    /* where each of the sun's maps ends, from the eye */
    uint32_t sun[4];    /* its first view + 1 (0: it casts none), how many, a map's side */
    uint32_t lights[ARTF_FRAME_LIGHTS][4]; /* by the frame's lights: first view + 1, and views << 8; 0: none */
    ArtfShadowView views[ARTF_SHADOW_VIEWS];
} ArtfShadowFrame;

/* Says when a map would come out as it was: this view, and all that is not an instance, are the same. */
uint64_t artfShadowViewKey(const ArtfScene* s, const ArtfShadowView* view, uint32_t side);
/* Did an instance change, since seen changes had been made, where this view reaches? */
bool artfShadowViewTouched(const ArtfScene* s, const ArtfShadowView* view, uint32_t side, uint64_t seen);
#define ARTF_SHADOW_CHANGES 1024 /* more than this since a frame, and all maps are drawn */

/* What a frame needs to know of a black hole, as the GPU's uniform block. In units of its mass. */
typedef struct ArtfHoleFrame {
    /* where the eye is on the unit sphere, and the two directions along it: x, z, and the axis */
    float er[4];      /* w: its distance */
    float etheta[4], ephi[4];
    /* a pixel's ray, forward + x right + y up with x and y in -1..1: along r, theta and phi at the eye */
    float right[4], up[4], forward[4];
    float metric[4];  /* at the eye: the lapse, space's turning times the radius, that radius, and per sine */
    float place[4];   /* sqrt(Sigma), sqrt(Delta), 1 / r, cos(theta) */
    float spin[4];    /* spin, the 1 / r of the horizon, the disc's height, the shutter */
    float flow[4];    /* the moment the fluid's field is of */
    float boost[4];   /* the eye's speed along r, theta and phi; w: gamma */
    float disc[4];    /* inner, outer, time, thickness */
    float look[4];    /* gain, doppler, stars, bloom */
    float image[4];   /* exposure, 2 / width, 2 / height, 1 / rays a pixel */
    uint32_t size[4]; /* width, height, rays a side, bloom levels */
    uint32_t bloom[ARTF_BLOOM_LEVELS][4]; /* of each level: where it starts, its width and its height */
} ArtfHoleFrame;

/* What both backends read, in one block of floats: the gas's finest detail, below what the fluid
 * resolves; of each row its turns per unit of time and its heat; a black body's colors, by temperature;
 * the dust of the sky; of each row how much light its gas gives. */
#define ARTF_HOLE_ROWS_AT (ARTF_HOLE_MAP_WIDTH * ARTF_HOLE_MAP_HEIGHT)
#define ARTF_HOLE_COLORS_AT (ARTF_HOLE_ROWS_AT + ARTF_HOLE_MAP_HEIGHT * 2)
#define ARTF_HOLE_SKY_AT (ARTF_HOLE_COLORS_AT + ARTF_HOLE_COLORS * 3)
#define ARTF_HOLE_GLOW_AT (ARTF_HOLE_SKY_AT + ARTF_HOLE_SKY_WIDTH * ARTF_HOLE_SKY_HEIGHT)
#define ARTF_HOLE_DUST_AT (ARTF_HOLE_GLOW_AT + ARTF_HOLE_MAP_HEIGHT)
#define ARTF_HOLE_MAP_FLOATS (ARTF_HOLE_DUST_AT + ARTF_HOLE_DUST_WIDTH * ARTF_HOLE_DUST_HEIGHT)
/* How much the fluid says of the gas's density (the painted map says the rest: the fluid is coarser than
 * what shows, and it moves the gas); and dust, which gives this much less light and stops this much more:
 * dark lanes, against the gas behind. */
#define ARTF_HOLE_FLUID .1f
#define ARTF_HOLE_DUST_DIM .97f
#define ARTF_HOLE_DUST_DENSE 25.f
/* A heat of 1 is this, in kelvin: the middle of the table of colors, which goes up linearly from 0. */
#define ARTF_HOLE_KELVIN 6500.0

/* What rays read (trace.c), laid out for the GPU as well. A node with count 0 has its children at first
 * and first + 1; any other has that many triangles, or instances, from first on. */
#define ARTF_TRACE_STACK 48

typedef struct ArtfTraceNode {
    float lo[3];
    uint32_t first;
    float hi[3];
    uint32_t count;
} ArtfTraceNode;

typedef struct ArtfTraceVertex {
    float p[3], u;
    float n[3], v;
    uint32_t color;
    float back[3];      /* from where it is to where it was a pose ago */
} ArtfTraceVertex;

typedef struct ArtfTraceTriangle { uint32_t v[3], pad; } ArtfTraceTriangle;

typedef struct ArtfTraceInstance {
    float toObject[12]; /* the rows of its inverse */
    uint32_t root, material;
    float opacity;
    uint32_t twoSided;
} ArtfTraceInstance;

/* Where tracing last saw an instance, by its handle. (A handle used again looks as if it jumped: the filter sees that.) */
typedef struct ArtfTraceSeen { float at[12]; bool there; } ArtfTraceSeen;

/* What a frame's rays start from: the GPU's uniform block. */
typedef struct ArtfTraceFrame {
    float eye[4], right[4], up[4], forward[4]; /* a pixel's ray: forward + x right + y up, x and y in -1..1 */
    uint32_t width, height, done, samples;     /* done: what is gathered so far */
    uint32_t bounces, paths, pad[2];           /* pad[0], on the GPU: done, with this frame's samples; pad[1]: the first of a filter's frames */
    float previous[16];                        /* the frame before's view and projection, for the filter */
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
    ARTF_VEC(ArtfTraceNode) top;            /* over the instances */
    ARTF_VEC(ArtfTraceInstance) instances;
    ARTF_VEC(float) motion;                 /* of each of those, twelve floats: from where a point of it is to where it was */
    ARTF_VEC(ArtfTraceSeen) seen;
    uint64_t motionVersion;
    bool motionLive;                        /* something moved: next frame, if nothing does, it stands still again */
    uint64_t instanceVersion, structureVersion, materialVersion;
    uint64_t meshVersion, topVersion;       /* for backends that keep a copy */
    uint64_t meshRemovals;
} ArtfTrace;

typedef struct ArtfFluid ArtfFluid;

typedef struct ArtfHole {
    bool on;
    ArtfBlackHoleDesc desc;
    float* map;
    uint64_t mapVersion;
    ArtfFluid* fluid;
    uint64_t fluidVersion; /* what the field holds: each step changes it */
} ArtfHole;

struct ArtfScene {
    uint64_t id; /* no two scenes of a program share one */
    ARTF_VEC(ArtfMeshData) meshes;
    ARTF_VEC(ArtfTextureData) textures;     /* 0 is white */
    ARTF_VEC(ArtfMaterialData) materials;   /* 0 is grey */
    ARTF_VEC(ArtfInstanceData) instances;   /* dense, in no order */
    ARTF_VEC(uint32_t) instanceHandles;     /* of each dense instance */
    ARTF_VEC(uint32_t) handleSlots;         /* of each handle's number - 1: dense index, or the next free number */
    ARTF_VEC(uint8_t) handleGenerations;    /* the top byte of a handle: it changes when one is removed */
    uint32_t freeHandle;                    /* the free list's head; 0: none */
    /* What changed, for backends that keep a copy. */
    uint64_t instanceVersion;   /* a transform or a material */
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
    int residencyWait;
    size_t decodedTextures;     /* those before this have pixels */
    uint32_t cutoutCount;       /* instances with ARTF_INSTANCE_CUTOUT, likewise */
    ArtfLightData lights[ARTF_MAX_LIGHTS];
    bool lightOn[ARTF_MAX_LIGHTS];
    ArtfVec3 toSun, sunColor, sky, ground, background, fogColor;
    ArtfShadowDesc shadows;
    bool lightCasts[ARTF_MAX_LIGHTS];
    float backgroundAlpha, exposure, fogDensity, sunSize;
    ArtfCamera camera;
    bool features[ARTF_FEATURE_COUNT];
    float lodError;
    ArtfHole hole;
    ArtfTrace trace;
    ArtfPostDesc post;
    ArtfIndirect indirect;
    float* environment;         /* ARTF_ENV_FLOATS, or NULL: sky and ground */
    float environmentLight;     /* what it is multiplied by */
    bool environmentShown;      /* behind the scene */
    uint64_t environmentVersion;
};

/* ---- light from all around, from a picture (environment.c) ----
 * Maps of an octahedron around +y, folded below, each with a border of one texel: five of light as surfaces of
 * roughness 0, .25, .5, .75 and 1 reflect it, then what a surface facing a way takes from all around (over pi). Three
 * floats a texel. The GPU reads the same floats, and its shaders do the same arithmetic. */
#define ARTF_ENV_LEVELS 6
#define ARTF_ENV_DIFFUSE 5
#define ARTF_ENV_FLOATS (3 * (514 * 514 + 66 * 66 + 34 * 34 + 3 * 18 * 18))
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

/* A layer in view: an instance (dense index) that shows what is behind it, and the mesh it takes. */
typedef struct ArtfLayer {
    uint32_t instance, mesh;
    float distance;
} ArtfLayer;

/* The layers in view, the farthest first: both backends draw them in this order, after the rest. out:
 * room for s->translucentCount. */
uint32_t artfSceneLayers(const ArtfScene* s, const ArtfFrameData* frame, ArtfLayer* out);

/* The disc's gas as a fluid (fluid.c). New: warmed up, at that moment. Advance: to that moment, in fixed
 * steps; true when the field changed. The field: ARTF_FLUID_WIDTH * ARTF_FLUID_HEIGHT floats, rows even in
 * 1 / r as the map's are, of what shows of the gas; what moment it is of. */
ArtfFluid* artfFluidNew(float inner, float outer, float spin, double time);
void artfFluidFree(ArtfFluid* f);
bool artfFluidAdvance(ArtfFluid* f, double time);
const float* artfFluidField(const ArtfFluid* f);
double artfFluidTime(const ArtfFluid* f);

/* Where the last stable orbit is around a hole of this spin, in masses. */
float artfHoleIsco(float spin);
/* Fills in what a frame of that size needs of the black hole; bloomPixels: what its levels take. */
void artfHoleFrame(const ArtfScene* s, int width, int height, ArtfHoleFrame* out, uint32_t* bloomPixels);

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
            if (m->lodErrors[i] * maxScale * lodScale <= distance) chosen = m->lods[i];
    return chosen;
}

#define ARTF_LAYER_FRAGMENTS 8 /* of a pixel, when layers are sorted */

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
            const float at = m->lodErrors[i] * maxScale * lodScale;
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
