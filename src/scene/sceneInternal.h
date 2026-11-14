/* artful — a scene's memory, as the backends read it. What a GPU reads is laid out here for it, so
 * that a backend copies it as it is. */
#ifndef ARTFUL_SCENE_INTERNAL_H
#define ARTFUL_SCENE_INTERNAL_H

#include "artful/scene.h"
#include <string.h>

#define ARTF_MESHLET_VERTICES 64
#define ARTF_MESHLET_TRIANGLES 124
#define ARTF_MAX_LODS 3
#define ARTF_SHADOW_BIG_VIEWS 16  /* the sun's and the frame's lights': a map's side each */
#define ARTF_SHADOW_VIEWS 64      /* and, below them, smaller ones for lights past the frame's own */
#define ARTF_SHADOW_EXTRA 32      /* lights past the frame's own that can have shadows at once */
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
    /* What bends has levels apart (scale.c), each with a skin of its own: on the mesh given, those meshes; on an
     * instance's copy of it, the copies of them that instance takes in turn. home: on each such copy, the copy of the
     * whole + 1, which holds the list. The backends never see these: an instance's mesh is the level it takes. */
    uint32_t skinLods[ARTF_MAX_LODS];
    float skinLodErrors[ARTF_MAX_LODS];
    uint32_t skinLodCount, home;
    /* Residency (scale.c): of this mesh's levels (0 the whole, k its lods[k - 1]), the first that is held: those before
     * it are drawn as that one. evicted: this mesh's own triangles are not on the GPU, nor, if it can be read again,
     * in memory. */
    uint32_t residentFrom;
    bool evicted;
    bool streamed;  /* it can be read again (artfSceneAddMeshStreamed) */
} ArtfMeshData;

/* The error of a mesh's level i, as both backends choose by it: 0, always taken, where the levels before it are not
 * held. */
ARTF_INLINE float artfMeshLevelError(const ArtfMeshData* m, uint32_t i) { return i < m->residentFrom ? 0.f : m->lodErrors[i]; }

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
    float subsurface[3], pad1;
    float absorb[3], pad2;              /* traced, through its inside: kept is 2^-(absorb x distance) */
} ArtfMaterialMore;
#define ARTF_MATERIAL_MORE_ROWS 8
enum {
    ARTF_MORE_COAT = 1, ARTF_MORE_SHEEN = 2, ARTF_MORE_SPECULAR = 4, ARTF_MORE_ANISOTROPY = 8, ARTF_MORE_IRIDESCENCE = 16,
    ARTF_MORE_SUBSURFACE = 32, ARTF_MORE_MAPS = 64 /* the coat's or sheen's textures */, ARTF_MORE_VOLUME = 128,
    ARTF_MORE_DRAWN = 127 /* what drawing reads: all but the volume, which is traced */
};
/* It needs where u and v go: a coat's normal map, or highlights drawn out. */
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

/* Light that bounced (trace.c): a grid of probes. First, of each, four vec4s: the light from all around, then of red,
 * green and blue how it changes with the normal (what a surface facing n takes, over pi, is c0 + (r . n, g . n,
 * b . n)); after all of those, of each, how far it sees, in a map of ARTF_PROBE_SIDE x ARTF_PROBE_SIDE directions
 * folded onto an octahedron around +z, as artfProbeFold: the mean and the mean of the square of each, two floats a
 * texel, by rows. (Apart, so that what every pixel reads of eight probes stays together.) A point the probe cannot see
 * is told by them (Chebyshev, as Majercik et al.'s probes do), so that light does not go through walls. */
