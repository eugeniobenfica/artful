# Changelog

## Unreleased

- **Light that bounced is gathered on the GPU when Vulkan draws**: a compute pass before the frame
  (`artfShaderProbes`, written with the SPIR-V builder like the rest) takes each probe's rays in a group of 64, through
  the tracer's trees or, on a device with ray queries, the top level the frame already builds (binding 64), with the
  tracer's code for what a ray meets, its shadow rays and the probes before; the same random directions as the CPU's,
  added up in the CPU's order. The CPU's step stays for the software backend, and as the reference: a new test
  (`tracing/probesGpu`) gathers a scene with walls, a closed box and a lamp both ways, with walls and through them,
  through the hardware and through the trees, and holds the GPU's probes to the CPU's (after six steps they are a few
  millionths of the brightest apart at most, how far they see a ten-millionth on average), the picture to the one drawn
  from probes sent up from the CPU, and the GPU's steps to themselves, to the bit. On a Radeon RX 9060 XT at 1920 x 1080 (`artful-render --indirect --frames 300`, the best
  of three runs), the CPU's time in a frame went from 0.26, 0.33 and 0.56 ms to 0.03, 0.03 and 0.05 (`spheres`, `heavy`,
  `outdoors`: as without probes), for a pass of 0.09 to 0.10 ms on the GPU. One new binding,
  70 (what a step writes); the probes' buffers are the renderer's, shared by both frames in flight.

- **The ray tracing hardware refits what bones move, builds only what changed, and waits for nothing** (Vulkan): what
  bones or targets move is built to allow an update, and refitted each frame it moves, from the vertices this frame's
  commands sent up; built whole again after 60 refits, staggered, or once its box has grown by half. The refit tried
  before had left shadows where nothing was: the levels were refitted without having been built to allow it, which
  Vulkan does not define and the Radeon RX 9060 XT's driver took as nothing to do (made so on purpose, it fails
  `tracing/tracedMoved`, and the new `tracing/tracedRefit`: a strip its bones carry far across a floor, against the
  CPU each frame and against levels built from nothing, then a mesh that comes and one that goes). Each mesh's bottom
  level is its own now: a mesh that comes or changes builds its own and no other, and one that goes leaves the others
  (what bends is built again, as the trees are made over). They are built in the frame's own commands, all at once,
  with no wait for the device; what they leave is destroyed once the frames that read it are done. When only poses
  change the trees, what moved goes up in the frame's commands too, from the slot's staging, in place of all of them
  with the device idle. `artful-render --scene crowd --figures n` takes fewer figures: with `--traced-shadows` and
  `ARTFUL_FEATURES=-lod`, a frame of 50 took 46 ms and takes 4.8, one of 200 took 120 and takes 15 (best of three;
  static scenes as they were). CPU frames are the same bytes.
- **The shading model written once** (internal): `src/shading/shading.inc` holds its formulas (Schlick's fifth power
  and Fresnel, the coat's, what a surface reflects head on, Trowbridge and Reitz's distribution drawn out or not, Smith's
  visibility, a light's lobe from the halfway vector, the mirror term from all around, the sky and the ground, Charlie's
  sheen and what it takes from all around, a thin film, light that wraps under the surface, Karis's nearest point of a
  ball, plain fog), written on a few operations and included three times: in floats for `trace.c` (`artfShade...`), in
  eight lanes for the kernels (`lanes...`, each instruction set) and as SPIR-V for the pixel and tracing shaders
  (`shader...`). The same bytes on the CPU on every instruction set, traced too (38 pictures compared, among them a new
  `artful-render --scene surfaces`: a coat, sheen, anisotropy, a thin film, light under the surface, a specular color,
  and lights with a size, a shape and a profile); the GPU within the tests' tolerances, every shader valid; no frame
  slower on either backend. What stays apart, and why, is in `docs/architecture.md`: `trace.c`'s own order in four
  places (kept for its bytes), and lights with a shape or a profile, which the compiler made 3% slower in the kernels'
  loop when written once. Lines: 329 removed and 271 added in `kernels.inc`, `shaders.c`, `trace.c` and
  `sceneInternal.h` (90 of those the three sets of operations), and `shading.inc`'s 264.
