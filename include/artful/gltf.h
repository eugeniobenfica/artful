/* artful — loading glTF 2.0: a .gltf or a .glb, with its meshes, materials, textures (PNG and JPEG) and the
 * transforms of its nodes. It is a convenience on top of scene.h: it uses nothing else of the library's, and
 * a program that does not call it does not carry it.
 *
 * Read as well: KHR_lights_punctual (lights follow their nodes, and a directional one is the sun), cameras,
 * KHR_materials_emissive_strength, KHR_texture_transform and KHR_mesh_quantization. KHR_texture_basisu is read when
 * its KTX2 file holds a plain format, not Basis's own.
 *
 * What is not read: Draco and meshopt compression, Basis's formats. What that leaves out goes
 * to artfSetLog; a file that requires an extension other than those read is not read at all. */
#ifndef ARTFUL_GLTF_H
#define ARTFUL_GLTF_H

#include "artful/scene.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Adds what the file's default scene holds to s, under transform (NULL: as it is). Returns how many
 * instances it added: 0 if the file could not be read, and artfSetLog says why. */
uint32_t artfSceneLoadGltf(ArtfScene* s, const char* path, const ArtfTransform* transform);
/* The same from memory; folder: where files it names are (NULL: it names none). */
uint32_t artfSceneLoadGltfMemory(ArtfScene* s, const void* data, size_t size, const char* folder, const ArtfTransform* transform);

/* A file that stays known to the scene, for its clips to play: a handle, 0 is none. Its transform is what was
 * given, and artfModelSetTransform moves all of it. */
typedef uint32_t ArtfModel;

ArtfModel artfSceneLoadModel(ArtfScene* s, const char* path, const ArtfTransform* transform);
ArtfModel artfSceneLoadModelMemory(ArtfScene* s, const void* data, size_t size, const char* folder, const ArtfTransform* transform);
/* The work of a model apart from the scene: any thread may read it, decode its images and make its meshes with their
 * lighter versions, and adding what came of it is quick. Adding takes what was prepared, which is gone after. */
typedef struct ArtfPreparedModel ArtfPreparedModel;
ArtfPreparedModel* artfPrepareModel(const char* path);
ArtfPreparedModel* artfPrepareModelMemory(const void* data, size_t size, const char* folder);
void artfPreparedModelFree(ArtfPreparedModel* prepared);
/* What preparing made (images with their mipmaps, meshes with their lighter levels), as a file (cooked: artfFree's),
 * tied to the glTF it was made from by its hash; and a model prepared from it in place of making them: a file of
 * another glTF, or one that does not hold what it says, is said and left, and the model made as artfPrepareModelMemory
 * makes it. A file of an older version is read by a newer Artful. */
void* artfPreparedModelSave(const ArtfPreparedModel* prepared, size_t* size);
ArtfPreparedModel* artfPrepareModelCooked(const void* data, size_t size, const char* folder, const void* cooked, size_t cookedSize);
ArtfModel artfSceneAddPreparedModel(ArtfScene* s, ArtfPreparedModel* prepared, const ArtfTransform* transform);

void artfModelSetTransform(ArtfScene* s, ArtfModel m, const ArtfTransform* transform);
uint32_t artfModelInstanceCount(const ArtfScene* s, ArtfModel m);
/* The file's cameras, where their nodes are now: for artfSceneSetCamera. false: there is none. An orthographic one is
 * one (ArtfCamera.orthoHeight, twice its ymag; its width goes by the frame's shape, not by xmag). */
uint32_t artfModelCameraCount(const ArtfScene* s, ArtfModel m);
bool artfModelCamera(const ArtfScene* s, ArtfModel m, uint32_t camera, ArtfCamera* out);
uint32_t artfModelClipCount(const ArtfScene* s, ArtfModel m);
/* Borrowed; empty when the file gave none. */
const char* artfModelClipName(const ArtfScene* s, ArtfModel m, uint32_t clip);
float artfModelClipDuration(const ArtfScene* s, ArtfModel m, uint32_t clip);
/* Puts a model as a clip has it at this time, in seconds, from the start again past its end. */
void artfAnimationSet(ArtfScene* s, ArtfModel m, uint32_t clip, float time);

/* One clip of a mix: at its own time, as much as weight, over the nodes under the one named only (that node and all
 * below it: an upper body, a hand), or over all of them when only is NULL. */
typedef struct ArtfClipMix {
    uint32_t clip;
    float time;   /* in seconds, from the start again past its end */
    float weight; /* how much of it; a node takes each clip that reaches it by its share of their weights */
    const char* only;
} ArtfClipMix;

/* Puts a model as several clips mixed: walking into running, a wave over a walk. Each node takes, from the clips that
 * reach it, where they put it, each by its weight over those reaching it: positions and scales by their mean,
 * rotations by the normalized mean of their quaternions (the shortest way round), the weights of targets by their
 * mean. A node no clip reaches, or reached with no weight, stays as the file has it. One clip of weight 1 is
 * artfAnimationSet. false: a clip that is not there, or a node not named so (said through artfSetLog). */
bool artfAnimationMix(ArtfScene* s, ArtfModel m, const ArtfClipMix* clips, uint32_t count);

/* Decodes a PNG or a JPEG into BGRA pixels with their alpha as it is, for artfSceneAddTexture. The caller
 * owns them (artfFree). NULL: it is neither, or it is damaged. */
uint32_t* artfImageDecode(const void* data, size_t size, int* width, int* height);

#ifdef __cplusplus
}
#endif

#endif
