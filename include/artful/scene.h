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
/* What is too small to see is not drawn: an instance whose bounding ball is less than this many pixels across, where it
 * is, as Unreal's minimum screen size. It still casts shadows, and is traced. 0 (to start with): all of it is drawn. */
void artfSceneSetCullSize(ArtfScene* s, float pixels);

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

/* What a scene holds, in bytes. Resident: of textures, at the mipmaps they have now; of meshes, the levels held
 * (see ArtfResidencyDesc.meshes). */
typedef struct ArtfSceneMemory { size_t meshes, textures, residentTextures, residentMeshes; } ArtfSceneMemory;
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
/* And with a mesh: its heaviest level to keep (0 the whole, k its k-th lighter version). */
typedef void (*ArtfMeshResidencyFn)(void* user, ArtfMesh mesh, int level);

typedef struct ArtfResidencyDesc {
    ArtfResidency mode;
    size_t budget;      /* bytes of textures; 0: as much as they need */
    int interval;       /* frames between looks; 0: 30 */
    ArtfResidencyFn report; /* assisted */
    void* user;
    /* Meshes too, as Unreal streams them: of each that has lighter versions, the levels that what uses it needs, by
     * its distance (with room to come nearer), and what fits meshBudget; its lightest level always stays. A level
     * that is not held leaves the GPU, and memory too for a mesh that can be read again (artfSceneAddMeshStreamed);
     * what would have drawn it draws the next level held, on every backend alike. Nothing of it while traced. */
    bool meshes;
    size_t meshBudget;  /* bytes of meshes' levels; 0: as much as they need */
    ArtfMeshResidencyFn reportMesh; /* assisted */
} ArtfResidencyDesc;

void artfSceneSetResidency(ArtfScene* s, const ArtfResidencyDesc* desc);

/* A mesh whose triangles the program gives again when they are wanted: read fills vertexCount vertices and indexCount
 * indices, on the thread that makes the mesh resident, the same each time. It is read once here, for its lighter
 * versions (the lod feature must be on), and only its lightest stays in memory until more is wanted. false: it could
 * not be read, and what was there stays. 0: it has no triangles. */
typedef bool (*ArtfMeshReadFn)(void* user, ArtfVertex* vertices, uint32_t* indices);
ArtfMesh artfSceneAddMeshStreamed(ArtfScene* s, uint32_t vertexCount, uint32_t indexCount, ArtfMeshReadFn read, void* user);
/* The heaviest level of a mesh held: 0 is all of it; each step up gives up a level, down to its lightest. */
void artfMeshSetResidentLevel(ArtfScene* s, ArtfMesh mesh, int level);
int artfMeshResidentLevel(const ArtfScene* s, ArtfMesh mesh);

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
 * is not culled by parts. With the lod feature on, one of 128 triangles or more gets lighter versions, each with the
 * joints, weights and targets of the vertices it keeps; each instance takes the one its distance calls for, as a
 * mesh that does not bend would, and only that one is bent. */
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
    /* A clear layer over it, as on lacquer, varnish or a car's paint (glTF's KHR_materials_clearcoat): a second
     * highlight, of no color, and what it reflects is taken from what is under it. 0 to 1 */
    float clearcoat;
    float clearcoatRoughness;  /* the layer's own: 0 is as smooth as can be */
    /* Light that grazing angles catch, as on velvet, felt or dust (glTF's KHR_materials_sheen, its color): a rim of
     * this color, strongest where the eye or the light skims the surface. 0: none */
    ArtfVec3 sheen;
    float sheenRoughness;      /* how wide that rim is, 0 to 1, as glTF's (Charlie's distribution): 0 is a thin one */
    /* Their textures, as glTF's: the coat's amount (red) and roughness (green), a normal map of its own, sheen's color
     * (its bytes, as a base color texture's) and roughness (alpha). 0: none */
    ArtfTexture clearcoatMap, clearcoatRoughnessMap, clearcoatNormalMap, sheenMap, sheenRoughnessMap;
    ArtfVec3 specularColor;    /* what it reflects head on, if not metal, against white (KHR_materials_specular); 0: white */
    /* Highlights drawn out along a direction, as on brushed metal (KHR_materials_anisotropy): 0 to 1, and that
     * direction, in radians from the way u grows on the surface. */
    float anisotropy, anisotropyRotation;
    /* The colors of a thin film over it, as on soap or oil (KHR_materials_iridescence): 0 to 1; the film's index (0:
     * 1.3) and thickness in nanometres (0: 400). */
    float iridescence, iridescenceIor, iridescenceThickness;
    /* Traced, what light keeps of each color through its inside (KHR_materials_volume): this much after this far.
     * 0: it keeps all */
    ArtfVec3 attenuation;
    float attenuationDistance;
    /* Light that goes under the surface and comes out around where it went in, and through thin parts: skin, wax,
     * marble. Its color (0: none), and how far it goes, in the scene's units (0: .05). */
    ArtfVec3 subsurface;
    float subsurfaceRadius;
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
/* A sky from the sun, as its light from all around (in place of a picture): the air (Rayleigh) and haze (Mie)
 * scatter the sun's light once, and ozone takes some, along each direction from a point above the ground. It is blue
 * by day, red and orange where the sun is low, and dark past the horizon; the ground below takes the sun through the
 * air. Call it after artfSceneSetSun, and again when the sun moves: it takes tens of milliseconds, on all cores.
 * The sun's color is then what of desc.sun reaches the ground (and, with clouds' shadow, what they leave of it where
 * the eye is). NULL: none. */