- **`ArtfPostDesc` by purpose** (breaks the API): its 39 fields are now in four parts, in the order they are done:
  `screen` (`ArtfScreenDesc`: what is worked out over the frame's pixels), `lens` (`ArtfLensDesc`: what a camera's
  glass and shutter do), `exposure` (`ArtfExposureDesc`: auto exposure) and `look` (`ArtfLookDesc`: the tone curve
  and grading). 0 is still off, or the default, everywhere; `{.lens.bloom = .05f, .look.toneMap = ARTF_TONE_NEUTRAL}`
  or `{.lens = {.bloom = .05f, .focus = 6}}` initialize it. `artfSceneSetExposure` stays as it was: the exposure
  without auto exposure, and its compensation with it. The same settings give the same bytes as before, on both
  backends. What moved:

  | Was | Is |
  | --- | --- |
  | `occlusion`, `occlusionRadius` | `screen.occlusion`, `screen.occlusionRadius` |
  | `reflections`, `tracedReflections` | `screen.reflections`, `screen.tracedReflections` |
  | `volumetric`, `scattering` | `screen.volumetric`, `screen.scattering` |
  | `temporal`, `sharpen` | `screen.temporal`, `screen.sharpen` |
  | `bloom`, `bloomThreshold`, `flare` | `lens.bloom`, `lens.bloomThreshold`, `lens.flare` |
  | `focus`, `aperture`, `motionBlur` | `lens.focus`, `lens.aperture`, `lens.motionBlur` |
  | `vignette`, `chromatic`, `grain` | `lens.vignette`, `lens.chromatic`, `lens.grain` |
  | `autoExposure` | `exposure.automatic` |
  | `exposureKey`, `exposureSpeed`, `exposureSpeedDown` | `exposure.key`, `exposure.speed`, `exposure.speedDown` |
  | `exposureMin`, `exposureMax` | `exposure.least`, `exposure.most` |
  | `exposureLow`, `exposureHigh` | `exposure.low`, `exposure.high` |
  | `toneMap` | `look.toneMap` |
  | `temperature`, `tint`, `saturation`, `contrast` | `look.temperature`, `look.tint`, `look.saturation`, `look.contrast` |
  | `lift`, `gamma`, `gain` | `look.lift`, `look.gamma`, `look.gain` |
  | `shadows`, `midtones`, `highlights` | `look.shadows`, `look.midtones`, `look.highlights` |
  | `lut`, `lutSize`, `lutAmount` | `look.lut`, `look.lutSize`, `look.lutAmount` |
- **The tests by area**: `tests/tests.c` was one file of 7,700 lines; it is now a file for each area under `tests/`
  (`core.c`, `backends.c`, `layers.c`, `materials.c`, `files.c`, `streaming.c`, `shadows.c`, `post.c`, `tracing.c`,
  `sky.c`, `motion.c`, `scale.c`, `views.c`, `blackhole.c`), with what they share in `tests/tests.h` and `common.c`;
  still one program, `artful-tests`, running all of them in the same order with the same 184,320 checks. Each test
  has a name, its area first (`shadows/softShadows`, `post/look/vulkan`): `artful-tests word...` runs only the tests
  whose names hold one of the words, and `artful-tests --list` names them all. The tests build and run without
  Vulkan (`-DARTFUL_VULKAN=OFF`, now in CI): what sets Vulkan's own switches is left out, and the GPU's frames that
  are compared start as zeros, so that a GPU that drew nothing is not compared.
