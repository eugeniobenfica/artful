# What is not there yet

What Artful does not do yet, or does only in part. What comes next, and in what order, is in [the roadmap](roadmap.md).

- When drawing triangles, layers go by the distance of their centers, whole, unless `layers-sorted` is on: then by each
  pixel's depth, up to eight in a pixel, at 96 bytes a pixel on the GPU. A layer shows only its nearest surface. Tracing
  has neither limit.
- Antialiasing is four samples, or none, and it smooths edges only: what a texture or a highlight does
  inside a triangle is not sampled again. Upscaling (`renderScale`) needs a frame in floats, and temporal antialiasing to
  be more than reading between pixels; it gathers 16 places within a pixel, so what is very thin flickers below .5.
- When tracing, a light is a point unless it has a size or a shape (`artfLightSetSize`, `artfLightSetShape`), and those
  past the sixteen are points; glass has no Fresnel of its own, and its color (`attenuation`) is taken from paths, not
  from shadow rays: the shadow of green glass is grey. It reads textures in their largest mipmap. Black holes stay behind
  everything and do not bend what is traced. Without `denoise`, a scene that moves starts over each frame; with it,
  what was hidden starts with no history, and is soft for some frames.
- The ray tracing hardware (`ArtfTraceDesc.hardware`, and shadows and reflections from rays): what bones or targets
  move is refitted each frame it moves, and a refit moves the boxes, not the tree: it is built whole again after 60
  refits, or once its box has grown by half, and between those it traces slower the further its pose is from the one
  it was built in. A mesh that comes or changes builds its own bottom level, and no other; when one goes, the others
  stay, but what bends is built again (the trees were made over, its triangles maybe in another order). The bottom
  levels are built in the frame's commands, and wait for nothing on the CPU; but on the GPU, a frame that changes them
  first waits for the frame before it to be done. A bottom level that outgrows its room takes a new one, and the room
  it leaves is not taken again until its whole block is empty. The trees' meshes, when only poses change them, go up
  in the frame's commands too; a mesh that comes or goes still sends them all up, waiting for the GPU to be idle. It
  was run on one device, a Radeon RX 9060 XT; CI's software Vulkan has no such hardware, and so does not run it.
- Shadows from rays (`ArtfShadowDesc.traced`) start off the surface by a thousandth of its distance from the eye: what
  touches its caster by less shows a gap there, and a mesh drawn at a level of detail lighter than the trees' (which
  are the whole mesh) may shade itself where the two part by more. Smooth normals on few triangles shade themselves
  near where the light grazes, as traced shadows do. A light with a size takes four rays a pixel: without temporal
  antialiasing its penumbra is a pattern of 2 x 2 pixels. They are for the sun and the sixteen lights: those past them
  keep their maps (none at resolution 0), and so does the sun, for fog that light goes through. Particles cast none.
  On a GPU without ray tracing hardware the pixel shader walks the trees: the same picture, slower. The GPU's pixel
  shader for plain materials reads what materials of the program's own make of alpha only when shadows from rays are
  on; then it holds their code too. Metal has neither shadows nor reflections from rays: with them asked, the sun and
  the sixteen lights cast nothing there, and reflections are marched.
- Reflections from rays (`ArtfPostDesc.screen.tracedReflections`) light what they find simply: its color under the sun and
  the sixteen lights with no shadows, its own light, and light from all around (the picture, or the sky's two colors:
  not probes, not fog); what is less than half there is gone through. One ray for each 2 x 2 pixels, as the march.
- Bones run on the GPU when Vulkan draws; targets, and bones on the CPU or Metal, go per instance on the CPU. What
  bends is not culled by its parts. Its levels are made when it is added with the lod feature on (not when the feature
  is turned on later), and each instance holds a copy of each level, a third more memory than the whole alone; an
  instance chooses by its mesh at rest, not by how a pose moved it. Rays meet the whole: while tracing, an instance
  drawn at a lighter level has its whole bent too, on the CPU, as many vertices as without levels.