/* A layer of clouds in that sky (as Unreal's Volumetric Clouds, made into the picture): followed through in steps,
 * each lit by the sun through the cloud above it and the air (Beer's law, with the powder of their edges and what
 * light scattered many times inside adds), and by the sky. They are the picture's, so both backends show, light and
 * trace them alike. The noise they are made of is the same everywhere. coverage 0: none. */
typedef struct ArtfCloudDesc {
    float coverage;   /* how much of the sky they cover, 0 to 1 */
    float base;       /* how high their bottom is, in meters; 0: 1500 */
    float thickness;  /* from bottom to top, in meters; 0: 2500 */
    float density;    /* how dense they are, against a cumulus's; 0: 1 */
    ArtfVec3 wind;    /* how far they have moved, in meters (x and z; y moves their shapes within): for an animation */
    bool shadow;      /* the scene's sun is dimmed by what they take of it over the eye (softly, from a few points
                       * around it): the eye in their shade, or not (with keepSun, it is not) */
} ArtfCloudDesc;
typedef struct ArtfSkyDesc {
    ArtfVec3 sun;     /* the sun's light above the air; 0: 3, 3, 3 */
    float haze;       /* how much haze, against a clear day's; 0: 1 */
    float altitude;   /* the eye's height over the ground, in meters; 0: 200 */
    ArtfVec3 ground;  /* the color of the ground under the horizon; 0: .1 */
    int width;        /* of the picture it makes, twice its height; 0: 256 */
    bool hidden;      /* it lights and is reflected, but the frame's background stays */
    bool keepSun;     /* the scene's sun keeps its color */
    /* Light scattered more than once (Hillaire, 2020): it makes the horizon and twilight as bright as they are, which
     * light scattered once leaves dark. 1 as the air has it; 0: none, as before. With it (or the sun drawn, or clouds),
     * what the air leaves of the sun is read from a table made once, so the sky takes less time, not more. */
    float multiple;
    /* The sun drawn in the sky, this many degrees across (the sun's own is .53; a texel of the picture is 360 / width
     * degrees, so a wider picture shows it sharper), darker toward its edge as the sun is. Its light is the sun's as it
     * reaches the eye, times 10: enough to bloom, and not so much that a traced path that meets it is a firefly. The
     * light it gives is still the scene's sun's. 0: not drawn. */
    float sunDisc;
    ArtfCloudDesc clouds;
} ArtfSkyDesc;
bool artfSceneSetSky(ArtfScene* s, const ArtfSkyDesc* desc);
/* Decodes a Radiance .hdr (RGBE) into linear RGB, three floats a pixel, for artfSceneSetEnvironment. The caller owns
 * them (artfFree). NULL: it is not one, or it is damaged. */
float* artfImageDecodeHdr(const void* data, size_t size, int* width, int* height);
/* 0 when ARTF_MAX_LIGHTS are there already. radius: where its light ends. Only the first 16 cast shadows. */
ArtfLight artfSceneAddLight(ArtfScene* s, ArtfVec3 position, ArtfVec3 color, float radius);
void artfLightSet(ArtfScene* s, ArtfLight l, ArtfVec3 position, ArtfVec3 color, float radius);
/* Makes it a spot: light goes toward direction, full up to inner radians from it, none past outer. */
void artfLightSetSpot(ArtfScene* s, ArtfLight l, ArtfVec3 direction, float inner, float outer);
void artfSceneRemoveLight(ArtfScene* s, ArtfLight l);
/* Gives it a size: a ball of light this wide around its position, as a bulb or a lamp's shade is, in place of a
 * point. Its highlights are as wide as it looks, on what is smooth too, with their light kept (Karis's nearest point);
 * traced, its shadows go soft with distance. For the sixteen lights the frame holds: those past them stay points.
 * 0: a point. */
