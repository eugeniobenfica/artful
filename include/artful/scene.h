/* artful — what is drawn: meshes, textures, materials, instances, lights and the camera. A scene is
 * plain memory and knows no renderer: the same scene can be drawn by any renderer, and outlives them
 * (a window that loses its surface makes a new renderer, and nothing has to be loaded again).
 *
 * Use a scene from one thread. Nothing here may run while a renderer draws it. */
#ifndef ARTFUL_SCENE_H
#define ARTFUL_SCENE_H

#include "artful/core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Handles: 0 is none. Materials live as long as the scene. An instance's handle stops working when it is
 * removed: calls with it are ignored, and logged (artfSetLog). */
typedef uint32_t ArtfMesh;
typedef uint32_t ArtfTexture;
typedef uint32_t ArtfMaterial;
typedef uint32_t ArtfInstance;
typedef uint32_t ArtfLight;

/* How many point lights can be on at once. */
/* Lights a scene holds. The first 16 that are on (by handle) are drawn as they always were, and may cast shadows;
 * those past them light what they reach, by the 64 x 64 squares of the frame each falls in, up to 64 a square,
 * with no shadow, and tracing leaves them out. */
#define ARTF_MAX_LIGHTS 1024

typedef struct ArtfScene ArtfScene;

ArtfScene* artfSceneNew(void);
void artfSceneFree(ArtfScene* s);

/* ---- features ----
 * What can be switched on and off, at any time, for a scene and whatever draws it. ARTFUL_FEATURES in
 * the environment sets the defaults: ARTFUL_FEATURES=-lod,+occlusion. */
typedef enum ArtfFeature {
    /* "lod": far objects drawn with lighter meshes, made when a mesh is added (on). Their pixels are
     * near, not equal: see artfSceneSetLodError. */
    ARTF_FEATURE_LOD,
    /* "occlusion": what is behind other things is not drawn (off). It never changes the picture. It
     * pays in scenes where much is hidden, and costs a little in those where little is. */
    ARTF_FEATURE_OCCLUSION,
    /* "cones": meshlets are dropped that face away, or that are out of view, before their vertices are read: on the CPU
     * and, for meshes of many meshlets, on the GPU (on). */
    ARTF_FEATURE_CONES,
    /* Occlusion on or off by what it does for this scene: the renderer tries both now and then, for a few frames, and
     * keeps what took less. The pixels are the same either way. */
    ARTF_FEATURE_OCCLUSION_AUTO,
    /* A mesh goes from one level of detail to the next over a stretch of distance, through a pattern of pixels, and not
     * all at once. What is in that stretch is drawn twice, each level in its half of the pixels. Off to start with. */
    ARTF_FEATURE_LOD_FADE,
    /* Layers go by each pixel's depth, and not by their centers, so two that cut through each other show as they are: up
     * to eight in a pixel, the farthest going first when there are more. It costs memory on the GPU: 96 bytes a pixel.
     * Where a device cannot do it, layers go whole. Off to start with. */
    ARTF_FEATURE_LAYERS_SORTED,
    ARTF_FEATURE_COUNT
} ArtfFeature;

void artfSceneSetFeature(ArtfScene* s, ArtfFeature feature, bool on);
bool artfSceneFeature(const ArtfScene* s, ArtfFeature feature);
/* How far a lighter mesh may stray on screen, in pixels (1 to start with). */
void artfSceneSetLodError(ArtfScene* s, float pixels);

/* ---- meshes ---- */

typedef struct ArtfVertex {
    float px, py, pz;
    float nx, ny, nz; /* of length 1 */
    float u, v;
    uint32_t color;   /* BGRA, as a texture's: multiplies the material, and its alpha shows what is behind
                       * (see artfInstanceSetOpacity). 0: white */
} ArtfVertex;

/* Triangles, counter-clockwise from the front. The data is copied, then packed to 16 bytes a vertex (4
 * more where colors are given), reordered for the vertex cache, and cut into meshlets: the order given
 * is not kept. With the lod feature on, a mesh of 128 triangles or more also gets lighter versions of
 * itself. 0 when there are no triangles. */
ArtfMesh artfSceneAddMesh(ArtfScene* s, const ArtfVertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount);

