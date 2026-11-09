# The API, by what it is for

The headers say what each call does. This is the map, and the rules that hold for all of them.

```c
#include "artful/artful.h"
```

## Rules

- **Nothing is shared with a renderer while it draws.** Use a scene from one thread, and change it between
  `artfRendererDraw` calls.
- **A scene outlives renderers.** A renderer that is lost is freed, a new one takes the same scene, and
  nothing is loaded again.
- **Handles are numbers; 0 is none.** An instance's handle stops working when it is removed: calls with it do
  nothing, and say so through `artfSetLog`.
- **Running out of memory aborts.** No call returns NULL for that.
- **Structs grow.** Start from `artfRendererDefaults` and `artfMaterialDefaults`, or `= {0}`, and set fields by
  name. A vertex that is not zeroed must set `color`.
- **Colors.** Materials, lights and fog are linear light. Bytes (textures, vertex colors, frames) are BGRA and
  hold the square root of light. Frames and overlays are premultiplied; textures are not.

## A first frame

| | |
| --- | --- |
| `artfSceneNew`, `artfSceneFree` | A scene. |
| `artfRendererDefaults`, `artfRendererNew`, `artfRendererFree` | What draws it: Vulkan, then the CPU. |
| `artfRendererDraw` | A frame. false: the device is lost, make a new renderer. |
| `artfRendererReadPixels`, `artfRendererPixels` | The frame, in memory. |
| `artfRendererResize` | A new size. |
| `artfSetLog` | Why something did not work. |

## What is in a scene

| | |
| --- | --- |
| `artfSceneLoadGltf`, `artfSceneLoadGltfMemory` | All a glTF file holds. |
| `artfSceneLoadModel`, `artfModelSetTransform`, `artfAnimationSet` | The same as a model you can move, with its clips. |
| `artfModelCameraCount`, `artfModelCamera` | A file's cameras, for `artfSceneSetCamera`. |
| `artfSceneAddSkinnedMesh`, `artfInstanceSetJoints` | Your own bones. |
| `artfSceneAddMesh` | Your own triangles. |
| `artfSceneAddCube`, `AddSphere`, `AddTorus`, `AddPlane` | Shapes. |
| `artfSceneAddTerrain` | A mesh from heights. |
| `artfSceneAddEmitter`, `artfEmitterStep`, `artfEmitterSet`, `artfSceneRemoveEmitter` | Particles: fire, smoke, sparks. |
| `artfSceneAddDecal`, `artfDecalSet`, `artfSceneRemoveDecal` | A picture laid over what is drawn, through a box. |
| `artfSceneAddTexture`, `artfImageDecode` | Pixels, and PNG or JPEG to pixels. |
| `artfSceneAddTextureKtx2` | A KTX2 file, compressed as it is. |
| `artfPrepareMesh`, `artfPrepareTexture`, `artfSceneAddPreparedMesh`, `artfSceneAddPreparedTexture` | The slow part on your own thread. |
| `artfSceneRemoveMesh`, `artfSceneRemoveTexture`, `artfSceneMemory` | Giving memory back, and how much is used. |
| `artfSceneSetResidency`, `artfTextureSetResidentMip` | Who decides what stays on the GPU: you, you with help, or Artful. |
| `artfSceneAddMaterial`, `artfMaterialSet` | Color, metal, roughness, emission, alpha, two sides, refraction, normal maps, unlit. |
| `artfGraphNew`, `artfExprMul` and the rest, `artfSceneAddGraph`, `artfMaterialSetParam` | Materials written in C. |
| `artfSceneAddInstance`, `artfSceneRemoveInstance` | A mesh placed in the world. |
| `artfInstanceSetTransform`, `SetMaterial`, `SetOpacity` | Changing one. |
| `artfSceneBounds` | A sphere around it all, to place a camera. |

## Light and the view

| | |
| --- | --- |
| `artfSceneSetCamera` | Where the eye is. |
| `artfSceneSetSun`, `artfSceneSetSunSize` | Light from far away; soft shadows, when traced. |
| `artfSceneSetAmbient` | Light from all around: a sky and a ground. |
| `artfSceneSetEnvironment`, `artfSceneSetEnvironmentLight`, `artfImageDecodeHdr` | Light from all around, from a picture of it, and that picture behind the scene. |
| `artfSceneAddLight`, `artfLightSet`, `artfLightSetSpot`, `artfSceneRemoveLight` | Point lights and spots, up to 1024; the first 16 may cast shadows, and 32 more each frame. |
| `artfSceneSetIndirect` | Light that bounced, from probes, every frame; walls stop it unless `throughWalls`. |
| `artfSceneSetPost` | With `ArtfRendererOptions.hdr`: bloom, ambient occlusion, reflections, fog that light goes through, temporal antialiasing, depth of field. |
| `artfSceneSetExposure` | Film: bright light rolls off to white. |
| `artfSceneSetFog` | Air. |
| `artfSceneSetBackground`, `artfSceneSetBackgroundAlpha` | What shows where nothing is. |

## How it is drawn

| | |
| --- | --- |
| `ArtfRendererOptions.antialias` | Four samples a pixel, with `hdr` too. |
| `ArtfRendererOptions.pipelineCache` | A file that makes the next start faster. |
| `ArtfRendererOptions.compressTextures` | Plain textures compressed as they go up. |
| `artfSceneSetFeature`, `artfSceneSetLodError` | Levels of detail, occlusion, cones. |
| `artfSceneSetTrace` | Rays or paths in place of triangles; `denoise` takes the noise out. |
| `artfSceneAddMorphMesh`, `artfInstanceSetMorphWeights` | A mesh with targets, mixed per instance, with or without bones. |
| `ARTF_FEATURE_LOD_FADE`, `ARTF_FEATURE_LAYERS_SORTED` | Levels that fade into each other; layers by each pixel's depth. |
| `artfSceneAddTextureStreamed` | A texture whose larger mipmaps are read when they become resident. |
| `artfPrepareModel`, `artfSceneAddPreparedModel` | A glTF model made on any thread, and added quickly. |
| `artfSceneSetShadows`, `artfLightSetShadow` | Shadows when drawing triangles. |
| `artfSceneSetBlackHole` | A black hole behind the scene. |
| `artfRendererSetOverlay` | A picture over the frame. |
| `artfRendererStats` | What a frame cost, on the CPU and on the GPU. |

## Using it from CMake

As a subfolder:

```cmake
add_subdirectory(artful)
target_link_libraries(app PRIVATE artful::artful)
```

Installed, with `cmake --install build --prefix <dir>`:

```cmake
find_package(artful REQUIRED)
target_link_libraries(app PRIVATE artful::artful)
```