- **The black hole is an extension** (breaks the API): `artfSceneSetBlackHole`, `ArtfBlackHoleDesc`,
  `artfBlackHoleDefaults` and `artfBlackHoleOrbitSpeed` moved from `artful/scene.h` to `artful/blackhole.h`, in a
  library of its own, `artful-blackhole` (`artful::blackhole` once installed; `extensions/blackhole`, option
  `ARTFUL_BLACKHOLE`). A program that draws one adds the header and links it; nothing else changes, and its
  pictures on the CPU are the same bytes as before (several holes, with Doppler, spin, a moving eye, several rays a
  pixel, each instruction set, a frame in floats, traced, under a scene). The library has no hole, fluid or lens code
  left. Turning a hole off now lets go of its maps, so turning it on again makes them again. The window tool keeps
  its name, `artful-blackhole`, as the target `artful-blackhole-window`.
- **A background pass**: `artfSceneSetBackgroundPass`, light behind the scene that a program makes, as the black hole
  is now made. Once a frame `frame` writes up to 512 bytes from the camera and the frame's size; on the CPU `rows`
  fills a tile's light, told which kernels the renderer runs; on Vulkan the program's compute shader writes the same
  buffer, with those bytes, its data and a stream at documented bindings. The renderer gives it a bloom and a tone
  curve, and it fills the frame under the scene, before what comes after a frame in floats. Metal draws none.
  `ArtfFrameStats.lensMs` is now what the pass took on the CPU, and the pass is named "background".
- **Metal is built only when asked**: `-DARTFUL_METAL=ON` (CI does, on macOS). It has still never run. Its shaders
  were made again from today's SPIR-V (the same bindings as before, entry by entry; checked here for form, and by
  Apple's compiler in CI), but its C side is 0.4's, so it lacks most of what came since: `docs/limits.md` says what.
  `tools/mkmetal.py --extract dir` writes what `shaders.inc` holds as `.metal` files; CI compiles both those and what
  its own SPIRV-Cross makes, and warns, without failing, when `shaders.inc` is not what it would make.
- **Two fogs, measured**: plain fog stays where it is lit; putting it through the grid of fog that light goes
  through would have taken a frame in floats from 11.3 to 19.4 ms on the CPU and from 1.03 to 1.45 on the GPU
  (`docs/architecture.md`).
- **The new features, cheaper**, with the same pictures: the same bytes on the CPU on every instruction set (a test holds
  grading, temporal antialiasing and upscaling with something moving to one hash), and the same bytes as before on the
  Radeon RX 9060 XT. Measured at 1920 x 1080, best of several runs, the CPU with AVX2 and 16 threads:
  - The look on the CPU goes through the kernels, eight pixels at a time (`gradeLanes`), and where bloom and the flare
    read bloom is worked out once for a size: over a frame in floats with none of it (4.6 ms), gamma, lift, gain and the
    wheels cost 7.2 ms more and now 0.9; the look-up table 3.8 and now 1.5; grain 1.6 and now 0.1; bloom 5.4 and now
    3.1, and the flare on top of it 4.9 and now 2.5.
  - Temporal antialiasing and upscaling on the CPU go through the kernels too (`temporalLanes`, `upscaleLanes`,
    `boxLanes`), where they can: the pass 5.8 to 2.6 ms, and upscaled from 0.67, 6.6 to 4.2; a frame 10.6 to 7.0 ms and
    9.4 to 6.9.
  - On the GPU no pass after the frame copies a buffer of the frame into another: light, the second light and the
    history change parts by a descriptor set (set 1) instead. A frame with temporal antialiasing 1.60 to 1.13 ms;
    upscaled, 1.20 to 0.76; with sharpening and motion blur too, 2.51 to 1.67.
  - Vulkan makes a new variant of the scene's pixel shaders (probes, their walls, fog's grid, a coat, soft shadows,
    shapes, a view) on a thread of its own after the first frame, and frames go on without the feature until it is
    made; old pipelines are destroyed once their frames are done, never after waiting for the device. Turning one on in
    a scene of 100 spheres took its frame 300 to 360 ms with the driver's cache cold, and 2 to 4 warm; now 0.7 (3 with
    probes, which the CPU makes). `artfRendererWarm` waits for what is being made. Shadows from rays and a program's
    materials in C are still made before their frame, which waits for the compiler but no longer for the device.
  - Shadows and reflections from rays, with instances that move: each slot has its own top level, built in the frame's
    commands, and its own copy of the trees' top and table of instances, written where the GPU reads them, with no
    wait for the device. 100 of 400 spheres moving every frame: 3.05 ms a frame, 1.5 of it on the CPU, to 1.68, 0.08 on
    the CPU; with reflections from rays too, 3.46 to 2.08.
  - A frame that uses none of these takes what it took (CPU 2.0 and 4.6 ms, GPU 0.19 and 0.71, without and with floats).