/* The work of a mesh or a texture, apart from the scene: any thread may do it, and adding what came of it
 * takes no time. So a world can load behind what is being drawn. levels: make lighter versions. */
typedef struct ArtfPreparedMesh ArtfPreparedMesh;
typedef struct ArtfPreparedTexture ArtfPreparedTexture;
ArtfPreparedMesh* artfPrepareMesh(const ArtfVertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount, bool levels);
ArtfPreparedTexture* artfPrepareTexture(const uint32_t* pixels, int width, int height, int stride);
void artfPreparedMeshFree(ArtfPreparedMesh* prepared);
void artfPreparedTextureFree(ArtfPreparedTexture* prepared);
/* These take what was prepared, which is gone after. */
ArtfMesh artfSceneAddPreparedMesh(ArtfScene* s, ArtfPreparedMesh* prepared);
ArtfTexture artfSceneAddPreparedTexture(ArtfScene* s, ArtfPreparedTexture* prepared);

/* Gives back a mesh, with its lighter versions, or a texture, on both backends. false, and logged, while an
 * instance or a material still uses it. A texture's place is used again; a mesh's memory is. */
bool artfSceneRemoveMesh(ArtfScene* s, ArtfMesh mesh);
bool artfSceneRemoveTexture(ArtfScene* s, ArtfTexture texture);

/* What a scene holds, in bytes. Resident: of textures, at the mipmaps they have now. */
typedef struct ArtfSceneMemory { size_t meshes, textures, residentTextures; } ArtfSceneMemory;
void artfSceneMemory(const ArtfScene* s, ArtfSceneMemory* out);

/* Who decides how much of each texture a GPU holds. Artful works out what each one needs, from how large what
 * uses it shows, and what fits a budget, as often as asked. All the mipmaps stay in memory, so a texture can
 * come back at once; what changes is what the frame draws from, and what the GPU keeps. */
typedef enum ArtfResidency {
    ARTF_RESIDENCY_MANUAL,    /* nothing happens by itself: artfTextureSetResidentMip is all there is */
    ARTF_RESIDENCY_ASSISTED,  /* Artful says what it would do, and the program decides */
    ARTF_RESIDENCY_AUTOMATIC  /* Artful does it */
} ArtfResidency;

/* What Artful would do with a texture: its largest mipmap to keep. */
typedef void (*ArtfResidencyFn)(void* user, ArtfTexture texture, int mip);

typedef struct ArtfResidencyDesc {
    ArtfResidency mode;
    size_t budget;      /* bytes of textures; 0: as much as they need */
    int interval;       /* frames between looks; 0: 30 */
    ArtfResidencyFn report; /* assisted */
    void* user;
} ArtfResidencyDesc;

void artfSceneSetResidency(ArtfScene* s, const ArtfResidencyDesc* desc);

/* A texture whose larger mipmaps stay out of memory until they are resident: Artful asks for each when it needs it,
 * on the thread that makes it resident (for files, assisted residency lets the program choose when). pixels: BGRA, of
 * the mipmap's size, each side halved and at least 1. false: it could not be read, and what was there stays. */
typedef bool (*ArtfTextureReadFn)(void* user, int mip, uint32_t* pixels);
/* mipCount: 0 for all down to one pixel. Starts from residentMip. 0: its first mipmaps could not be read. */
ArtfTexture artfSceneAddTextureStreamed(ArtfScene* s, int width, int height, int mipCount, int residentMip, ArtfTextureReadFn read, void* user);
/* 0 is all of it; each step up is a quarter of the memory, and half the sharpness. */
void artfTextureSetResidentMip(ArtfScene* s, ArtfTexture texture, int mip);

/* A mesh that bends: what bones move each vertex, and by how much (the weights add up to 1). */
typedef struct ArtfVertexSkin {
    uint16_t joints[8];
    float weights[8];
} ArtfVertexSkin;

/* skin: one for each vertex. Each instance of such a mesh is its own copy, moved by artfInstanceSetJoints. Such a mesh
 * has no levels of detail and is not culled by parts. */
ArtfMesh artfSceneAddSkinnedMesh(ArtfScene* s, const ArtfVertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount, const ArtfVertexSkin* skin);