#define ARTF_PROBE_SIDE 8
#define ARTF_PROBE_MAP (ARTF_PROBE_SIDE * ARTF_PROBE_SIDE * 2)
#define ARTF_PROBE_FLOATS (16 + ARTF_PROBE_MAP) /* all of a probe's, in both places */

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
    float* probes;      /* dims product x ARTF_PROBE_FLOATS */
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
    /* The look (ArtfPostDesc): the tone map, auto exposure (1 or 0), log2 of its key, its speed; log2 of its least and
     * most, saturation, contrast; temperature, tint, vignette, chromatic. */
    float grade[3][4];
    /* How many materials there are: on the GPU, where their ArtfMaterialMore begin, in rows. Fog by height: where it
     * is as dense as artfSceneSetFog says, and how fast it thins above that (0: it does not). */
    uint32_t materialCount;
    float fogBase, fogFalloff, motionBlur; /* motionBlur: ArtfPostDesc's, the share of a frame */
    float lightSizes[ARTF_FRAME_LIGHTS]; /* of each light the frame holds, the radius of its ball (artfLightSetSize) */
    /* What is too small is not drawn (artfSceneSetCullSize): a ball of radius r at distance d is, when r * cullScale < d.
     * 0: all of it is. */
    float cullScale;
    float moving;       /* 1: something moved since the frame before, and the GPU has how (ArtfMotion); 0: the view alone */
    float view;         /* what the frame shows (ArtfView): 0, the picture */
    float cullPad;
    /* More of the look (ArtfPostDesc): lift, and 1 when there is grading (lift, gamma, gain or a wheel); 1 / gamma, the
     * grain; gain, the frame's number (for the grain); the shadows', midtones' and highlights' colors, after them the
     * look-up table's side (0: none), how much of it, and the flare; the histogram's darkest and brightest shares
     * left out, the speed down, and 1 when the histogram is on. */
    float grade2[7][4];
    /* An orthographic camera: how deep its depth goes (far - near), and where it ends, along the view; 0: perspective.
     * Its depth is (far - t) / (far - near) of t along the view, where a perspective's is near / t: 1 at near, 0 at far,
     * even across a triangle as w is 1. The eye's rows (eye) then hold the view's half width and half height, in the
     * world, and all rays go along eye[2], each from cameraPos + x eye[0] + y eye[1]. */
    float orthoPad[2], ortho[2];
    /* Rays while drawing: ARTF_TRACED_SHADOWS and REFLECTIONS; which lights cast (ARTF_TRACED_SUN and bits 0 to 15);
     * how far the pattern of a pixel's rays turns this frame; rays to a light with a size. */
    uint32_t traced, tracedCasts, tracedFrame, tracedRays;
    /* Of each light the frame holds, four rows: a shape's way along and half its length (0: none); which way a
     * rectangle faces, and half its width (0: a tube); where its profile's vertical angles start, and its handle's number
     * + 1 if it has one (0: none); where horizontal ones start. */
    float lightShapes[ARTF_FRAME_LIGHTS][4][4];
} ArtfFrameData;

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

/* A ball of this radius, this far from the eye, is too small to draw (ArtfFrameData.cullScale). */
ARTF_INLINE bool artfTooSmall(const ArtfFrameData* f, float radius, float distance) { return f->cullScale > 0 && radius * f->cullScale < artfSeenDistance(f, distance); }
/* How wide a light the frame holds is, for soft shadows: its ball and the most of its shape. */
ARTF_INLINE float artfLightSoftSize(const ArtfFrameData* f, uint32_t i) { return f->lightSizes[i] + artfMaxf(f->lightShapes[i][0][3], f->lightShapes[i][1][3]); }
/* Of the lights a frame of the scene holds, one has a shape or a profile (lights.c). */
bool artfSceneShapedLights(const ArtfScene* s);
/* It has a shape or a profile. */
ARTF_INLINE bool artfLightShaped(const ArtfFrameData* f, uint32_t i) { return f->lightShapes[i][0][3] > 0 || f->lightShapes[i][2][3] > 0; }


/* log2 and 2^x by bits and polynomials, the same on every processor and in the shaders: for metering, where a few
 * millionths do not matter. log2 of x > 0; 2^x for x within +-100. */
ARTF_INLINE float artfLog2Fast(float x) {
    uint32_t bits;
    memcpy(&bits, &x, 4);
    const float e = (float)(int32_t)((bits >> 23) & 255) - 127.f;
    bits = (bits & 0x7FFFFFu) | 0x3F800000u;
    float m;
    memcpy(&m, &bits, 4);
    const float z = (m - 1.f) / (m + 1.f), z2 = z * z; /* log2 m = 2 / ln 2 atanh z */
    return e + z * 2.8853901f * (1.f + z2 * (.33333333f + z2 * (.2f + z2 * .14285714f)));
}
ARTF_INLINE float artfExp2Fast(float x) {
    x = x < -100.f ? -100.f : x > 100.f ? 100.f : x;
    const int32_t i = (int32_t)(x + 128.f) - 128; /* floor */
    const float u = (x - (float)i) * .69314718f;
    const float f = 1.f + u * (1.f + u * (.5f + u * (.16666667f + u * (.041666668f + u * (.0083333333f + u * .0013888889f)))));
    const uint32_t bits = (uint32_t)(i + 127) << 23;
    float scale;
    memcpy(&scale, &bits, 4);
    return f * scale;
}
/* What fog by height makes of the fog's density at a height: as much as that, times this. */
ARTF_INLINE float artfFogHeight(const ArtfFrameData* f, float y) {
    if (!(f->fogFalloff > 0)) return 1.f;
    return artfMinf(artfExp2Fast((f->fogBase - y) * f->fogFalloff * 1.44269504f), 16.f);
}
/* How each instance moved since the last step (motion.c): of each, twelve floats, the rows of what takes a point
 * of the world from where it is to where it was. A renderer keeps its own. */