- **Views**, as Unreal's view modes: `artfRendererSetView` shows, in place of the picture, a surface's base color, its
  normal, roughness, metal, the light it has from all around, its own light, which level of detail is drawn, its
  meshlets, its instance, how many triangles cover each pixel, depth, how far each pixel moved since the frame before,
  or how many lights reach it. Both backends draw each the same: tested on a square whose values come out to the byte,
  and on a scene where the two backends are under 0.2% of pixels apart in every view, with a frame in floats too. On
  the CPU a view goes through the copy of the lighting that is called, as coats do; on the GPU the scene's pixel
  shaders are made with it while one is asked. With the picture shown, nothing changes, to the byte, and nothing was
  measured slower. `artful-render --view name`.
- **What each pass takes**: `ArtfFrameStats.passes`, a name and its milliseconds for each pass of the frame, in order.
  On Vulkan from timestamps (culling, shadows, the fog's grid, tracing, the scene's passes, occlusion, layers and each
  pass after the frame); on the CPU from the clock (culling, geometry, occlusion, shadows, tiles and each pass after
  the frame). They add up to `gpuMs` and `cpuMs`. `artful-render --frames n --passes` prints them.
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
- **Shadows and reflections from rays, while drawing**, as Unreal's hardware ray traced ones. `ArtfShadowDesc.traced`:
  the sun and the sixteen lights take their shadows from rays through tracing's trees, or the GPU's ray tracing
  hardware where it has some (ray queries in the pixel shader), in place of maps: exact, through what shows through as
  much as it is not there (`lightThrough`, the tracer's own), soft by the sun's size and the lights' with four rays a
  pixel over a pattern of 2 x 2 pixels that turns each frame with temporal antialiasing. `ArtfPostDesc.screen.tracedReflections`:
  a ray along each mirror in place of the march across the frame, so what is off the frame or behind something shows,
  lit simply (its color and what it reflects under light from all around, the sun and the sixteen lights, with no
  shadows, and its own light), straying with roughness. The same picture on both backends, and on a GPU without ray
  tracing hardware (the pixel shader and the pass walk the trees): 0.06% of pixels apart, CPU against GPU, and 0.00%
  through the hardware against the trees. The trees and the top level are kept up to date while drawing as instances
  move, without particles. Off by default: a scene that asks for neither builds nothing and draws the same bytes.
  `artful-render --traced-shadows --traced-reflections`.
- **The ray tracing hardware**, better: a mesh that bones or targets move has its bottom level built again alone, each
  frame it moves, where every mesh's was before (the hardware's refit was tried, and left shadows of what was not
  there: see `docs/limits.md`); a mesh's first triangle comes from the table of instances, not a custom index of 24
  bits, so a traced scene is no longer held to 16 million triangles; and fronts are counter-clockwise as the hardware has
  them with no flag. The flag the top level had turned them, so eye rays saw backs: `testTraceHardware` did not see it,
  as the samples it compared were gathered before the hardware was asked for, and now that changes the picture's key.