void artfLightSetSize(ArtfScene* s, ArtfLight l, float radius);
/* Gives it a shape, as Unreal's rect lights and source length: with width 0, a tube as long as length along `along`
 * through its position, as thick as artfLightSetSize says; with a width, a rectangle length x width around its
 * position, whose light goes only to the side `facing` says (along is made square to facing). Its light reaches as far
 * from all of it; highlights are as long and as wide as it looks (Karis's nearest point); traced, its shadows go soft.
 * For the sixteen lights the frame holds. length 0: a ball or a point again. */
void artfLightSetShape(ArtfScene* s, ArtfLight l, ArtfVec3 along, ArtfVec3 facing, float length, float width);
/* How much light goes each way, as a lamp's maker measured it: vertical angles evenly from 0 to 180 degrees, ends
 * included, 0 being the light's direction (a spot's) or straight down; rows of those for horizontal angles evenly from
 * 0 to 360, from +x round toward +z (or from +y, for a light along x); one row: the same all around. Light times this.
 * Resampled to 96 x 16. For the sixteen lights the frame holds. NULL: none. */
void artfLightSetProfile(ArtfScene* s, ArtfLight l, const float* table, int vertical, int horizontal);
/* Reads an IES file (LM-63) into such a table, its brightest 1: 181 vertical angles, a degree apart, and 1 row for a
 * light that is the same all around, 72 (5 degrees apart) for one that is not. The caller owns it (artfFree). NULL: it is
 * not one. */
float* artfIesDecode(const void* data, size_t size, int* vertical, int* horizontal);

/* ---- the view ---- */

typedef struct ArtfCamera {
    ArtfVec3 position;
    ArtfVec3 target;
    ArtfVec3 up;
    float fovY;  /* radians */
    float near_; /* no far plane: see artfMat4Perspective */
    /* Orthographic, when above 0: the view is this tall in the world, its width by the frame's shape, and all its rays
     * go the same way, along position to target (a plan, an elevation, an isometric game). fovY does not count then.
     * What is nearer than near_ along the view is cut, and so is what is farther than far_. 0: perspective. */
    float orthoHeight;
    float far_;  /* orthographic only: where depth ends, along the view; 0: just past the scene's bounds */
} ArtfCamera;

void artfSceneSetCamera(ArtfScene* s, const ArtfCamera* camera);
/* Shadows when drawing triangles: tracing has its own. Each light that casts takes square maps of this
 * side: three for the sun, one for a spot, six for a point light, sixteen in all; what does not fit is logged
 * and casts none. What shows through casts no shadow. A map is drawn again only when what can fall in it changed. */
typedef struct ArtfShadowDesc {
    int resolution;  /* of a map, a multiple of 64: 1024 is a good start; 0: no shadows */
    float distance;  /* how far from the eye the sun's shadows go; 0: 60 */
    /* Shadows as soft as their light is wide, sharp where they touch what casts them (Fernando's PCSS): the sun's
     * size (artfSceneSetSunSize) and the sixteen lights' (artfLightSetSize). What is in the way is looked for in the
     * map around each pixel, and its distance says how wide the penumbra is there: 50 reads in place of 9. Lights
     * past the sixteen, and fog that light goes through, keep sharp ones. */
    bool soft;
    /* Shadows from rays, as Unreal's ray traced shadows, for the sun and the sixteen lights: each pixel asks the
     * scene's trees (or the GPU's ray tracing hardware) whether anything is between it and the light, and what shows
     * through lets light through as much as it is not there. Exact, with no map; soft by the sun's size and the
     * lights' (four rays a pixel for a light with a size, in a pattern over each 2 x 2 pixels that turns each frame
     * with temporal antialiasing). They take no maps: with resolution 0, lights past the sixteen and fog that light
     * goes through have no shadows, and with a resolution they keep their maps. */
    bool traced;
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
/* Fog that thins with height, for fog that light goes through (ArtfPostDesc.volumetric): its density is
 * artfSceneSetFog's at height base, and falls by e every 1 / falloff above it (and grows below, up to 16 times).
 * falloff 0: the same at every height. */
void artfSceneSetFogHeight(ArtfScene* s, float base, float falloff);
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
    float turbulence;     /* a wind that swirls, as smoke and sparks have it: how hard it pushes, per second */
    float turbulenceSize; /* how wide its swirls are; 0: 1 */
    bool ground;          /* they bounce off the plane y = groundHeight, and do not go through it */
    float groundHeight;
    float bounce;         /* what they keep of their speed into the ground: 0 they stop, 1 all of it */
    float friction;       /* what they lose of their speed along it, at each bounce: 0 to 1 */
    float spin;           /* how they turn, facing the eye, in radians a second: each gives or takes this much */
    /* Many at once, as Niagara's GPU emitters: its particles are not instances, but a buffer, moved in parallel where
     * the scene is drawn (by a compute shader when Vulkan draws, on the CPU's workers otherwise: the same to a
     * rounding) and drawn as quads facing the eye, after all other layers, in the order of their places in the buffer.
     * most may then be a million. A particle's numbers come from its birth's: steps that differ in size give others.
     * Set when made. */
    bool parallel;
} ArtfEmitterDesc;
typedef uint32_t ArtfEmitter;

