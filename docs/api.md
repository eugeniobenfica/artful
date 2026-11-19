# The API, by what it is for

The headers say what each call does. This is the map, and the rules that hold for all of them.

```c
#include "artful/artful.h"
```

## Rules

- **Nothing is shared with a renderer while it draws.** Use a scene from one thread, and change it between
  `artfRendererDraw` calls. A scene and a renderer belong to one thread at a time; another thread may take them
  over between frames. A mesh, a texture, a material, an instance, a light or the camera changed from another thread
  while the scene is drawn is said through `artfSetLog` (a check of the rule, not a guarantee). `artfPrepareMesh`,
  `artfPrepareTexture` and `artfPrepareModel` touch no scene, and run on any thread.
- **A scene outlives renderers.** A renderer that is lost is freed, a new one takes the same scene, and
  nothing is loaded again.
- **Handles are numbers; 0 is none.** An instance's or a mesh's handle stops working when it is removed: calls with
  it do nothing, and say so through `artfSetLog`. Their places are used again, each with a generation in the handle's
  top byte, so the handle of one removed does not stand for the one put in its place (for 255 such over one place).
  Textures' handles are their places alone, for now: one removed may stand for the next put in its place.
- **Running out of memory.** Main memory running out aborts: no call returns NULL for that. The GPU's memory running
  out does not: a texture goes up without its largest mipmaps, or is drawn white, and anything else makes the frame
  fail as a lost device does. Each is said by an event.
- **Events.** `artfSetEvents` hears what a program may want to act on, typed: a device lost, the GPU's memory run
  out, something refused, shaders made for a feature turned on mid-run. The log says the same, as words.
- **Structs grow.** Start from `artfRendererDefaults` and `artfMaterialDefaults`, or `= {0}`, and set fields by
  name. A vertex that is not zeroed must set `color`.
- **Colors.** Materials, lights and fog are linear light. Bytes (textures, vertex colors, frames, the background) are
  BGRA and sRGB, as pictures and screens are. Frames and overlays are premultiplied; textures are not.

## What 1.x will keep (proposed)

To be decided before 1.0; as it stands, for the owner to confirm:

- **Source compatibility.** A program written for 1.0 compiles and behaves the same against every 1.x: no function,
  type, enum value or field is removed or changes meaning in 1.x. New ones are added. What is to go is marked
  deprecated in a minor version and kept until 2.0.
- **No binary compatibility.** Artful is a static library, built with the program; 1.x promises nothing of its ABI
  (struct sizes and layouts may grow between minor versions). A program rebuilds against the version it ships. This is
  what Filament and wgpu promise, and what keeps structs free to grow.
- **Structs grow at their end**, with zero meaning what was before: start from the defaults or `= {0}`.
- **Pictures.** What a frame shows may change in a minor version, for the better, and is said in the changelog with
  the measure of it; the same scene on the same version gives the same bytes on the CPU, on every processor.
- **Files.** What Artful writes to disk (the pipeline cache, and any cache to come) is read by later 1.x, or thrown
  away and made again: never misread.

## A first frame

| | |
| --- | --- |
| `artfSceneNew`, `artfSceneFree` | A scene. |
| `artfRendererDefaults`, `artfRendererNew`, `artfRendererFree` | What draws it: Vulkan, then the CPU. |
| `artfRendererDraw` | A frame. false: the device is lost, make a new renderer. |
| `artfRendererReadPixels`, `artfRendererPixels` | The frame, in memory. |
| `artfRendererResize` | A new size. |
| `ArtfRendererOptions.renderScale` | With `hdr`: the scene drawn smaller and upscaled, with temporal antialiasing as TSR does. |
| `artfSetLog` | Why something did not work. |
| `artfSetEvents` | The same, typed, for what a program acts on: a device lost, the GPU's memory run out, something refused, shaders ready. |

## What is in a scene