- **Many as one, far away** (Unreal's HLOD): `artfSceneBuildHlod` gathers the instances that stay into cubes of the
  world, merges each cube's of a material into one mesh (from each one's first lighter level, through its transform),
  makes it lighter with `simplify.c` on all cores, and draws it in their place past a distance: by default where what
  that moved stays under `artfSceneSetLodError`'s pixels, as levels are chosen. `artfSceneAddHlod` does it for
  instances given, `artfSceneClearHlod` undoes it, `artfSceneHlodShown` counts. The choice is made once a frame,
  before any backend draws, as opacities: so both draw the same, shadows follow, and near, the frame is the same bytes
  as without it. `--scene forest --hlod` (80,001 instances): 392 merged meshes in 1.4 s, and the CPU draws 31,000
  instances where it drew 62,000, 15.5 ms a frame at 1920 x 1080 where it took 19.1 (Vulkan: 0.73, 0.75).
- **Levels of detail for what bends**: a skinned or morphed mesh of 128 triangles or more gets three lighter versions,
  as other meshes do, each with the joints, weights and targets of the vertices it keeps (a collapse keeps the vertex
  it moves onto; one between vertices that bones or targets move apart costs more, so they go last). Each instance has
  a copy of each, takes the one its distance calls for, and only that one is bent: on the GPU by the `skin` shader, on
  the CPU by `artfMeshDeform`. A test holds a level's vertices to within 0.003 of where their bones put them, and the
  backends to the same picture. `--scene crowd` (2,500 figures bent every frame), at 1920 x 1080: 20 ms a frame on the
  CPU where it took 107, 0.41 ms on Vulkan where it took 0.76.
- **Emitters of many** (Niagara's GPU emitters): `ArtfEmitterDesc.parallel`. Particles are a buffer, not instances:
  the scene says where each step's births go (free places, one after another, around the buffer) and keeps the steps;
  where it is drawn, each particle is moved alone, by a compute shader (`swarm`) when Vulkan draws and on the CPU's
  workers otherwise, with the same arithmetic, random numbers from a hash of each birth's number, and the same
  features: gravity, drag, turbulence, the ground, spin, bursts, color, size and opacity over a life. Then each is an
  instance of the quad facing the eye, written by the GPU itself: one draw for all of them, after the other layers.
  A test holds the backends to 0.06% of pixels apart, and draws 100,000 at once on both. `--particles 200000` at
  1920 x 1080: 0.72 ms a frame on Vulkan, 40 on the CPU; a million, 2.3 ms on Vulkan.
  The small emitters are as they were.
- **Meshes that come and go**: `ArtfResidencyDesc.meshes` and `meshBudget`, `artfMeshSetResidentLevel`,
  `artfSceneAddMeshStreamed`. As textures' mipmaps: of each mesh with levels, those its instances need at their
  distance (with room to come nearer), within a budget, are held; the lightest always is. A level not held leaves the
  GPU, and memory too for a mesh the program can read again, which is read and made again, the same bytes, when it is
  wanted; both backends draw the next level held in its place, and shadows too. Traced, all is held.
- `artful-render`: `--scene forest`, `--hlod [cell]`, `--scene crowd` (2,500 figures that bend), `--particles n`,
  `--stream-meshes [MB]`.
- **Upscaling**: `ArtfRendererOptions.renderScale`. With a frame in floats the scene is drawn at that share of the
  frame's size (.5 to 1) and made whole after it, as Unreal's TSR and FSR 2 do: each frame's samples land elsewhere
  within their pixels, and a pixel of the frame takes, from those of this frame within a pixel of its center, as much as
  their nearness gives, over what it had, followed to where it was and kept within what the nearest sample's
  neighbours show. Without temporal antialiasing it is read between pixels. Depth of field, motion blur, bloom, the
  look, the overlay and what is read back are the frame's size. Both backends, the same picture (0.07% of pixels
  apart). At 1920 x 1080 with temporal antialiasing, occlusion and reflections: `city` 2.9 ms on the GPU (Radeon RX
  9060 XT) at the whole size, 2.0 at .75, 1.5 at .5; on the CPU (16 threads) 70, 50 and 30 ms. `ArtfPostDesc.screen.sharpen`
  brings edges back after it, or after temporal antialiasing, within each pixel's neighbours.