/* What a mesh can turn into: for each vertex, what is added to its position and normal, three floats each. NULL: nothing. */
typedef struct ArtfMorphTarget {
    const float* positions;
    const float* normals;
} ArtfMorphTarget;

/* A mesh with targets, and bones if skin is not NULL. artfInstanceSetMorphWeights mixes them, before the bones. */
ArtfMesh artfSceneAddMorphMesh(ArtfScene* s, const ArtfVertex* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount, const ArtfVertexSkin* skin, const ArtfMorphTarget* targets, uint32_t targetCount);

/* Shapes for tests and first steps, centered on the origin. */
ArtfMesh artfSceneAddCube(ArtfScene* s, float size);
ArtfMesh artfSceneAddSphere(ArtfScene* s, float radius, int rings, int segments);
ArtfMesh artfSceneAddTorus(ArtfScene* s, float radius, float tube, int rings, int segments);
ArtfMesh artfSceneAddPlane(ArtfScene* s, float size, float uvRepeat); /* in the xz plane, facing +y */

/* ---- textures and materials ---- */

/* pixels: BGRA, width * height, stride in pixels (0: width). Copied, with its mipmaps made here. Its
 * alpha is not premultiplied, and counts only for a material that asks: see ArtfAlphaMode. */
ArtfTexture artfSceneAddTexture(ArtfScene* s, const uint32_t* pixels, int width, int height, int stride);

/* A KTX2 file's blocks, with their mipmaps: BC1, BC3, BC7, ETC2 and ASTC 4 x 4. A GPU that takes the format reads
 * them as they are. Elsewhere, and on the CPU, they are turned into pixels, which BC7 and ASTC cannot be yet: they
 * draw grey there, and it is logged. No supercompression. 0: it could not be read. */
ArtfTexture artfSceneAddTextureKtx2(ArtfScene* s, const void* data, size_t size);

typedef enum ArtfAlphaMode {
    ARTF_ALPHA_OPAQUE, /* solid, whatever the texture's alpha says */
    ARTF_ALPHA_MASK,   /* where alpha is below alphaCutoff there is a hole: leaves, fences */
    ARTF_ALPHA_BLEND   /* shows what is behind it, by alpha and transparency: see artfInstanceSetOpacity */
} ArtfAlphaMode;

/* What a surface is made of, as glTF says it: light follows from that, by the same physics in all lights
 * and on both backends. Metal has no diffuse color: baseColor is what it reflects. Others reflect 4% head
 * on, and all things more at a grazing angle. */
typedef struct ArtfMaterialDesc {
    ArtfVec3 baseColor;  /* multiplies the texture, in linear light */
    ArtfTexture texture; /* 0: none */
    float specular;      /* 0: no reflection at all; 1: what its substance gives */
    float roughness;     /* 0 is a mirror, 1 is chalk */
    ArtfVec3 emissive;   /* light of its own, added at the end */
    ArtfAlphaMode alphaMode;
    float alphaCutoff;   /* with a mask; 0: .5 */
    float transparency;  /* with blending: 0 is all there, 1 is nothing */
    bool doubleSided;    /* its back is drawn too, lit as a front */
    float ior;           /* when traced, what goes through bends: glass is 1.5; 0: it goes straight */
    float metallic;      /* 0..1 */
    ArtfTexture normalMap;     /* tangent space, +y up, as glTF's; 0: none */
    float normalScale;         /* how far it bends normals; 0: 1 */
    ArtfTexture surfaceMap;    /* green multiplies roughness, blue metallic; 0: none */
    ArtfTexture occlusionMap;  /* red: how much light from all around reaches a place; 0: none */
    float occlusionStrength;   /* 0: 1 */
    uint32_t graph;            /* what artfSceneAddGraph gave: see below; 0: none */
    bool unlit;                /* its color as it is, lit by nothing (glTF's KHR_materials_unlit): with emissive added */
} ArtfMaterialDesc;

/* White, not metal, half rough: fields set by name on this stay right when the struct grows. */
ArtfMaterialDesc artfMaterialDefaults(void);
ArtfMaterial artfSceneAddMaterial(ArtfScene* s, const ArtfMaterialDesc* desc);
void artfMaterialSet(ArtfScene* s, ArtfMaterial m, const ArtfMaterialDesc* desc);