| | |
| --- | --- |
| `artfSceneLoadGltf`, `artfSceneLoadGltfMemory` | All a glTF file holds. |
| `artfSceneLoadModel`, `artfModelSetTransform`, `artfAnimationSet` | The same as a model you can move, with its clips. |
| `artfAnimationMix` | Clips mixed by weight, each over all of a model or only below a node it names. |
| `artfModelCameraCount`, `artfModelCamera` | A file's cameras, for `artfSceneSetCamera`. |
| `artfSceneAddSkinnedMesh`, `artfInstanceSetJoints` | Your own bones, with levels of detail. |
| `artfSceneAddMesh` | Your own triangles. |
| `artfSceneAddCube`, `AddSphere`, `AddTorus`, `AddPlane` | Shapes. |
| `artfSceneAddTerrain` | A mesh from heights. |
| `artfSceneAddTerrainMaterial` | Its layers: up to four surfaces, by a painted map or by slope and height. |
| `artfSceneScatter` | Grass, trees or rocks over a terrain: placed, turned and sized at random, the same each time. |
| `artfSceneAddEmitter`, `artfEmitterStep`, `artfEmitterSet`, `artfEmitterBurst`, `artfSceneRemoveEmitter` | Particles: fire, smoke, sparks; with a wind that swirls, a ground they bounce on, and spin. `parallel`: a hundred thousand and more, moved where they are drawn, by a compute shader on the GPU. |
| `artfSceneAddDecal`, `artfDecalSet`, `artfSceneRemoveDecal` | A picture laid over what is drawn, through a box. |
| `artfSceneAddTexture`, `artfImageDecode` | Pixels, and PNG or JPEG to pixels. |
| `artfSceneAddTextureKtx2` | A KTX2 file, compressed as it is. |
| `artfPrepareMesh`, `artfPrepareTexture`, `artfSceneAddPreparedMesh`, `artfSceneAddPreparedTexture` | The slow part on your own thread. |
| `artfSceneRemoveMesh`, `artfSceneRemoveTexture`, `artfSceneMemory` | Giving memory back, and how much is used. |
| `artfSceneSetResidency`, `artfTextureSetResidentMip` | Who decides what stays on the GPU: you, you with help, or Artful. |
| `ArtfResidencyDesc.meshes`, `artfMeshSetResidentLevel`, `artfSceneAddMeshStreamed` | Meshes' levels too, by distance and a budget; a mesh read again when it is wanted. |
| `artfSceneBuildHlod`, `artfSceneAddHlod`, `artfSceneClearHlod` | Far away, many as one: cubes of instances merged into a lighter mesh each. |
| `artfSceneAddMaterial`, `artfMaterialSet` | Color, metal, roughness, emission, alpha, two sides, refraction, normal maps, unlit, a clear coat and sheen with their textures, a specular color, highlights drawn out, a thin film, light under the surface, and what glass takes inside, traced. |
| `artfGraphNew`, `artfExprMul` and the rest, `artfSceneAddGraph`, `artfMaterialSetParam` | Materials written in C. |
| `artfSceneAddInstance`, `artfSceneRemoveInstance` | A mesh placed in the world. |
| `artfInstanceSetTransform`, `SetMaterial`, `SetOpacity` | Changing one. |
| `artfSceneBounds` | A sphere around it all, to place a camera. |

## Light and the view

| | |
| --- | --- |
| `artfSceneSetCamera` | Where the eye is; `orthoHeight` makes its view orthographic, as tall as that in the world. |
| `artfScenePick`, `artfSceneRaycast` | What is under the mouse, or along any ray: the instance, the triangle, where, its normal, how far. |
| `artfSceneSetSun`, `artfSceneSetSunSize` | Light from far away; soft shadows, when traced or with shadows from rays. |
| `artfSceneSetAmbient` | Light from all around: a sky and a ground. |
| `artfSceneSetEnvironment`, `artfSceneSetEnvironmentLight`, `artfImageDecodeHdr` | Light from all around, from a picture of it, and that picture behind the scene. |
| `artfSceneSetSky` | Light from all around from the sun: a sky of air and haze, red at sunset, and the sun's own color through it; light scattered more than once, the sun drawn, and a layer of clouds (`ArtfCloudDesc`), if asked. |
| `artfSceneAddLight`, `artfLightSet`, `artfLightSetSpot`, `artfSceneRemoveLight` | Point lights and spots, up to 1024; the first 16 may cast shadows, and 32 more each frame. |
| `artfLightSetSize` | A light as wide as a bulb: wider highlights, and soft shadows. |
| `artfLightSetShape` | A tube or a rectangle in place of the bulb: long and wide highlights, light from all of it, soft shadows. |
| `artfLightSetProfile`, `artfIesDecode` | How much light goes each way, as a lamp's IES file says. |
| `artfSceneSetIndirect` | Light that bounced, from probes, every frame; walls stop it unless `throughWalls`. |
| `artfSceneSetPost` | With `ArtfRendererOptions.hdr`: bloom, ambient occlusion, reflections (marched across the frame, or from rays with `tracedReflections`), fog that light goes through, temporal antialiasing that follows what moves, depth of field, motion blur; then auto exposure, a choice of tone curve, white balance, saturation, contrast, vignette and chromatic aberration. |
| `artfSceneSetExposure` | Film: bright light rolls off to white. |
| `artfSceneSetFog`, `artfSceneSetFogHeight` | Air; thinner higher up. |
| `artfSceneSetBackground`, `artfSceneSetBackgroundAlpha` | What shows where nothing is. |

## How it is drawn