- **What bones and targets bend is followed by its vertices**: on the CPU each instance that bends keeps where its
  vertices were (`ArtfMotion`), and its pixels where they were a frame ago; on the GPU what the vertex buffer held is
  copied before poses move it, and the vertex shader takes it through the instance and the view as they were. A pixel's
  second word is then where it was (`ARTF_MOTION_VECTOR`). Temporal antialiasing on a striped strip that bones bend:
  0.15% of pixels by more than 48 from it settled still, against 0.72%; and motion blur blurs it.
- **Motion blur with depth**: McGuire et al.'s reconstruction (2012) in place of eight points along each pixel's way:
  velocities, the largest of each tile of 16 and of its neighbours, and 12 points weighed by which is in front and which
  moves over which. A still ball before a moving wall stays sharp and is not smeared over it (3.6 of 255 around it from
  the wall alone, against 102 before).
- **The surface target is two words only when motion is followed** (temporal antialiasing or motion blur); else one.
  Its passes and pipelines are made again only when that changes. In floats with neither, a frame of soft shadows went
  from 0.77-0.88 to 0.72 ms on the GPU.
- **Cheaper soft shadows with temporal antialiasing**: 2 x 2 reads to find what is in the way and 3 x 3 to filter, on
  few rows of the map, moved within their squares by a 4 x 4 pattern from pixel to pixel and frame to frame, which the
  frames gather (as close to all the reads as temporal antialiasing itself is); without it the 50 reads, the same every
  frame. What they cost over sharp shadows: 0.27 to 0.12 ms on the GPU, 8 to 4 ms on the CPU, in a scene of them.