/* ---- materials of your own ----
 * What a surface is, computed for each pixel. An expression is built by calls, and your own C functions build
 * larger ones out of them. It is the one definition: the shaders are generated from it and the CPU runs it,
 * so both draw the same. All values are four numbers; a number given where four are wanted is four alike.
 *
 *   ArtfGraph* g = artfGraphNew();
 *   ArtfExpr uv = artfExprUv(g);
 *   ArtfExpr wave = artfExprSin(g, artfExprAdd(g, artfExprMul(g, artfExprX(g, uv), artfExprNumber(g, 20)), artfExprParam(g, 0)));
 *   artfGraphSetOutput(g, ARTF_OUT_COLOR, artfExprMix(g, artfExprConst(g, 0, 0, .3f, 1), artfExprConst(g, .1f, .5f, 1, 1), wave));
 *   desc.graph = artfSceneAddGraph(scene, g);
 *   artfGraphFree(g);
 *
 * A material takes four parameters (artfMaterialSetParam), which change with no cost: time, say. Textures are
 * read in their largest mipmap, in linear light. A scene's first frame after a new graph waits for shaders. */
typedef struct ArtfGraph ArtfGraph;
typedef uint32_t ArtfExpr;

/* What an expression can say: what is not set stays as the material has it. */
typedef enum ArtfGraphOutput {
    ARTF_OUT_COLOR,     /* the base color, in linear light */
    ARTF_OUT_EMISSIVE,  /* light of its own */
    ARTF_OUT_ROUGHNESS,
    ARTF_OUT_METALLIC,
    ARTF_OUT_ALPHA,     /* with ARTF_ALPHA_BLEND */
    ARTF_OUT_NORMAL     /* in the world */
} ArtfGraphOutput;

ArtfGraph* artfGraphNew(void);
void artfGraphFree(ArtfGraph* g);
void artfGraphSetOutput(ArtfGraph* g, ArtfGraphOutput output, ArtfExpr e);
/* Copied; the id for ArtfMaterialDesc.graph. */
uint32_t artfSceneAddGraph(ArtfScene* s, const ArtfGraph* g);

ArtfExpr artfExprConst(ArtfGraph* g, float x, float y, float z, float w);
ArtfExpr artfExprNumber(ArtfGraph* g, float v);
ArtfExpr artfExprUv(ArtfGraph* g);        /* u, v, 0, 0 */
ArtfExpr artfExprPosition(ArtfGraph* g);  /* in the world */
ArtfExpr artfExprNormal(ArtfGraph* g);
ArtfExpr artfExprToEye(ArtfGraph* g);     /* of length 1 */
ArtfExpr artfExprBaseColor(ArtfGraph* g); /* what the material and its texture say */
ArtfExpr artfExprParam(ArtfGraph* g, uint32_t index); /* 0 to 3 */
ArtfExpr artfExprTexture(ArtfGraph* g, ArtfTexture texture, ArtfExpr uv);
ArtfExpr artfExprAdd(ArtfGraph* g, ArtfExpr a, ArtfExpr b);
ArtfExpr artfExprSub(ArtfGraph* g, ArtfExpr a, ArtfExpr b);
ArtfExpr artfExprMul(ArtfGraph* g, ArtfExpr a, ArtfExpr b);
ArtfExpr artfExprDiv(ArtfGraph* g, ArtfExpr a, ArtfExpr b);
ArtfExpr artfExprMin(ArtfGraph* g, ArtfExpr a, ArtfExpr b);
ArtfExpr artfExprMax(ArtfGraph* g, ArtfExpr a, ArtfExpr b);
ArtfExpr artfExprDot(ArtfGraph* g, ArtfExpr a, ArtfExpr b); /* of x, y and z */
ArtfExpr artfExprFloor(ArtfGraph* g, ArtfExpr a);
ArtfExpr artfExprFract(ArtfGraph* g, ArtfExpr a);
ArtfExpr artfExprAbs(ArtfGraph* g, ArtfExpr a);
ArtfExpr artfExprSqrt(ArtfGraph* g, ArtfExpr a);
ArtfExpr artfExprSin(ArtfGraph* g, ArtfExpr a);
ArtfExpr artfExprCos(ArtfGraph* g, ArtfExpr a);
ArtfExpr artfExprMix(ArtfGraph* g, ArtfExpr a, ArtfExpr b, ArtfExpr t);
ArtfExpr artfExprIf(ArtfGraph* g, ArtfExpr condition, ArtfExpr a, ArtfExpr b); /* a where condition > 0, each number alone */
ArtfExpr artfExprX(ArtfGraph* g, ArtfExpr a); /* one number of the four, four times */
ArtfExpr artfExprY(ArtfGraph* g, ArtfExpr a);
ArtfExpr artfExprZ(ArtfGraph* g, ArtfExpr a);
ArtfExpr artfExprW(ArtfGraph* g, ArtfExpr a);
ArtfExpr artfExprVec(ArtfGraph* g, ArtfExpr x, ArtfExpr y, ArtfExpr z); /* the x of each, and 1 */

