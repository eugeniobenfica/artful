/* artful — loading glTF 2.0: a .gltf or a .glb, with its meshes, materials, textures (PNG and JPEG) and the
 * transforms of its nodes. It is a convenience on top of scene.h: it uses nothing else of the library's, and
 * a program that does not call it does not carry it.
 *
 * Read as well: KHR_lights_punctual (lights follow their nodes, and a directional one is the sun), cameras,
 * KHR_materials_emissive_strength, KHR_texture_transform and KHR_mesh_quantization. KHR_texture_basisu is read when
 * its KTX2 file holds a plain format, not Basis's own.
 *
 * What is not read: orthographic cameras, Draco and meshopt compression, Basis's formats. What that leaves out goes
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
ArtfModel artfSceneAddPreparedModel(ArtfScene* s, ArtfPreparedModel* prepared, const ArtfTransform* transform);

void artfModelSetTransform(ArtfScene* s, ArtfModel m, const ArtfTransform* transform);
uint32_t artfModelInstanceCount(const ArtfScene* s, ArtfModel m);
/* The file's perspective cameras, where their nodes are now: for artfSceneSetCamera. false: there is none. */
uint32_t artfModelCameraCount(const ArtfScene* s, ArtfModel m);
bool artfModelCamera(const ArtfScene* s, ArtfModel m, uint32_t camera, ArtfCamera* out);
uint32_t artfModelClipCount(const ArtfScene* s, ArtfModel m);
/* Borrowed; empty when the file gave none. */
const char* artfModelClipName(const ArtfScene* s, ArtfModel m, uint32_t clip);
float artfModelClipDuration(const ArtfScene* s, ArtfModel m, uint32_t clip);
/* Puts a model as a clip has it at this time, in seconds, from the start again past its end. Mixing clips is
 * up to the program: artfInstanceSetJoints takes what it makes of them. */
void artfAnimationSet(ArtfScene* s, ArtfModel m, uint32_t clip, float time);

/* Decodes a PNG or a JPEG into BGRA pixels with their alpha as it is, for artfSceneAddTexture. The caller
 * owns them (artfFree). NULL: it is neither, or it is damaged. */
uint32_t* artfImageDecode(const void* data, size_t size, int* width, int* height);

#ifdef __cplusplus
}
#endif

#endif