- ASTC is its low dynamic range profile: a block in high dynamic range decodes to magenta on the CPU. The CPU makes
  blocks into pixels when first drawn, not sample by sample, for all mipmaps. A streamed texture is plain pixels.
- A material in C picks its mipmap from neighbouring pixels along a row, on the CPU, so a surface slanted the other way
  may show more detail than it should. When tracing it reads the largest resident mipmap.
- What shows through casts no shadow in maps when drawing triangles (from rays, it lets light through as it is not
  there).
- A frame in floats (`hdr`): layers go by their centers. With four samples, what comes after reads the first sample's
  depth and surface. Reflections marched across the frame see only what it shows: what is off screen or behind
  something is not reflected, and the light from all around stays there instead. Ambient occlusion reads depth, not geometry: thin
  things and their backs are guessed. On the GPU, what bones and targets bend is followed by the vertices it had, in
  the bits of the box it had then: the frame its box grows, it moves as its instance does. Motion blur takes 12 points
  over at most 32 pixels, and what is behind a moving thing at its edge is guessed from what is around it.
- Auto exposure meters 576 points of the frame (2304 with a histogram), not all of it: a light smaller than the space
  between them can be missed, or counted for more than it covers. Its histogram has 64 bins of three eighths of a stop,
  from 1 / 4096 to 4096. Grading (lift, gamma, gain, the wheels, the look-up table, grain) is done in what a screen
  shows, the square root of light, not in a log space as Unreal's is; a look-up table is read between its points, not
  by a tetrahedron, and a .cube file's domain is taken to be 0 to 1. The flare is three ghosts of what blooms, with no
  dirt on the lens. A background pass (the black hole) keeps its own exposure, bloom and tone mapping. Fog that
  light goes through is a grid of cells 16 pixels square, in 64 slices: a shadow's edge in it is as soft as a cell is
  wide, and it reaches 4 / density along the view. It is not traced. Metal leaves all of it out.
- Lights past the first 16 are listed up to 64 for each 64 x 64 square of the frame, and are left out of tracing and of
  what the probes gather. Of those that ask for shadows, the 32 that matter most each frame (near the eye, wide, in
  view) get them, in maps a quarter as wide as the others; 48 such maps at most, six to a point light.
- Light that bounced (probes) fills a box set when it is turned on: a scene that grows needs it set again. A wall
  stops it once probes see it, as far as an 8 x 8 map of directions can tell; a wall much thinner than the probes are
  apart, with probes on both sides near it, can still let some through; `throughWalls` leaves walls out, for less
  work. What it gives reflections is as rough as the probes are: a mirror shows its color, not what it would see.
  Every probe takes its rays every frame, on either backend: none is left for a later frame, so the cost grows with the
  grid (on the GPU, a group of 64 a probe). When Vulkan draws, the GPU gathers them, from the frame's own trees or
  the hardware, and the scene's copy on the CPU is not stepped: a renderer made again (a device lost) starts them over.
  A probe that lies on a surface may see it through the ray tracing hardware (a hit at 0) where the trees do not.
- Particles are stepped on the calling thread, one instance each; they are not traced, unless the whole frame is.
  They bounce on a flat ground only, not on what the scene holds, and their wind is noise, not a fluid.
- Emitters of many (`parallel`) go after all other layers, in the order of their places, not by depth: one nearer
  that comes first hides what is behind it where it is dense. They are not traced, cast no shadow, and Metal leaves
  them out. The steps are kept for what draws, 120 of them: a renderer made later, or one that missed more, starts
  them over from the oldest kept. Born at a step, a particle's place is where the scene thinks one is free: one that
  dies later than the scene reckoned, by a rounding, is replaced. The GPU and the CPU agree to a rounding, not a bit,
  and turbulence makes that grow.
- Merged meshes far away (`artfSceneBuildHlod`) do not follow what they hold: instances moved, removed or changed
  after it are drawn as they were merged, until it is built again. They hold the opacity of what they merged while it
  is far. One merged mesh is one material: a cube of many materials makes one for each. Building them reads each
  instance's first lighter level, on all cores: 1.4 s for the 80,000 of `--scene forest`.