void artfMaterialSetParam(ArtfScene* s, ArtfMaterial m, uint32_t index, float x, float y, float z, float w);

/* ---- instances ---- */

/* A mesh placed in the world. material 0: a plain grey one. */
ArtfInstance artfSceneAddInstance(ArtfScene* s, ArtfMesh mesh, ArtfMaterial material, const ArtfTransform* transform);
void artfSceneRemoveInstance(ArtfScene* s, ArtfInstance i);
void artfInstanceSetTransform(ArtfScene* s, ArtfInstance i, const ArtfTransform* transform);
void artfInstanceSetMaterial(ArtfScene* s, ArtfInstance i, ArtfMaterial material);
/* 1 to start with; 0: not drawn. An instance below 1, of a mesh with colors below full alpha, or of a
 * material that blends, is a layer: drawn after everything solid, the farthest first, each showing its nearest surface over what
 * is behind it. So a fading object does not show its own insides, and two that cut through each other
 * are drawn one over the other. */
void artfInstanceSetOpacity(ArtfScene* s, ArtfInstance i, float opacity);
/* Poses an instance of a skinned mesh: joints[j] takes a vertex as modelled to where bone j puts it. */
void artfInstanceSetJoints(ArtfScene* s, ArtfInstance i, const ArtfTransform* joints, uint32_t count);
/* How much of each of its mesh's targets an instance shows: 0 to 1, or past that. */
void artfInstanceSetMorphWeights(ArtfScene* s, ArtfInstance i, const float* weights, uint32_t count);
uint32_t artfSceneInstanceCount(const ArtfScene* s);
/* A sphere around all instances, for placing a camera; false when there are none. */
bool artfSceneBounds(const ArtfScene* s, ArtfVec3* center, float* radius);

/* ---- light ---- */

/* direction: where the light goes to. A zero color turns the sun off. */
void artfSceneSetSun(ArtfScene* s, ArtfVec3 direction, ArtfVec3 color);
/* Light from all around: sky from above, ground from below. */
void artfSceneSetAmbient(ArtfScene* s, ArtfVec3 sky, ArtfVec3 ground);
/* Light from all around, from a picture of it: equirectangular (longitude across, from +z round by -x to -z in the
 * middle and on; latitude down, +y at the top row), linear RGB, three floats a pixel. It takes the place of sky and
 * ground for all that is lit, reflected and traced, times light; shown: it is the background too, where the scene
 * draws nothing. Made into maps here, on all cores: about 100 ms. NULL: sky and ground again. false: no picture. */
bool artfSceneSetEnvironment(ArtfScene* s, const float* rgb, int width, int height, float light, bool shown);
/* The same maps, with another light, or shown or not: costs nothing. */
void artfSceneSetEnvironmentLight(ArtfScene* s, float light, bool shown);
/* Decodes a Radiance .hdr (RGBE) into linear RGB, three floats a pixel, for artfSceneSetEnvironment. The caller owns
 * them (artfFree). NULL: it is not one, or it is damaged. */
float* artfImageDecodeHdr(const void* data, size_t size, int* width, int* height);
/* 0 when ARTF_MAX_LIGHTS are there already. radius: where its light ends. Only the first 16 cast shadows. */
ArtfLight artfSceneAddLight(ArtfScene* s, ArtfVec3 position, ArtfVec3 color, float radius);
void artfLightSet(ArtfScene* s, ArtfLight l, ArtfVec3 position, ArtfVec3 color, float radius);
/* Makes it a spot: light goes toward direction, full up to inner radians from it, none past outer. */
void artfLightSetSpot(ArtfScene* s, ArtfLight l, ArtfVec3 direction, float inner, float outer);
void artfSceneRemoveLight(ArtfScene* s, ArtfLight l);