ArtfEmitter artfSceneAddEmitter(ArtfScene* s, const ArtfEmitterDesc* desc);
/* All but most, which stays as it was made. */
void artfEmitterSet(ArtfScene* s, ArtfEmitter e, const ArtfEmitterDesc* desc);
void artfEmitterStep(ArtfScene* s, ArtfEmitter e, float seconds);
/* So many are born at the next step, as many as there is room for, as well as those its rate gives. */
void artfEmitterBurst(ArtfScene* s, ArtfEmitter e, uint32_t count);
uint32_t artfEmitterAlive(const ArtfScene* s, ArtfEmitter e);
void artfSceneRemoveEmitter(ArtfScene* s, ArtfEmitter e);

/* Terrain: a mesh from heights (width x depth of them, by rows, times height), cell apart, centered, facing +y, with
 * texture coordinates repeating uvRepeat times across. Its lighter versions are made as any mesh's. */
ArtfMesh artfSceneAddTerrain(ArtfScene* s, const float* heights, int width, int depth, float cell, float height, float uvRepeat);

/* Layers of a terrain, as a landscape's: up to four surfaces, each a color times a texture tiled over it, blended by a
 * map of weights painted over the terrain (its red, green, blue and alpha: how much of each), or, with no map, by
 * rules: the second where it is steep, the third high up. A material in C (artfSceneAddGraph), so both backends draw
 * it alike. */
typedef struct ArtfTerrainLayer {
    ArtfVec3 color;       /* times the texture, in linear light; 0 0 0: white */
    ArtfTexture texture;  /* 0: the color alone */
    float tiling;         /* how many times the texture repeats across the terrain; 0: 1 */
    float roughness;      /* 0: .8 */
} ArtfTerrainLayer;
typedef struct ArtfTerrainLayersDesc {
    ArtfTerrainLayer layers[4];
    uint32_t count;       /* 1 to 4 */
    ArtfTexture weights;  /* the map, over the whole terrain; 0: the rules below */
    float uvRepeat;       /* what artfSceneAddTerrain was given; 0: 1 */
    float steep;          /* rules: the second layer where the surface's up is under this (1 flat, 0 a wall); 0: .8 */
    float high;           /* the third above this height, in the world */
    float blend;          /* how wide the rules' edges are; 0: .1 */
} ArtfTerrainLayersDesc;
ArtfMaterial artfSceneAddTerrainMaterial(ArtfScene* s, const ArtfTerrainLayersDesc* desc);

/* Foliage, rocks and the like scattered over a terrain made from the same heights (as artfSceneAddTerrain, then placed
 * by at): instances of a mesh, so many a unit of area, each at its own place, turned about up and sized at random,
 * standing up or leaning with the ground, and only where the ground is flat enough and high enough. The same seed,
 * the same places. How many it placed; their handles, if instances is given, up to room. */
typedef struct ArtfScatterDesc {
    const float* heights;
    int width, depth;
    float cell, height;
    ArtfTransform at;       /* where the terrain is; zeros: where it was made */
    ArtfMesh mesh;
    ArtfMaterial material;
    float density;          /* a unit of area */
    float scaleMin, scaleMax; /* 0: 1 */
    bool lean;              /* each leans with the ground under it */
    float steepest;         /* none where the ground's up is under this; 0: anywhere */
    float lowest, highest;  /* only between these heights, in the world; both 0: anywhere */
    uint32_t seed;
} ArtfScatterDesc;
uint32_t artfSceneScatter(ArtfScene* s, const ArtfScatterDesc* desc, ArtfInstance* instances, uint32_t room);