- Meshes' residency holds levels whole, not parts of them, and only of meshes with levels. What is not held is gone
  from memory only for a mesh that can be read again (`artfSceneAddMeshStreamed`); reading it again makes its levels
  again, on the calling thread, which takes as long as adding it did.
- A clear coat and sheen are their factors: their textures, and sheen's roughness, are not read. Sheen is Burley's
  rim, not the Charlie lobe glTF's sample viewer draws. The coat has the surface's own normal.
- A sky (`artfSceneSetSky`) scatters light once unless `multiple` asks for more, and then every order is taken to
  scatter alike, as Hillaire's table has it. It is a picture of all around, made again, in tens of milliseconds, when
  the sun or the clouds move: the sun drawn in it is as sharp as the picture's texels, and its clouds are as fine as
  they are (256 across: 1.4 degrees each); they are not drawn in front of the scene, nor in fog that light goes through.
  Their shadow is one for the whole scene, what they take of the sun over the eye, not a map over the ground. Below the
  horizon the ground is lit by the sun alone. Fog by height thins fog that light goes through, not the plain fog.
- Lights' sizes count for the sixteen lights the frame holds. Soft shadows (`ArtfShadowDesc.soft`) take 50 reads a
  pixel and light, 13 with temporal antialiasing (gathered over frames, so they settle in a few); their penumbra is at most 12 texels wide, and lights past the sixteen and fog that light goes
  through keep sharp ones.
- Past glTF's core: what sheen takes from all around is a fit by eye to Charlie's lobe, not its integral, and nothing
  makes up for the light sheen takes from under it. The coat's normal map has a scale of 1. Of
  `KHR_materials_specular`, `_iridescence`, `_anisotropy` and `_volume`, the factors are read and the textures left out
  (and logged): a film is as thick as its maximum, and drawn out highlights go one way over the whole material. A thin
  film is two beams at three wavelengths, seen along the normal, not Belcour and Barla's model; what comes from all
  around is reflected as if the surface were not drawn out. `specularColor` tints what a dielectric reflects head on,
  not at grazing angles.
- Light under the surface is a cheap model when drawing: light wraps half a radian past where the surface turns from
  it, and comes through from behind as much as what it went through leaves, by the depth in the light's shadow map;
  so only the sun and the sixteen lights with shadows bring light through, and from what is first in their way, not
  from how thick the thing itself is. Traced, a ray inside toward each light finds how thick it is, and the wrap stops
  where the surface turns from the light. Neither blurs light across the surface, as diffusion would.
- Lights' sizes, shapes and profiles count for the sixteen lights the frame holds. A shape's light reaches as far from
  each point of it as a point's would; a rectangle's highlight comes from where the mirror ray meets it, not from its
  integral over the lobe (no LTC), and its shadow map is drawn from its middle; its barn doors are not there. A
  profile is 96 x 16, by its own steps (2 degrees down, 22.5 round): a very narrow beam is softened. IES files of
  photometry type C are read as such; A and B are read as if they were C, and a lamp's tilt is left out. Soft shadows
  (`ArtfShadowDesc.soft`) take 50 reads a pixel and light; their penumbra is at most 12 texels wide, and lights past the
  sixteen and fog that light goes through keep sharp ones.
- A sky (`artfSceneSetSky`) scatters light once: twilight, which comes of light scattered more than once, is darker
  than a real one. It is a picture of all around, so its sun is not a disc in it, and it is made again, in tens of
  milliseconds, when the sun moves. Fog by height thins fog that light goes through, not the plain fog.
- What is too small to see (`artfSceneSetCullSize`) goes by an instance's bounding ball: a long thin thing seen end on
  counts as large.
- A terrain's layers are a material in C, with one set of texture coordinates; what is scattered over it is placed
  once, and does not follow if the heights change.