/* ---- the view ---- */

typedef struct ArtfCamera {
    ArtfVec3 position;
    ArtfVec3 target;
    ArtfVec3 up;
    float fovY;  /* radians */
    float near_; /* no far plane: see artfMat4Perspective */
} ArtfCamera;

void artfSceneSetCamera(ArtfScene* s, const ArtfCamera* camera);
/* Shadows when drawing triangles: tracing has its own. Each light that casts takes square maps of this
 * side: three for the sun, one for a spot, six for a point light, sixteen in all; what does not fit is logged
 * and casts none. What shows through casts no shadow. A map is drawn again only when what can fall in it changed. */
typedef struct ArtfShadowDesc {
    int resolution;  /* of a map, a multiple of 64: 1024 is a good start; 0: no shadows */
    float distance;  /* how far from the eye the sun's shadows go; 0: 60 */
} ArtfShadowDesc;

/* NULL: none. The sun casts; other lights do when asked. */
void artfSceneSetShadows(ArtfScene* s, const ArtfShadowDesc* desc);
void artfLightSetShadow(ArtfScene* s, ArtfLight l, bool casts);

/* Light past 1 rolls off to white, as film does, by the same curve as a black hole's; 0 (to start with):
 * light is cut at 1. What a surface shows is multiplied by this first. */
void artfSceneSetExposure(ArtfScene* s, float exposure);
/* Air that is not clear: a surface fades into color with distance, by about e^(-density x distance). The
 * background is not touched: set it to the same color. density 0: none. */
void artfSceneSetFog(ArtfScene* s, ArtfVec3 color, float density);
/* When traced, how wide the sun is, as the tangent of its radius: 0 (to start with) gives hard shadows. */
void artfSceneSetSunSize(ArtfScene* s, float size);
/* What shows where nothing is drawn, as it shows (no light applied): 0..1 for red, green, blue. */
void artfSceneSetBackground(ArtfScene* s, ArtfVec3 color);
/* How much of the background there is: 1 to start with; 0: nothing, and the frame's alpha is what the
 * scene covers, to put it over something else. A black hole fills the frame all the same. */
void artfSceneSetBackgroundAlpha(ArtfScene* s, float alpha);

/* ---- what is made of the scene's own parts ----
 * Particles: an emitter's are instances of one quad, each facing the eye and fading as it ages; artfEmitterStep moves
 * them, makes new ones and turns them to the camera as it is then. They show through each other, as layers do. */
typedef struct ArtfEmitterDesc {
    ArtfVec3 position;    /* where they are born, give or take area each way */
    float area;
    ArtfVec3 velocity;    /* how they leave, give or take spread of its speed each way */
    float spread;
    ArtfVec3 gravity;     /* what pulls them, per second */
    float drag;           /* how fast they slow: 0 not at all */
    float rate;           /* born a second */
    float life;           /* seconds; 0: 2 */
    float size, sizeEnd;  /* a side, when born and when it dies */
    ArtfVec3 color, colorEnd;
    float opacity, opacityEnd;
    float glow;           /* how much of their color is light of their own */
    ArtfTexture texture;  /* its alpha is its shape; 0: a soft round puff */
    uint32_t most;        /* alive at once; 0: 1024 */
} ArtfEmitterDesc;
typedef uint32_t ArtfEmitter;

ArtfEmitter artfSceneAddEmitter(ArtfScene* s, const ArtfEmitterDesc* desc);
/* All but most, which stays as it was made. */
void artfEmitterSet(ArtfScene* s, ArtfEmitter e, const ArtfEmitterDesc* desc);
void artfEmitterStep(ArtfScene* s, ArtfEmitter e, float seconds);
uint32_t artfEmitterAlive(const ArtfScene* s, ArtfEmitter e);
void artfSceneRemoveEmitter(ArtfScene* s, ArtfEmitter e);

