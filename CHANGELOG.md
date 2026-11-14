# Changelog

## Unreleased

- **An orthographic camera**: `ArtfCamera.orthoHeight`, the view's height in the world, and `far_`, where its depth
  ends (0: just past the scene's bounds). All its rays go one way, so things keep their size however far they are:
  plans, elevations, isometric games. Depth is `(far - t) / (far - near)`, even across a triangle as w is 1, where a
  perspective's stays `near / t`; the frame says which (`ArtfFrameData.ortho`, in what was padding), and everything
  that read depth, or took rays from the eye, reads it so on both backends: culling, levels of detail and what is too
  small to see (all as at one distance), cones, occlusion, the sun's cascades (boxes along the view), the lists of
  lights past sixteen, the view a surface is lit from, fog, all that comes after a frame in floats (occlusion,
  reflections, temporal antialiasing, motion blur, depth of field, the grid of fog), decals' mipmaps, the picture of
  all around behind the scene (one direction: one color) and tracing. The backends agree on a scene with all of it:
  0.02% of pixels apart drawn, 0.00% after the frame, 0.01% traced. glTF's orthographic cameras are orthographic now.
  A perspective frame is the same bytes as before: a test holds four of them, drawn, after the frame, with fog that
  light goes through, and traced, to the hashes the commit before drew on the CPU; on the GPU, the same four came
  out the same bytes as before on the Radeon.
- **Picking**: `artfScenePick`, what is under a point of the frame, and `artfSceneRaycast`, the first surface along a
  ray (`ArtfPickHit`: the instance, the triangle, where, its normal, how far, and the texture coordinates there).
  Through the trees tracing makes, so on any renderer or none, with bones as posed; through holes, what is less than
  half there, and particles. The trees are made the first time, and again only for what changed: among `city`'s 40,201
  instances, 16 ms for the first pick, then 10,000 picks in 9 ms. A pick between frames leaves what moved
  for the next frame's filter to see.
- `artful-render`: `--ortho height`.
- **The ray tracing hardware**: `ArtfTraceDesc.hardware`. Where the GPU has acceleration structures and ray queries
  (`VK_KHR_acceleration_structure`, `VK_KHR_ray_query`), a ray goes through them in place of the trees the compute
  shader walks: a bottom level for each mesh traced, from the same triangles, and the top over the traced instances,
  in their order, so that all that follows a hit is as it was. The picture is the same to the pixel: on a Radeon
  RX 9060 XT, rays and paths, with what has one side and two, and after an instance moved, 0.00% of the pixels apart.
  `city` (40,201 instances) traced at 1280 x 720 went from 7.1 ms a frame to 1.1. Elsewhere, and on the CPU, it is as
  without (`ARTFUL_NO_RAY_QUERY`: as a device without).
- **The look**, in `ArtfPostDesc`, last before the frame is bytes, on both backends: **auto exposure**
  (`autoExposure`), the frame metered on 32 x 18 points in stops (log2 by bits and a polynomial, the same everywhere),
  the middle weighed twice the corners, brought to a key at a speed, within bounds, in a pass of its own on the GPU;
  a choice of **tone curve** (`toneMap`: ACES's fit as before, Khronos's PBR Neutral, Reinhard's, none); **white
  balance** (`temperature`, `tint`), **saturation**, **contrast**, a **vignette** and **chromatic aberration**. With
  none of it asked, a frame is the same bytes as before.
- **Motion followed**: each renderer keeps how each instance moved since the frame before (`artfMotionStep`, by
  handle), and each pixel which instance it shows (on the GPU in the surface target, now two words a pixel). Temporal
  antialiasing follows a surface to where it was, so what moves no longer leaves a trail (a sphere going sideways
  under it: 0.54 of 255 from it drawn still, on average); and **motion blur** (`ArtfPostDesc.motionBlur`, the share of
  a frame the shutter is open) spreads it along the way it went, eight points, at most 32 pixels. What bones bend
  moves as its instance does.
- **Clear coat and sheen**: `ArtfMaterialDesc.clearcoat`, `clearcoatRoughness` and `sheen`, and glTF's
  `KHR_materials_clearcoat` and `KHR_materials_sheen` (their factors; their textures are logged and left out). The coat
  is a second lobe of no color, F0 .04, with its own roughness, over the sky, the picture of all around and every
  light; what it reflects is taken from what is under it. Sheen is Burley's: the fifth power of 1 - l.h. Drawn on both
  backends and traced on both. They live apart from the material record Metal's shaders read, after all of them in the
  same buffer; on the GPU only scenes that have them build the shaders that read them, and on the CPU what has neither
  goes through a copy of the lighting without them.
- **A sky from the sun**: `artfSceneSetSky`. Air (Rayleigh), haze (Mie) and ozone scatter and take the sun's light,
  once, along each direction from a point above the ground, with Bruneton's and Hillaire's constants: blue overhead by
  day, orange and red toward a low sun, and the ground below lit through the air. It becomes the scene's light from all
  around, so both backends light by it, reflect it, show it and trace it; and the sun's color becomes what reaches the
  ground. Tens of milliseconds, on all cores.
- **Fog by height**: `artfSceneSetFogHeight`. Fog that light goes through thins above a height, and thickens below it.
- **Lights with a size**: `artfLightSetSize`. Their highlights come from the point of the ball nearest the mirror ray,
  spread as wide as it looks with their light kept (Karis's): on a smooth floor, 2.6 times as wide. Traced, each path
  aims at a point of the ball, so shadows go soft with distance.
- **Soft shadows when drawing**: `ArtfShadowDesc.soft`. What is in the way is looked for around each pixel in the map,
  and its distance gives the penumbra's width (Fernando's PCSS): as wide as the sun (`artfSceneSetSunSize`) or a light
  is, sharp where they touch. 50 reads in place of 9, for the sun and the sixteen lights.
- **Light that bounced, in reflections**: probes are read along the mirror direction too, in place of the sky's two
  colors: a metal floor by a red wall turns red.
- **What is too small to see**: `artfSceneSetCullSize`, as Unreal's minimum screen size. Shadows and rays still have it.
- **Particles**: `turbulence`, a wind that swirls (smooth noise, the same on every processor); a `ground` they bounce
  on, with `bounce` and `friction`; `spin`; and `artfEmitterBurst`.
- **Terrain's layers and what grows on it**: `artfSceneAddTerrainMaterial`, up to four surfaces, by a painted map or by
  slope and height, as a material in C; and `artfSceneScatter`, instances over the same heights, turned, sized and
  leaning at random, kept off what is steep, the same for the same seed.
- `artful-render`: `--tone`, `--auto-exposure`, `--temperature`, `--tint`, `--saturation`, `--contrast`, `--vignette`,
  `--chromatic`, `--motion-blur`, `--sun`, `--sky`, `--fog-height`, `--soft-shadows`, `--sun-size`, `--hardware`.
- The frame's block grows to 104 rows. Metal's shaders were not made again (SPIRV-Cross was not at hand): they read
  the records they did, and leave all of the above out.
- Cost, where none of it is used, measured against 0.4 on a Ryzen with a Radeon RX 9060 XT, best of several runs at
  1920 x 1080: the CPU within 2% (`spheres`, `heavy`, `city`, `outdoors` in floats with temporal antialiasing); the
  GPU within 1% on `city`, 4% on `spheres` (0.008 ms), 4% on `outdoors` in floats (0.11 ms), most of that the surface
  target being two words a pixel.
- Fixed: `artfRendererReadPixels` on Vulkan read the frame from memory the CPU does not cache (on a discrete card
  with resizable BAR, the GPU's own), through a buffer made again each time: 60 to 120 ms for a frame of 1051 x 754
  on a Radeon RX 9060 XT, where the GPU drew it in 0.2. It now reads it from cached memory, through a buffer kept
  from one frame to the next: 0.6 ms.

## 0.4.0

- **Fog that light goes through, as a grid**: froxels 16 pixels square in 64 slices along the view, built before the
  frame from the sun's shadow maps (on the CPU's workers; in a compute pass on the GPU), and read by every surface as
  it is lit, by layers too, and by what is behind the scene. Smoke and glass in front of the sky are fogged at their
  own depth, where they used to fade into it; and the march at a quarter of the pixels after the frame is gone.
  At 1920 x 1080, with the sun's shadows, it costs about what the march did: 3.5 ms on a Vega 8, 32 ms on its CPU.
- **Reflections take the place of what they hide**: where the march finds something, it replaces the light from all
  around that the surface was lit with in the mirror direction (the picture, or the sky's two colors), instead of
  going over it; where it finds nothing, that light stays, as it was.
- **Light that bounced does not go through walls**: each probe keeps how far it sees, in an 8 x 8 map of directions
  on an octahedron (the mean and the mean of the square), and a point it cannot see takes next to nothing from it
  (Chebyshev, as Majercik et al.'s probes do); probes behind a surface count less. By the wall inside a closed box
  under a bright sky: 0.1% of the light just outside, where 11% came through. It costs: 5 ms more at 1920 x 1080 on
  a Vega 8, 40 ms more on its CPU, which now reads the probes eight pixels at a time; `ArtfIndirectDesc.throughWalls`
  leaves it out, for what 0.3 cost.
- **Decals**: `artfSceneAddDecal`, `artfDecalSet`, `artfSceneRemoveDecal`. A picture laid over what is drawn, through
  a box, its color (and roughness, if given) over the surface's, fading at the box's ends; on both backends, up to 256.
- **Shadows for lights past the sixteen**: of those that ask (`artfLightSetShadow`), the 32 that matter most each
  frame (wide and near the eye, in view) get maps a quarter as wide in the atlas, in rows below the others: 48 maps, a
  spot taking one and a point light six. They go first in the list of lights past the sixteen, and both backends look
  them up as they do the sixteen's.
- **Four samples a pixel in a frame in floats, on the GPU**: `antialias` with `hdr`, as the CPU had it. The light is
  resolved; what comes after reads the first sample's depth and surface, as on the CPU.
- **Particles**: turned to the eye as each frame is drawn (`artfSceneFaceParticles`), not as the camera was when they
  were stepped. They are left out of the trees tracing and probes use, and do not draw shadow maps again by moving,
  unless the whole frame is traced. On the GPU those that follow each other are one draw; on the CPU a layer clears
  and reads only the rows of the tile it reaches.
- **glTF**: `EXT_meshopt_compression`, its three codecs (vertices, triangles, indices) and its three filters
  (octahedral, quaternion, exponential); `KHR_materials_unlit`, with `ArtfMaterialDesc.unlit`; `KHR_materials_ior`,
  as how much a dielectric reflects; `KHR_materials_transmission`, blended, and bent when traced; orthographic
  cameras, as a narrow perspective from far behind.
- **The GPU's pixel shaders hold only what a scene uses**: probes, their maps, the fog's grid. Code that is never
  run still takes registers, and fewer pixels run at once: with all of it, a plain frame of `--scene spheres` took 10%
  longer. Turning one of them on or off makes the shaders again, as new materials in C do.
- Fixed: on the CPU, a material with nothing that needed the world (no light, no highlight) left the point in the
  world unset, which decals and fog that light goes through read. The fragment shader's scene, cutout and layer
  variants of a frame in floats are now put through `spirv-val` too.
- Documents: the roadmap no longer says that Linux waits for WSL (CI runs it), and says what 0.3 and 0.4 did.

## 0.3.0

- **A frame in floats, and what a lens and film do after it**: `ArtfRendererOptions.hdr` and `artfSceneSetPost`, on
  both backends with the same arithmetic. The scene draws light (RGB9E5 in the CPU's tiles, RGBA16F on the GPU), each
  pixel's surface beside it (its normal, roughness, metal, and the share of its light that came from all around), and
  keeps the depth of what is solid; then, in order: **ambient occlusion** from eight points in the world, taken from
  that share alone; **screen-space reflections**, along a line across the frame at half its resolution; **fog that
  light goes through**, marched through the sun's shadow maps at a quarter of the pixels, with Henyey-Greenstein's
  phase; **temporal antialiasing**, the eye moved by Halton's points (`artfFrameJitter`) and each pixel followed to
  where it was; **depth of field** from a circle of confusion; **bloom** down and up a pyramid of halves; then
  exposure, tone mapping and the overlay. Without `hdr` a frame is made as before, to the byte.
- **Up to 1024 lights**: the first 16 that are on are drawn as before and may cast shadows; those past them are
  listed by the 64 x 64 squares of the frame they reach (`artfSceneLightTiles`), and the CPU's tiles and the GPU's
  pixel shader read the same lists. 256 lights at 1920 x 1080: 12 ms on a Vega 8.
- **Light that bounced, every frame**: `artfSceneSetIndirect`. A grid of probes follows a few rays each frame through
  the trees tracing uses, on the CPU's workers for both backends, and keeps a first-order spherical harmonic of what
  reaches it, mixed with what it had: light bounces once more each frame, and surfaces take it from the eight probes
  near them.
- **Particles**: `artfSceneAddEmitter`, `artfEmitterStep`. Instances of one quad, turned to the eye, fading in size,
  color and opacity as they age, as soft round puffs unless given a texture.
- **Terrain**: `artfSceneAddTerrain`, a mesh from heights, with lighter versions as any mesh's.
- `artful-render`: `--hdr` and an option for each effect, `--indirect`, `--scene lights` and `--scene outdoors`.
- **Light from all around, from a picture**: `artfSceneSetEnvironment`, `artfSceneSetEnvironmentLight`, and
  `artfImageDecodeHdr` for Radiance files. The picture becomes maps of an octahedron, one sharp and four as rough
  surfaces reflect it (GGX), and one of what a surface takes from all around. Surfaces, reflections, rays and paths
  read them, on both backends, with the same arithmetic; the picture can be the background. `artful-render
  --environment file.hdr`.
- **glTF**: `KHR_lights_punctual`, lights that follow their nodes, a directional one being the sun; cameras, with
  `artfModelCameraCount` and `artfModelCamera`; `KHR_materials_emissive_strength`; `KHR_texture_transform` and
  `texCoord`; a texture whose only image is `KHR_texture_basisu`'s, when its KTX2 holds a plain format.
- **Loading a model on all cores**: its images are decoded and its meshes made, with their levels, by all workers.
  Eight meshes of 100,000 triangles: 1.4 s, not 2.9, on four cores, and the same pixels.
- **Android**: a phone held sideways no longer shows the frame on its side. The swapchain asks the compositor to
  turn it, and is made again only when the screen turns or changes size, not on each suboptimal frame.
- **CI on GitHub**: Linux (GCC and Clang, under the validation layers and sanitizers), macOS (and Metal's shaders
  through Apple's compiler), Windows (GCC and MSVC) and Android. What it found on its first run: a function named
  `wait` in `metal.c` that macOS's headers declare too, and copies of nothing from a null pointer in `spirv.c`.
- Metal's table of bindings held 34, and its shaders name binding 48.
- `ArtfFrameData` grew by three rows: where the eye looks, for what is drawn behind the scene.
- The black hole's painted map is made on all cores: a scene with one is ready in 0.6 s, not 1.4.
- `signApk.py` is in `integrations/cascade`, beside the only script that uses it. Text files are LF, held so
  by `.gitattributes`.
- **The black hole's disc is the film's**, set against the figures of James et al., 2015: gas painted as an
  artist would, as theirs was, in strands like hair that wander as they go around, clouds torn further out,
  lanes of dust that stop more light than they give, and an edge that frays strand by strand. It is
  brightest near the hole and thins outward, so that the image under the hole shows through it. The
  fluid now only moves it: what it held was coarser than what shows, in rows of beads. The map is 2048 x
  1024. One temperature, 4500 K, toward salmon, as before.
- **The veil of a lens**, as the film's (their figure 16), is on by default, set against it by measure: fed
  only by what is far too bright, so it is wide around the hole and the corners stay dark, and nearly white.
- **A black hole at any size and any number of rays on the GPU**: a frame of more than 64 million rays (4K
  at 64 rays a pixel is 530 million) goes out in bands, each a submission of its own, so that Windows no longer
  takes the device as lost after two seconds. Before, such a frame failed with nothing said.
- **bestyle is now cascade**, and the bridge to it is no longer part of Artful: `artful/bestyle.h` is
  `artful/cascade.h`, in `integrations/cascade`, a target of its own (`artful::cascade`, or
  `find_package(artfulCascade)` when installed). `ArtfBestl*` and `artfBestl*` are `ArtfCscd*` and `artfCscd*`;
  `ARTFUL_BESTYLE_DIR` is `ARTFUL_CASCADE_DIR`, and `ARTFUL_CASCADE` finds an installed cascade. Artful's own
  headers name no other library: `ArtfRect` no longer promises to be laid out as anyone else's rectangle.
- **Shadow maps** are culled by view on the GPU, and a map that nothing changed in is not drawn: `shadowMapsDrawn`.
- **Levels of detail** go past seams and poles: a sphere of 12,000 triangles now reaches 192, and seams stay closed.
- `ARTF_FEATURE_LOD_FADE`: levels that fade into each other through a pattern of pixels.
- Meshlets culled on the GPU, for large meshes, with occlusion too (the `cones` feature).
- `ARTF_FEATURE_LAYERS_SORTED`: layers by each pixel's depth, so two that cut through each other show as they are.
- **Denoising what changes**: a light or a shadow that changes over a surface that stays no longer lingers for hundreds
  of frames. What moves keeps what it gathered, by instance and by what bones do. The CPU no longer repeats its samples from frame to frame,
  and the backends now agree.
- **Bones on the GPU**, where Vulkan draws: 2,000 characters a frame. `ArtfVertexSkin` holds eight joints. What bends
  has no limit on how far it goes.
- **Morph targets**: `artfSceneAddMorphMesh`, `artfInstanceSetMorphWeights`, and glTF's targets and weights.
- **BC7 and ASTC on the CPU**, and ASTC in all its 2D footprints.
- **Materials in C**, eight lanes at a time on the CPU, with mipmaps when drawing.
- **Residency**: what a material in C reads is kept, and so is what only a mirror shows. `artfSceneAddTextureStreamed`.
- **glTF**: sparse accessors, `artfPrepareModel` for other threads, spherical rotations between keys, and `JOINTS_1`. A
  file that requires an extension it cannot read is refused.
- A removed mesh's place is used again.
- `ARTF_FEATURE_OCCLUSION_AUTO`.
- A barrier that was missing before a frame's layers are written. A device lost stays lost, and a swapchain the driver
  refuses gives the window back.

- **A black hole**: `artfSceneSetBlackHole`. Each pixel's ray follows its real path in Kerr's metric,
  around a spinning hole, through a disc of gas with depth and out to the stars; then high dynamic range,
  bloom and tone mapping. On both backends, with the same picture, in real time. The gas is a fluid,
  simulated in the disc's plane with Kerr's orbits; a black body's colors; an eye that can move, with
  aberration; motion blur. `doppler` gives what relativity says of the disc's light, or leaves it out, as
  cinema does. The view and the disc are set against the figures of James et al., 2015.
- `artful-render --scene blackhole`, and `artful-blackhole`, a window on it (Windows).
- `ArtfFrameStats.lensMs`: what the CPU spent on a hole's light.
- **Colors on vertices**: `ArtfVertex.color`, BGRA, multiplying the material. A mesh without them stays at
  16 bytes a vertex; with them it takes 4 more. `ArtfVertex` grew, so a vertex that is not zeroed or given
  in full must now set `color`.
- **Opacity**: `artfInstanceSetOpacity`, and the alpha of vertices' colors. What shows through is drawn after
  all that is solid, the farthest first, each showing its nearest surface.
- **Alpha in the frame**: `artfSceneSetBackgroundAlpha`. Frames are premultiplied, and the overlay adds to
  their alpha. A scene that does not ask for it gives the same bytes as before.
- **Antialiasing**: `ArtfRendererOptions.antialias` and `ArtfBestlHost.antialias`: four samples a pixel on
  both backends, each surface's pixels still shaded once. On the GPU it turns occlusion off.
- `artful-render --scene layers`, `--antialias`, and `--clear`, which writes a PNG with alpha.
- **Alpha in materials**: `ArtfMaterialDesc.alphaMode`, as glTF has it: a mask, for holes, or blending, with
  `alphaCutoff` and `transparency`. Textures keep their alpha, which counts only where a material asks.
- **Two sided surfaces**: `ArtfMaterialDesc.doubleSided`.
- **Occlusion with antialiasing on the GPU**: it no longer turns off.
- **Shadows when drawing**: `artfSceneSetShadows` and `artfLightSetShadow`, on both backends: three cascades for
  the sun, a map for a spot and six for a point light, in one atlas, filtered over nine texels. Holes let
  light through. `artful-render --shadows`.
- **Physically based light**, in place of Blinn-Phong, on both backends, drawn and traced: `metallic` and
  `roughness`, with `normalMap`, `surfaceMap` and `occlusionMap`. `ArtfMaterialDesc.shininess` is gone, and
  `specular` now says how much of what a substance reflects there is: 1 is right, 0 is none. Scenes look
  different.
- **Checked**: validation layers, `spirv-val`, AddressSanitizer and MSVC ran for the first time. Render passes
  that had to match did not, by their dependencies; and a buffer was written without saying so.
- A renderer given another scene kept what it had traced of the last one, if both looked the same to it.
- **glTF**: `artfSceneLoadGltf`, with a JSON reader and PNG and JPEG decoders of its own; `artfImageDecode`,
  `artfSceneBounds`, and `artful-render --model`.
- **Handles that stop working**: a removed instance's handle does nothing, and is logged. The number a handle
  holds changed.
- **`artfSetLog`**: why a backend is not there, what a wrong handle was, what a file left out.
- **`artfRendererDefaults`, `artfMaterialDefaults`**: structs that can grow.
- **Film and air**: `artfSceneSetExposure` and `artfSceneSetFog`, on both backends, drawn and traced.
- **Spots**: `artfLightSetSpot`.
- **Traced glass and soft sun**: `ArtfMaterialDesc.ior` and `artfSceneSetSunSize`.
- **`ArtfRendererOptions.pipelineCache`** and **`ArtfFrameStats.gpuMs`**.
- **`find_package(artful)`**, with install rules.
- **Tracing**: `artfSceneSetTrace`, with `ARTF_TRACE_RAYS` and `ARTF_TRACE_PATHS`, on both backends, with no ray
  tracing hardware: trees of boxes for meshes and instances, frames that add samples to each other, and
  `ArtfFrameStats.traceSamples`. `artful-render --trace`, `--trace-samples`, `--bounces`.

## 0.2.0

- **Features that can be switched**: `artfSceneSetFeature`, and `ARTFUL_FEATURES` in the environment.
- **Levels of detail** (`lod`, on): lighter meshes made when a mesh is added, by a simplifier of its own,
  and chosen per instance on both backends.
- **Occlusion** (`occlusion`, off): two passes on both backends, with the same pixels as drawing without.
- **Cones** (`cones`, on): what the CPU already did, now optional.
- **Platforms**: run on Android (arm64, Adreno); clang works on Windows; NEON and SSE4.1 kernels, with the
  same pixels as the others; MSVC and macOS paths written. `ArtfRendererOptions.portable` became `simd`.
- **Vulkan asks for less**: `multiDrawIndirect` and partly bound descriptors are no longer needed, and the
  texture array follows what the device allows.
- **A real window on Android**: the swapchain takes the format the surface offers (Android has no BGRA),
  and the example runs there as an APK, on both backends, with touch and the app's life.
- `artfNowMs` is public.
- CI for every system, with validation layers and spirv-val: written, not run yet.
- `artful-shaders`, `tools/androidTest.sh`, `tools/androidApk.sh`.

## Bones, materials and memory

Not released yet either. This breaks the API, as it may until 1.0.

- **Skinning and clips**: `artfSceneAddSkinnedMesh`, `artfInstanceSetJoints`; glTF's skins and clips through
  `artfSceneLoadModel` and `artfAnimationSet`. Bones bend a copy of the mesh, on the CPU.
- **Materials in C**: `ArtfGraph`, `artfExprMul` and the like, `artfSceneAddGraph`, `artfMaterialSetParam`, on both
  backends, drawn and traced.
- **Compressed textures**: `artfSceneAddTextureKtx2` and `ArtfRendererOptions.compressTextures`. BC1, BC3, ETC2 and EAC
  on both backends; BC7 and ASTC on the GPU only.
- **Removing and streaming**: `artfSceneRemoveMesh`, `artfSceneRemoveTexture`, `artfSceneMemory`, `artfPrepareMesh`,
  `artfPrepareTexture`, `artfSceneSetResidency`, `artfTextureSetResidentMip`. GPU memory is reused.
- **Denoising**: `ArtfTraceDesc.denoise`, in space and in time, guided by what surfaces are, on both backends.
  `artful-render --denoise`. `ArtfTraceDesc` grew by a field.
- **A Metal backend, never run**: `ARTF_BACKEND_METAL`, for macOS and iOS with Metal 3. It compiles; nothing more is
  known. Chosen only by name. `tools/mkmetal.py` makes its shaders from Vulkan's.

## 0.1.0

The first version.

- Scenes apart from renderers: meshes packed to 16 bytes a vertex, ordered for the vertex cache and cut
  into meshlets; textures with mipmaps; instances; a sun, ambient light and 16 point lights.
- A Vulkan backend, opened at run time: compute culling and one indirect draw for a whole scene, its
  shaders built by the library.
- A software backend: tile-based and deferred, on every core, with AVX2 chosen at run time and the
  same pixels without it.
- An overlay over the scene, updated by damage rectangles.
- `artful/bestyle.h`: Artful as a presenter of bestyle.