- On the GPU, turning probes, their walls, fog that light goes through, soft shadows, shadows from rays, a view, the
  first material past glTF's core, the first light with a shape or a profile, temporal antialiasing or motion blur on or
  off after the first frame, adding a material in C, or starting to trace makes what they need on a thread of its own
  (the scene's pixel shaders, its passes, the tracing shader, reflections from rays): frames go on with what they had
  for as long as making it takes, a few frames, and `artfRendererWarm` waits for it. Until then a frame is drawn without
  the feature (or still with it, when it was turned off): shadows from rays are the maps' (with no maps, there are
  none), temporal antialiasing and motion blur are off, tracing is rasterized, and reflections from rays are the
  screen's. A new material in C is drawn by its material's own values (its color, texture, roughness, metal and
  emission), as without its graph, in the tracing shader too. The frame that takes new passes (temporal antialiasing or
  motion blur) waits for the GPU, to make its targets again. A scene other than the one whose materials in C the shaders
  were made for has its own made before its frame, which waits for the compiler.
- Decals are tested against each drawn pixel, all of those in view: dozens are cheap, hundreds cost. They go on what is
  solid, not on what blends, and read one mipmap, chosen for the box's distance.
- Light from all around, from a picture, is reflected as if the eye looked along each surface's normal (the usual
  approximation): at grazing angles a rough surface's reflection is less stretched than it should be. Its maps are
  512 texels a side at most, so the picture shown behind the scene is soft at a wide view's center. Metal leaves it
  out. Paths read the sharp map where they leave the scene: a small, very bright sun in the picture makes fireflies,
  as it would in any path tracer that does not aim at it.
- A background pass is light behind the scene, and only that: what the scene holds is drawn in front of it, and is
  not bent, lit or reflected by it; it fills the frame (alpha 1), its bloom and tone curve are its own, and Metal
  draws none. The black hole (extensions/blackhole) is one: it bends only its own disc and stars.
  Its gas is painted, as the film's was, and moved by a two-dimensional fluid with no magnetic field; both
  are maps, which show their texels when the eye comes close.
- A removed mesh gives back GPU memory, but its handle's place in the scene stays used. Residency keeps textures'
  mipmaps in main memory. A glTF file's JSON is read on the calling thread, and its images and meshes made on
  all cores; all of it can go to another thread with `artfPrepareModel`. A scene holds up to 1024 textures on the GPU (beyond that they
  draw white).
- Making levels of detail takes time, on the calling thread: about half a second for 100,000 triangles
  on a slow desktop.
- glTF: Draco compression and Basis Universal's formats are not read. `EXT_meshopt_compression` is decoded as its
  specification has it, and checked against streams made by an encoder in the tests, not yet against meshoptimizer's
  own files. An orthographic camera's width goes by the frame's shape, not by its xmag. A file's lights end at a
  radius, as Artful's do, and are not the square law: as bright as it at a quarter of their reach. Each material has one set of texture coordinates, moved by the transform of its first
  texture. Transmission is drawn as blending, and bends light only when traced.
- Views (`artfRendererSetView`) show what is drawn as triangles: tracing and a background pass are left out while one is
  asked, and so is everything after the frame. Overdraw counts each triangle that covers a pixel, holes and what is
  hidden included, without occlusion; the GPU and the CPU round slivers in their own ways, so in a crowd of small
  things (`city`) 2% of pixels differ by a step. The meshlets view needs geometry shaders on Vulkan (`PrimitiveId`), and
  draws whole meshes while it is on; without them it shows instances. Light complexity counts the lights whose reach a
  point is in, not their cones or shadows. Motion is what instances and the eye did, not what bones bend. Metal shows
  only the picture. On the GPU a view asked for after the first frame shows a few frames later, once its shaders are
  made (`artfRendererWarm` waits for them).
- What passes take on the GPU is between timestamps at the bottom of the pipe: where a GPU runs passes side by side,
  one's time can hold part of another's. They are of the last frame the GPU finished, two behind.
- An orthographic camera (`ArtfCamera.orthoHeight`) sees the picture of all around, behind the scene, along one
  direction: all of it one color. Its depth is even from near to far, and far is just past the scene's bounds unless
  given (`far_`): what is past it is not drawn, a scene that spans thousands of units spends a float's precision on
  all of them, and finding those bounds goes over every instance each frame. A black hole is still seen through a
  perspective. Its shadows' cascades are boxes as wide as the view: a wide view spreads the sun's texels thin.