/* Terrain: a mesh from heights (width x depth of them, by rows, times height), cell apart, centered, facing +y, with
 * texture coordinates repeating uvRepeat times across. Its lighter versions are made as any mesh's. */
ArtfMesh artfSceneAddTerrain(ArtfScene* s, const float* heights, int width, int depth, float cell, float height, float uvRepeat);

/* Decals: a picture laid over what is drawn, through a box. The transform takes the unit box (-.5 to .5 each way) to
 * the world; what is inside it takes the picture from above, down the box's y: u along its x, v along its z. The
 * picture times color goes over the surface's own color by the picture's alpha times opacity, fading toward the box's
 * top and bottom and on what does not face up the box; so does roughness, when given. Both backends; not on what
 * blends (particles, glass). Up to ARTF_MAX_DECALS. */
#define ARTF_MAX_DECALS 256
typedef struct ArtfDecalDesc {
    ArtfTransform transform;
    ArtfTexture texture;  /* 0: none, the color alone */
    ArtfVec3 color;       /* 0, 0, 0 is black: (1, 1, 1) for the picture as it is */
    float opacity;        /* 0 to 1 */
    float roughness;      /* where it covers; 0: the surface's own */
} ArtfDecalDesc;
typedef uint32_t ArtfDecal;

ArtfDecal artfSceneAddDecal(ArtfScene* s, const ArtfDecalDesc* desc); /* 0: no room */
void artfDecalSet(ArtfScene* s, ArtfDecal d, const ArtfDecalDesc* desc);
void artfSceneRemoveDecal(ArtfScene* s, ArtfDecal d);

/* ---- after the frame: what a renderer made with ArtfRendererOptions.hdr does to it ----
 * Each is off at 0. Lengths are the world's; what a scene asks of a renderer without hdr is not done. */
typedef struct ArtfPostDesc {
    float bloom;            /* light past the threshold spreads around it: .05 is a camera's lens */
    float bloomThreshold;   /* what is bright enough to spread, after exposure; 0: 1 */
    float occlusion;        /* ambient occlusion: how much light from all around corners and creases lose, 0 to 1 */
    float occlusionRadius;  /* how far it looks; 0: .5 */
    float reflections;      /* screen-space reflections: how much of what the frame shows smooth surfaces mirror, 0 to 1 */
    float volumetric;       /* fog that light goes through, lit by the sun through its shadows: how much, 0 to 1. It takes
                             * artfSceneSetFog's color and density, and replaces that fog */
    float scattering;       /* where that fog sends light: 0 all ways, toward .9 mostly onward (Henyey-Greenstein's g) */
    bool temporal;          /* temporal antialiasing: the eye moves within a pixel from frame to frame, and frames are
                             * mixed, as far as each pixel's history can be followed */
    float focus;            /* depth of field: the distance in focus */
    float aperture;         /* how much what is out of focus spreads, in pixels, for something twice as far: 0 none */
} ArtfPostDesc;

/* NULL: none. */
void artfSceneSetPost(ArtfScene* s, const ArtfPostDesc* desc);

/* ---- light that bounced, every frame ----
 * A grid of probes over a box (the scene's bounds, as they are when this is called, unless given): each frame each probe follows a few
 * rays through the scene (as tracing does, on the calling thread's cores, for both backends), and keeps what light
 * reaches it from each way, mixed with what it had. Surfaces take light from all around from the probes near them, in
 * place of sky and ground or a picture: so light bounces from one surface to the next, once more each frame. Each
 * probe knows how far it sees each way, and a point it cannot see takes next to nothing from it: walls stop light. */
typedef struct ArtfIndirectDesc {
    int probes[3];      /* across the bounds; 0: 8, 4, 8 */
    int rays;           /* each probe follows each frame; 0: 64 */
    float hysteresis;   /* how much of what it had a probe keeps each frame: more is steadier, and slower; 0: .9 */
    float strength;     /* 0: 1 */
    ArtfVec3 low, high; /* the box the probes fill; both zero: the scene's bounds */
    bool throughWalls;  /* leave out what each probe sees: lighter (about 5 ms less at 1920 x 1080 on a Vega 8, the
                         * pixel shader reading a third as much), and light leaks through walls thinner than a cell */
} ArtfIndirectDesc;