/* Far away, many as one (Unreal's hierarchical levels of detail): instances that do not move, gathered in cubes of
 * the world, each cube's of a material merged into one mesh, made lighter, and drawn in their place past a distance:
 * one instance where there were hundreds. The choice is made once a frame, before any backend draws, so all draw the
 * same; while it holds them, the instances' opacity is the merged mesh's business. Their shadows go with what is
 * drawn. Instances that bend, show through, are particles or are merged already are left as they are. Instances moved
 * or removed after this are not followed: clear and build again. */
typedef struct ArtfHlodDesc {
    float cell;        /* the side of the cubes; 0: 32 */
    float keep;        /* the share of a merged mesh's triangles kept; 0: .125 */
    /* From the eye to a cube's ball, past which the merged mesh is drawn; 0: where what making it lighter moved stays
     * under artfSceneSetLodError's pixels, as levels of detail are chosen, and never within the ball's radius. */
    float distance;
    uint32_t least;    /* the fewest instances worth merging; 0: 2 */
} ArtfHlodDesc;
/* All instances of the scene, by cubes: how many merged meshes it made. On all cores. */
uint32_t artfSceneBuildHlod(ArtfScene* s, const ArtfHlodDesc* desc);
/* These instances, as one, whatever their place: the merged mesh's instance, or 0. */
ArtfInstance artfSceneAddHlod(ArtfScene* s, const ArtfInstance* instances, uint32_t count, const ArtfHlodDesc* desc);
/* All merged meshes go, and what they held shows again. */
void artfSceneClearHlod(ArtfScene* s);
/* How many merged meshes were drawn in place of theirs at the last frame. */
uint32_t artfSceneHlodShown(const ArtfScene* s);

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
typedef enum ArtfToneMap {
    ARTF_TONE_ACES = 0,     /* Narkowicz's fit to ACES: bright colors go to white */
    ARTF_TONE_NEUTRAL = 1,  /* Khronos's PBR Neutral: a base color under light of 1 shows as it is, hue kept */
    ARTF_TONE_REINHARD = 2, /* x / (1 + x), each channel */
    ARTF_TONE_NONE = 3      /* cut at 1 */
} ArtfToneMap;
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

    /* The look: what is done to the light last, before it is bytes. */
    ArtfToneMap toneMap;    /* the curve from light to what a screen shows, when there is an exposure */
    bool autoExposure;      /* the exposure follows what the frame shows: artfSceneSetExposure's (or 1) then
                             * multiplies it, as a camera's compensation does. The frame is metered on a grid of
                             * 32 x 18 points, the middle weighed twice the corners, and its mean in stops is aimed at */
    float exposureKey;      /* the grey the mean is brought to; 0: .18 */
    float exposureSpeed;    /* the share of the way there taken each frame, in stops; 0: .05; 1: at once */
    float exposureMin;      /* the least auto exposure multiplies by; 0: 1 / 64 */
    float exposureMax;      /* the most; 0: 64 */
    float temperature;      /* white balance: -1 cool (bluer) to 1 warm (redder); 0 as it is */
    float tint;             /* -1 toward green to 1 toward magenta; 0 as it is */
    float saturation;       /* -1 grey, 0 as it is, 1 twice as far from grey */
    float contrast;         /* around the middle of what is shown: -1 flat, 0 as it is, 1 twice as steep */
    float vignette;         /* light falls toward the corners: at 1 a corner keeps a quarter */
    float chromatic;        /* red and blue part toward the edges, as a cheap lens's: how far, in pixels, at a corner */
    /* Motion blur: what moved since the frame before is spread along the way it went, the eye's own motion too, as a
     * film's shutter does. This is the share of a frame the shutter is open: .5 is a film camera's. Temporal
     * antialiasing follows what moves by the same measure: instances, and what bones and targets bend, by its
     * vertices. What is still and in front stays sharp over what moves behind it (McGuire et al.'s reconstruction). */
    float motionBlur;

    /* Auto exposure by a histogram, as Unreal's: with either of these, the frame is metered on 64 x 36 points into a
     * histogram of stops, the darkest and the brightest shares are left out, and the mean of what is left is aimed
     * at; so a small bright light, or a black corner, does not move the exposure. 0 and 0: the mean of all. */
    float exposureLow;      /* the share of the darkest left out, 0 to 1 (Unreal's is .1) */
    float exposureHigh;     /* the share of the brightest left out (Unreal's is .1) */
    float exposureSpeedDown;/* the share of the way taken each frame when the exposure falls (the frame got brighter):
                             * eyes go dark fast and light slowly; 0: exposureSpeed */

    /* Grading, in what a screen shows (after the tone curve and contrast), each channel from 0 to 1: */
    ArtfVec3 lift;          /* added to the dark, nothing to white: out = gain (in + lift (1 - in)); 0: none */
    ArtfVec3 gamma;         /* then out^(1 / gamma): above 1 the middle is lighter; 0, 0, 0: 1, 1, 1 */
    ArtfVec3 gain;          /* 0, 0, 0: 1, 1, 1 */
    ArtfVec3 shadows;       /* color wheels: what the dark, the middle and the bright are multiplied by, each as much as */
    ArtfVec3 midtones;      /* a pixel is of it (by its luminance: (1 - l)^2, 2 l (1 - l), l^2); 0, 0, 0: white */
    ArtfVec3 highlights;
    /* A look-up table of colors, as a .cube file has (artfLutDecodeCube): lutSize^3 RGB triples, red the fastest,
     * applied last, between its points. What it takes is what a screen shows (each channel the square root of light,
     * near enough to sRGB's). Copied by artfSceneSetPost. lutSize from 2 to 65; NULL: none. */
    const float* lut;
    int lutSize;
    float lutAmount;        /* how much of it: 0: all */
    float grain;            /* film grain, new each frame, the same on both backends: 0 to 1 (.3 is plain to see) */
    float flare;            /* lens flare: what blooms, again as ghosts mirrored across the middle; with bloom */
    /* Reflections from rays in place of the march across the frame, as Unreal's ray traced reflections: what smooth
     * surfaces mirror is what a ray finds in the scene, off the frame too, lit simply (its color under the sun and
     * the sixteen lights with no shadows, its own light, and light from all around); rougher, the ray strays as far.
     * It takes `reflections` as how much. */
    bool tracedReflections;
    /* After temporal antialiasing or upscaling (ArtfRendererOptions.renderScale): edges brought back, 0 to 1, each
     * pixel away from its neighbours, never past them. */
    float sharpen;
} ArtfPostDesc;