- What is off is the same bytes as before (a test holds a hash made before these came). The frame's block grows to 178
  rows (`output`: the frame's size, sharpening, the frame's number); bindings 68 (what the vertices were) and 69 (motion
  blur's velocities); the instances' motion is four rows each. `artful-render`: `--render-scale`, `--sharpen`.

- **The ray tracing hardware**: `ArtfTraceDesc.hardware`. Where the GPU has acceleration structures and ray queries
  (`VK_KHR_acceleration_structure`, `VK_KHR_ray_query`), a ray goes through them in place of the trees the compute
  shader walks: a bottom level for each mesh traced, from the same triangles, and the top over the traced instances,
  in their order, so that all that follows a hit is as it was. The picture is the same to the pixel: on a Radeon
  RX 9060 XT, rays and paths, with what has one side and two, and after an instance moved, 0.00% of the pixels apart.
  `city` (40,201 instances) traced at 1280 x 720 went from 7.1 ms a frame to 1.1. Elsewhere, and on the CPU, it is as
  without (`ARTFUL_NO_RAY_QUERY`: as a device without).
- **The look**, in `ArtfPostDesc`, last before the frame is bytes, on both backends: **auto exposure**
  (`exposure.automatic`), the frame metered on 32 x 18 points in stops (log2 by bits and a polynomial, the same everywhere),
  the middle weighed twice the corners, brought to a key at a speed, within bounds, in a pass of its own on the GPU;
  a choice of **tone curve** (`look.toneMap`: ACES's fit as before, Khronos's PBR Neutral, Reinhard's, none); **white
  balance** (`look.temperature`, `look.tint`), **saturation**, **contrast**, a **vignette** and **chromatic aberration**. With
  none of it asked, a frame is the same bytes as before.
- **More of the look**, in `ArtfPostDesc`, on both backends with the same arithmetic: auto exposure by a **histogram**
  (`exposure.low`, `exposure.high`: as Unreal's, the darkest and brightest shares left out; 64 x 36 points into 64 bins
  of whole numbers, which the GPU's 64 invocations add to at once and the CPU in turn, to the same sums), with a speed
  of its own **down** (`exposure.speedDown`); **lift, gamma and gain**, and **color wheels** for the shadows, midtones and
  highlights; a **3D look-up table** (`look.lut`, `look.lutSize` from 2 to 65, `look.lutAmount`; `artfLutDecodeCube` reads .cube
  files), between its points, after the tone curve, in bloom's buffer past auto exposure's state on the GPU; **film
  grain** (`lens.grain`), a hash of the pixel and the frame's number; and a **lens flare** (`lens.flare`): what blooms, again as
  three ghosts mirrored across the middle. A small bright lamp that moves the mean's exposure by 3.7 of 255 moves the
  histogram's by none; the GPU and the CPU are 0.00% of pixels apart with all of it. With none of it asked, a frame is
  the same bytes as before (checked against 0.4's build, both backends).
- **Motion followed**: each renderer keeps how each instance moved since the frame before (`artfMotionStep`, by
  handle), and each pixel which instance it shows (on the GPU in the surface target, now two words a pixel). Temporal
  antialiasing follows a surface to where it was, so what moves no longer leaves a trail (a sphere going sideways
  under it: 0.54 of 255 from it drawn still, on average); and **motion blur** (`ArtfPostDesc.lens.motionBlur`, the share of
  a frame the shutter is open) spreads it along the way it went, at most 32 pixels.
- **Clear coat and sheen**: `ArtfMaterialDesc.clearcoat`, `clearcoatRoughness`, `sheen` and `sheenRoughness`, their
  textures (`clearcoatMap`, `clearcoatRoughnessMap`, `clearcoatNormalMap`, `sheenMap`, `sheenRoughnessMap`), and glTF's
  `KHR_materials_clearcoat` and `KHR_materials_sheen` with theirs. The coat is a second lobe of no color, F0 .04, with
  its own roughness and normal, over the sky, the picture of all around and every light; what it reflects is taken from
  what is under it. Sheen is Charlie's (Estevez and Kulla), with Neubelt's visibility, its power by `artfExp2Fast` and
  `artfLog2Fast` on both backends. Drawn on both backends and traced on both. They live apart from the material record
  Metal's shaders read, after all of them in the same buffer; on the GPU only scenes that have them build the shaders
  that read them, and on the CPU what has neither goes through a copy of the lighting without them.
- **More of glTF's materials**, drawn and traced on both backends: `specularColor` (`KHR_materials_specular`),
  highlights drawn out along a direction (`anisotropy`, `anisotropyRotation`: `KHR_materials_anisotropy`, Trowbridge
  and Reitz's with two alphas), a thin film's colors (`iridescence`, `iridescenceIor`, `iridescenceThickness`:
  `KHR_materials_iridescence`, two beams at three wavelengths), and, traced, what the inside of glass takes of each color
  (`attenuation`, `attenuationDistance`: `KHR_materials_volume`). Their factors; their own textures are logged and left
  out.
- **Light under the surface**, for skin, wax and marble: `ArtfMaterialDesc.subsurface` and `subsurfaceRadius`. Drawn,
  light wraps past where the surface turns from it, and comes through thin parts from behind, by how far it went inside
  past what the light's shadow map met first: a thin plate with the sun behind it shows 146 of 255 where it showed 26.
  Traced, a ray inside toward each light finds how thick the thing is.
- **Lights with a shape**: `artfLightSetShape`, as Unreal's rect lights and source length. A tube, or a rectangle that
  lights one side; diffuse light from the nearest point of it, highlights from the point nearest the mirror ray, made
  as long and wide as the shape looks with their light kept (Karis's): on a smooth floor a tube's highlight is 3.5 times
  as wide as a point's. Traced, paths aim at points of the shape, so shadows go soft. **Profiles**: `artfIesDecode` reads
  an IES file (LM-63, its symmetries, tilted lamps), and `artfLightSetProfile` gives a light that table, by the angle
  from its direction and round it, drawn and traced. For the sixteen lights a frame holds. The frame's block grows to
  168 rows; the GPU's profiles go in the lights' buffer.
- **A sky from the sun**: `artfSceneSetSky`. Air (Rayleigh), haze (Mie) and ozone scatter and take the sun's light,
  once, along each direction from a point above the ground, with Bruneton's and Hillaire's constants: blue overhead by
  day, orange and red toward a low sun, and the ground below lit through the air. It becomes the scene's light from all
  around, so both backends light by it, reflect it, show it and trace it; and the sun's color becomes what reaches the
  ground. Tens of milliseconds, on all cores.
- **The sky, closer to Unreal's Sky Atmosphere**: `ArtfSkyDesc.multiple`, light scattered more than once (Hillaire's
  2020 table: what a point takes from all around, as if every order scattered alike), so that the horizon and twilight
  are as bright as they are (in twilight, the sky's blue 1.7 times what scattering once gave, and the earth's shadow
  shows); what the air leaves of the sun read from a table by Bruneton's mapping, made once per sky (the sun's color
  from it within 0.3% of a march of 20,000 steps, where the 8 steps of light scattered once were 2 to 5% too bright);
  and `sunDisc`, the sun drawn, darker toward its edge, at ten times its light as it reaches the eye: enough to bloom,
  little enough that a traced path meeting it is no firefly, and none of the scene's light. Off, the sky is the same
  bytes as before.
- **Clouds**, as Unreal's Volumetric Clouds, into the sky's picture: `ArtfSkyDesc.clouds` (coverage, base, thickness,
  density, wind). A layer followed in 64 steps through tiled noise made from a hash (the same bytes everywhere), each
  step lit by the sun through the cloud above it (Beer's law, powder at the edges, three octaves of light scattered
  many times) and by the sky, seen through the air between, fading into it far off. As they are the picture's, both
  backends show, light, reflect and trace them alike. With `clouds.shadow`, the scene's sun dims by what they take of
  it over the eye. A sky with clouds, of 256 x 128: 30 to 40 ms on 16 threads.
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
  And `--sky-multiple`, `--sun-disc`, `--sky-width`, `--clouds`, `--cloud-base`, `--cloud-thickness`, `--cloud-density`,
  `--wind`, `--cloud-shadow`, `--exposure-low`, `--exposure-high`, `--exposure-speed`, `--exposure-speed-down`, `--lift`,
  `--gamma`, `--gain`, `--grade-shadows`, `--grade-midtones`, `--grade-highlights`, `--lut`, `--lut-amount`, `--grain`,
  `--flare`; it prints what the sky took.
- The frame's block grows to 111 rows. Metal's shaders were not made again (SPIRV-Cross was not at hand): they read
  the records they did, and leave all of the above out.
- The frame's block grows to 104 rows, then 168. Metal's shaders were not made again (SPIRV-Cross was not at
  hand): they read the records they did, and leave all of the above out.
- Cost, where none of it is used, measured against 0.4 on a Ryzen with a Radeon RX 9060 XT, best of several runs at
  1920 x 1080: the CPU within 2% (`spheres`, `heavy`, `city`, `outdoors` in floats with temporal antialiasing); the
  GPU within 1% on `city`, 4% on `spheres` (0.008 ms), 4% on `outdoors` in floats (0.11 ms), most of that the surface
  target being two words a pixel. Shapes, profiles and the materials past glTF's core, unused, against the commit before
  them: the same within the noise of a shared machine (CPU `spheres` 2.08 ms against 2.13 at best, `city` 22.7 against
  22.9; GPU `heavy` 0.118 against 0.119, `city` 1.31 against 1.32).
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