/* NULL: none. */
void artfSceneSetIndirect(ArtfScene* s, const ArtfIndirectDesc* desc);

/* ---- tracing ----
 * Light followed along rays, in place of drawing triangles: on both backends, and on any GPU, as this
 * needs no ray tracing hardware. Each frame adds samples to what the frames before gathered, for as long
 * as the scene and the camera stay as they are, so a still picture gets better by itself.
 *
 *   rays    what the rasterizer shows, with real shadows, reflections in what shines, and what shows
 *           through going through: few samples are enough.
 *   paths   all light, however often it bounced: colors bleed, corners darken, and what a light does not
 *           reach directly is lit. It starts noisy, and takes hundreds of samples. */
typedef enum ArtfTraceMode { ARTF_TRACE_NONE, ARTF_TRACE_RAYS, ARTF_TRACE_PATHS } ArtfTraceMode;

typedef struct ArtfTraceDesc {
    ArtfTraceMode mode;
    int samples;    /* added to each pixel in a frame; 0: 1 */
    int maxSamples; /* where a still picture is done, and a frame costs nothing more; 0: never */
    int bounces;    /* how far light is followed; 0: 3 for rays, 6 for paths */
    /* Noise is filtered out, across the frame and from one frame to the next, by what surfaces are: a moving
     * scene does not start over, and a sample or two a frame looks clean, a little soft at first. It takes
     * about eight times a frame's size in memory. */
    bool denoise;
} ArtfTraceDesc;

/* NULL: drawing as ever. Meshes get what rays need the first time they are traced, on the calling thread:
 * about as long as their levels of detail. */
void artfSceneSetTrace(ArtfScene* s, const ArtfTraceDesc* desc);

/* ---- a black hole ----
 * Behind everything else in the scene: its light is traced along the paths Kerr's metric gives it,
 * pixel by pixel, through a glowing disc of gas and out to the stars, then goes through high dynamic
 * range, bloom and tone mapping. What the scene holds is drawn over it, and is not bent. Lengths given
 * in masses are multiples of mass: with no spin the horizon is at 2 and the photons' orbit at 3. */
typedef struct ArtfBlackHoleDesc {
    ArtfVec3 position;
    ArtfVec3 axis;       /* what the disc turns around; zero: +y */
    float mass;          /* in the world's lengths */
    float spin;          /* -1 to 1, of the most a hole can have; the same way round as the gas when above 0 */
    float discInner;     /* where the disc starts, in masses; 0: where orbits end, for this spin */
    float discOuter;     /* where it has faded out */
    float discHeight;    /* how deep it is, each way, per unit of radius */
    float discThickness; /* how much of the light behind it the disc stops */
    float discGain;      /* how bright it is */
    float doppler;       /* 0: the disc looks the same all around; 1: the side coming at the eye is as
                          * much brighter and bluer as relativity says */
    float stars;         /* how bright they are; 0: none */
    float bloom;         /* the veil of a lens: how much light spills around what is far too bright */
    float exposure;
    float time;          /* in masses: going forward, the gas flows; it cannot be run back */
    float shutter;       /* how much time a frame's exposure lasts: with several rays a pixel, motion blur */
    ArtfVec3 velocity;   /* the eye's, in the world, as a fraction of light's: what it sees moves toward
                          * where it is going, and with doppler, turns bluer there */
    int samples;         /* rays a pixel, on a side: 1 is one ray, 3 is nine */
} ArtfBlackHoleDesc;

/* A hole that looks like what cinema has shown of one: seen from outside, a thin bright disc, its far
 * side thrown up over the shadow and under it. */
ArtfBlackHoleDesc artfBlackHoleDefaults(void);
/* How fast a circular orbit goes at r masses, as a fraction of light's: for ArtfBlackHoleDesc.velocity. */
float artfBlackHoleOrbitSpeed(float spin, float r);
/* NULL: none. Call it every frame with a new time: the disc's gas is a fluid, and this is where it moves
 * on, in fixed steps, on the calling thread (a few milliseconds each). A change of the disc's size or of
 * the spin starts it over, which takes some time. */
void artfSceneSetBlackHole(ArtfScene* s, const ArtfBlackHoleDesc* desc);

#ifdef __cplusplus
}
#endif

#endif