/* Decodes a .cube file (Adobe's and Resolve's 3D look-up tables) into lutSize^3 RGB triples, red the fastest, for
 * ArtfPostDesc.lut. The caller owns them (artfFree). NULL: it is not one, or it is damaged. */
float* artfLutDecodeCube(const void* data, size_t size, int* lutSize);

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
    /* Rays go through the GPU's ray tracing hardware, where it has some (Vulkan's ray queries), in place of the
     * trees the compute shader walks: the same picture, and on a large scene, faster. Elsewhere it is as without. */
    bool hardware;
} ArtfTraceDesc;

/* NULL: drawing as ever. Meshes get what rays need the first time they are traced, on the calling thread:
 * about as long as their levels of detail. */
void artfSceneSetTrace(ArtfScene* s, const ArtfTraceDesc* desc);

/* ---- picking ----
 * What is under the mouse, or along any ray: through the trees tracing uses, so on any renderer or none. The first
 * call makes them for the meshes there are (as long as their levels of detail take), and they are made again only
 * for what changed: a pick costs a ray. A ray sees the fronts of what has one side, as the frame does, and bones as
 * they are posed. It goes through holes, through what is less than half there (faded, or more transparent than not)
 * and through particles. */
typedef struct ArtfPickHit {
    ArtfInstance instance;
    uint32_t triangle;  /* of its mesh, in the order Artful keeps it in (not the order given) */
    ArtfVec3 position;  /* in the world */
    ArtfVec3 normal;    /* the triangle's own, of length 1, toward where the ray came from */
    float distance;     /* along the ray: from its origin; for a pick, from the eye (orthographic: from its plane) */
    float u, v;         /* texture coordinates there */
} ArtfPickHit;

/* The first surface along a ray, at most maxDistance away (0: any). direction need not be of length 1. false: none. */
bool artfSceneRaycast(ArtfScene* s, ArtfVec3 origin, ArtfVec3 direction, float maxDistance, ArtfPickHit* out);
/* The first surface at a point of a frame of width x height, in pixels from its top left corner (a pixel's center is
 * at + .5), as a camera sees it (NULL: the scene's), from near on, as the frame shows it. */
bool artfScenePick(ArtfScene* s, const ArtfCamera* camera, float x, float y, int width, int height, ArtfPickHit* out);

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