- Picking (`artfScenePick`, `artfSceneRaycast`) goes by the trees tracing makes: the first pick makes them for all
  the scene's meshes, as long as levels of detail take, and it is the full mesh, not the level drawn. A surface
  stops a pick when at least half of it is there, so glass more transparent than not is gone through. A material in
  C's normals do not count: the normal is the triangle's. The triangle is in the order Artful keeps the mesh in.
- **Metal has never been run**, and is not built unless asked for (`-DARTFUL_METAL=ON`, Apple only). It was written
  without a Mac: CI builds it on macOS and puts its shaders through Apple's compiler, and no more than that is known.
  So it is chosen only by name, and needs Metal 3. Its shaders (`src/metal/shaders.inc`) are seventeen of Vulkan's,
  translated ahead of time by `tools/mkmetal.py`: the plain variants only, made again from today's SPIR-V (after the
  shading model was written once), by artful-shaders built with clang and SPIRV-Cross 1.4.363 as CI has them, with the
  same bindings as before; on Windows, so Apple's compiler has not yet seen this version. Its C side is still 0.4's. So it reads the frame
  block whole (`ArtfFrameData`, filled by the code both other backends use), but binds nothing that came after 0.4,
  and leaves out: a frame in floats and all that comes after it (bloom, ambient occlusion, reflections, temporal
  antialiasing, upscaling, depth of field, motion blur, the look), fog that light goes through, light from a picture
  of all around and the sky, probes, lights past the sixteen, lights with a shape or a profile, soft and traced
  shadows, decals, clear coats, sheen and the rest of `ArtfMaterialMore`, light under the surface, views, sorted
  layers, meshlets culled on the GPU, emitters of many, the ray tracing hardware, a background pass, occlusion and
  materials in C; and it sends textures up decoded. What it draws of what it has is meant to be Vulkan's picture, and
  nobody has looked. Linux and macOS are built and tested by CI, Linux on a software Vulkan; iOS has not been built
  with Apple's tools. Windows on ARM is built with clang (`aarch64-w64-mingw32`, the NEON kernels) and has never run;
  MSVC builds and tests only on CI (no MSVC here), and MSVC for ARM64 has not been built at all: it would take the
  portable kernels.
- On macOS and iOS the cascade bridge draws on the CPU, as the Metal layer is cascade's to hand over.

[What stands between this and a stable version](stability.md), and
[the study behind these choices](performanceAndPlatforms.md).

## In 2D (`artful-2d`)

- Through Vulkan, a window's images are copied into, not drawn into: it takes a swapchain whose images take a copy, of
  BGRA or RGBA bytes (most do). A frame shown is not read back unless asked (`artfRenderer2DReadPixels`), which waits
  for the GPU. Two frames at most are on their way. A texture that changes goes up alone, where it was if it still fits;
  what textures leave behind is taken back only when they are all laid out again (more than half of the room left
  behind, or none left). The scene brought into the frame and the lights' shadow maps are made on the CPU each frame.
  No Metal. On the CPU its pixels are made one at a time, not eight at a time as the 3D kernels' are, which is where
  most of its time goes.
- The two backends agree within a step of a byte, not to the byte: the GPU's sines, square roots and divisions are its
  own, and a polygon's coverage is exact across a pixel only while no more than two of its edges cross it on a row.
- gi sees what is on the screen: what glows off it gives no light, and a wall off it stops none. A ray that leaves the
  screen finds nothing. Light that bounces is the last frame's, so it trails what moves by a frame. Its cascades blend
  the ones above with no correction for what is between probes: near thin walls, a little light leaks, and there is
  some ringing around what glows strongly. What glows also stops gi's rays, as a wall does.
- Shadows come from outlines: a sprite's rectangle (round corners left out), ellipse, polygon or line, not from the
  alpha of its picture; occluders for anything else. A spot's map is as round as a point's. Walls cast no shadow on
  themselves, and a wall's pixels light only as their normal map says.
- No text, no gradients, no strokes around shapes, no blend modes but over: a shape's outline is a line or a polygon
  of its own.