typedef struct ArtfMotion {
    float* at;          /* by handle: where each was at the last step */
    uint8_t* there;     /* by handle: it was */
    size_t handles;     /* room for so many */
    float* motion;      /* by instance */
    size_t count, capacity;
    uint64_t scene, version; /* version: the motion changed */
    bool moving;
} ArtfMotion;
void artfMotionStep(ArtfMotion* m, const ArtfScene* s);
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
 * shape: along and half its length; facing and half its width (0: a tube). ball: a tube's radius. Out: toward its
 * point nearest p, which its diffuse light comes from, and that light's share (a rectangle's light goes one way, and
 * less to the side); toward its point nearest the mirror ray, which its highlight comes from, and what that keeps as it
 * is spread over as long and as wide as the shape looks (Karis, 2013); the square of the distance to the first point,
 * which its light fades with. */
typedef struct ArtfShapeLit { float toDiffuse[3], diffuse, toHighlight[3], highlight, distance2; } ArtfShapeLit;
ARTF_INLINE void artfShapeLight(const float* shape, const float* center, float ball, const float* p, const float* n, const float* view, float rough, ArtfShapeLit* out) {
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
}

/* The same for a light that may have no shape: a ball's or a point's, as nearestOfBall in kernels.inc. */
ARTF_INLINE void artfLightPlace(const float* shape, const float* center, float ball, const float* p, const float* n, const float* view, float rough, ArtfShapeLit* out) {
    if (shape[3] > 0) {
        artfShapeLight(shape, center, ball, p, n, view, rough, out);
        return;
    }
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

/* What a thin film makes of what a surface reflects head on, seen at cos (n . v): two beams, off its top and its
 * bottom, at red, green and blue's wavelengths, as much as amount says. */
ARTF_INLINE void artfIridescence(float* f0, float cos, float amount, float ior, float thickness) {
    const float sin2 = (1.f - cos * cos) / (ior * ior), inside = sqrtf(artfMaxf(1.f - sin2, 0));
    const float r12 = (1.f - ior) / (1.f + ior);
    static const float waves[3] = {1.f / 650.f, 1.f / 510.f, 1.f / 475.f};
    for (int k = 0; k < 3; ++k) {
        const float root = sqrtf(artfClampf(f0[k], 0, .99f)), under = (1.f + root) / (1.f - root);
        const float r23 = (ior - under) / (ior + under);
        const float twice = 2.f * r12 * r23 * artfCosFast(12.5663706f * ior * thickness * inside * waves[k]);
        const float both = (r12 * r12 + r23 * r23 + twice) / (1.f + r12 * r12 * r23 * r23 + twice);
        f0[k] += (both - f0[k]) * amount;
    }
}

/* Sheen, Charlie's (Estevez and Kulla, 2017): its distribution, with pi put back as lights here have it taken out,
 * times Neubelt's visibility. inverse: 1 / its alpha. */
ARTF_INLINE float artfCharlie(float ndh, float ndl, float ndv, float inverse) {
    const float sin2 = artfMaxf(1.f - ndh * ndh, 1e-7f);
    const float d = (2.f + inverse) * artfExp2Fast(artfLog2Fast(sin2) * .5f * inverse) * .5f;
    return d / (4.f * (ndl + ndv - ndl * ndv) + 1e-4f);
}
/* What sheen takes from all around, of what lights a surface: a fit by eye to the integral of the above. */
ARTF_INLINE float artfSheenAround(float far, float rough) { return .14f * rough + (.5f - .2f * rough) * far; }

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
    float cullSize;            /* artfSceneSetCullSize's pixels */
    ArtfCamera camera;
    bool features[ARTF_FEATURE_COUNT];
    float lodError;
    ArtfHole hole;
    ArtfTrace trace;
    ArtfPostDesc post;
    ArtfDecalDesc decals[ARTF_MAX_DECALS];
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
    float* lut;                 /* ArtfPostDesc.lut's copy, lutSize^3 RGB, or NULL */
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
/* scene.c's, for scale.c: lighter versions of a mesh (touching no scene); a place for a mesh; an instance by handle,
 * NULL if there is none, said to no one; and an instance that changed, for what keeps copies. */
uint32_t artfSceneMakeLevels(const ArtfMeshData* m, ArtfMeshData* made, float* errors);
uint32_t artfSceneMeshPlace(ArtfScene* s, const ArtfMeshData* m);
ArtfInstanceData* artfSceneInstance(ArtfScene* s, ArtfInstance i);
void artfSceneInstanceChanged(ArtfScene* s, ArtfInstanceData* d);

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

/* Rays while drawing (ArtfShadowDesc.traced, ArtfPostDesc.tracedReflections): ArtfFrameData.traced holds these. The
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
/* What a ray from p (of normal n) along the mirror of the view v finds, strayed by rough through the ball's point
 * `point`, lit simply, into out; false when it finds nothing. */
bool artfTracedReflection(const ArtfScene* s, const ArtfFrameData* f, const float* p, const float* n, const float* v, float rough, uint32_t point, float* out);

/* A layer in view: an instance (dense index) that shows what is behind it, and the mesh it takes. */
typedef struct ArtfLayer {
    uint32_t instance, mesh;
    float distance;
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
            if (artfMeshLevelError(m, i) * maxScale * lodScale <= distance) chosen = m->lods[i];
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