| | |
| --- | --- |
| `ArtfRendererOptions.antialias` | Four samples a pixel, with `hdr` too. |
| `ArtfRendererOptions.pipelineCache` | A file that makes the next start faster. |
| `ArtfRendererOptions.compressTextures` | Plain textures compressed as they go up. |
| `artfSceneSetFeature`, `artfSceneSetLodError` | Levels of detail, occlusion, cones. |
| `artfSceneSetCullSize` | What is too small to see is not drawn. |
| `artfSceneSetTrace` | Rays or paths in place of triangles; `denoise` takes the noise out; `hardware` uses the GPU's ray tracing hardware. |
| `artfSceneAddMorphMesh`, `artfInstanceSetMorphWeights` | A mesh with targets, mixed per instance, with or without bones. |
| `ARTF_FEATURE_LOD_FADE`, `ARTF_FEATURE_LAYERS_SORTED` | Levels that fade into each other; layers by each pixel's depth. |
| `artfSceneAddTextureStreamed` | A texture whose larger mipmaps are read when they become resident. |
| `artfPrepareModel`, `artfSceneAddPreparedModel` | A glTF model made on any thread, and added quickly. |
| `artfSceneSetShadows`, `artfLightSetShadow` | Shadows when drawing triangles; `soft` makes them as wide as their light; `traced` takes them from rays (through the ray tracing hardware where there is some), exact, through what shows through. |
| `artfSceneSetBackgroundPass`, `artfSceneBackgroundPass` | Light behind the scene that the program makes: a frame's parameters, a tile's light on the CPU, a compute shader on Vulkan; then its own bloom and tone curve. |
| `artfSceneSetBlackHole` | A black hole behind the scene: `artful/blackhole.h`, in the library `artful-blackhole` (`extensions/blackhole`), on the background pass. |
| `artfRendererSetOverlay` | A picture over the frame. |
| `artfRendererStats` | What a frame cost, on the CPU and on the GPU, and each of its passes (`passes`, `passCount`). |
| `artfRendererWarm` | Vulkan makes what a feature turned on mid-run needs while frames go on without it: this waits for that, and the next frame has all it asks for. |
| `artfRendererSetView`, `artfViewName`, `artfViewFromName` | What the frame shows: the picture, or one thing that went into it (`ArtfView`). |

## In 2D

`#include "artful/2d.h"`, and the library `artful-2d` (`artful::2d`), which takes only the core. The same rules: handles
are numbers and 0 is none, structs grow (start from `artfSprite2DDefaults`, `artfLight2DDefaults`, `artfGI2DDefaults`),
colors are linear light, frames premultiplied BGRA. The world's y goes down, as the frame's; angles are clockwise.

| | |
| --- | --- |
| `artfScene2DNew`, `artfScene2DFree` | A 2D scene. |
| `artfRenderer2DNew`, `artfRenderer2DDraw`, `artfRenderer2DPixels`, `artfRenderer2DResize` | What draws it, and its frame. A renderer keeps what gi gathered: one for each view. |
| `ArtfRenderer2DOptions.backend`, `artfRenderer2DBackend`, `artfRenderer2DDescription` | Vulkan where it runs, else the CPU (`ARTF_BACKEND2D_AUTO`); or one by name. |
| `artfScene2DAddTexture`, `artfScene2DSetTexture`, `artfScene2DRemoveTexture` | Pixels, straight or premultiplied (a 3D frame), or a normal map; mipmaps made. They repeat past their edges. |
| `artfScene2DAddSprite`, `artfSprite2DSet`, `artfSprite2DPlace`, `artfScene2DRemoveSprite` | A picture or a shape (`ARTF_SHAPE_RECT`, `ELLIPSE`, `POLYGON`, `LINE`) in a layer: its texture or a part of it (`uvMin`, `uvMax`), `nearest` for pixel art, a normal map, its own light (`emissive`, `emissiveMap`), a highlight, `unlit`, `castsShadow`. |
| `artfScene2DAddOccluder`, `artfScene2DRemoveOccluder` | An outline that stops light and is not drawn. |
| `artfScene2DAddLight`, `artfLight2DSet`, `artfScene2DRemoveLight` | Points, spots and suns; `radius` makes shadows soft, `height` how they rake across normal maps, `mask` what they light. |
| `artfScene2DSetAmbient` | Light all over. |
| `artfScene2DSetGI` | Light from what glows, that walls stop and send on, by radiance cascades, at a share of the frame (`scale`). |
| `artfScene2DSetPost` | Exposure, a tone curve, bloom. |
| `artfScene2DSetCamera`, `artfScene2DToWorld`, `artfScene2DToFrame` | The view: where, how near, turned; from pixels to the world and back. |
| `artfScene2DPick` | The sprite on top at a point. |
| `artfScene2DSetBackground` | What shows where nothing is; alpha 0 to go over a 3D frame (`artfRendererSetOverlay`). |
| `ArtfRenderer2DOptions.surface`, `noVsync` | Vulkan: frames shown in a window, through a swapchain of its own. |
| `artfRenderer2DReadPixels` | The last frame into memory, whatever the backend, shown in a window or not. |
| `artfRenderer2DStats` | What a frame took: shadows, gi, tiles, after; on Vulkan, the GPU's part (`gpuMs`, by timestamps). |

## Using it from CMake

As a subfolder:

```cmake
add_subdirectory(artful)
target_link_libraries(app PRIVATE artful::artful)   # 3D; artful::2d for 2D, or both
```

Installed, with `cmake --install build --prefix <dir>`:

```cmake
find_package(artful REQUIRED)
target_link_libraries(app PRIVATE artful::artful)
```
