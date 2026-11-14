# What is not there yet

What Artful does not do yet, or does only in part. What comes next, and in what order, is in [the roadmap](roadmap.md).

- When drawing triangles, layers go by the distance of their centers, whole, unless `layers-sorted` is on: then by each
  pixel's depth, up to eight in a pixel, at 96 bytes a pixel on the GPU. A layer shows only its nearest surface. Tracing
  has neither limit.
- Antialiasing is four samples, or none, and it smooths edges only: what a texture or a highlight does
  inside a triangle is not sampled again. There is no upscaling: the frame is drawn at its size.
- When tracing, a light is a point unless it has a size (`artfLightSetSize`), and those past the sixteen are points;
  glass has no Fresnel of its own, nor color. It reads textures in their largest mipmap. Black holes stay behind
  everything and do not bend what is traced. Without `denoise`, a scene that moves starts over each frame; with it,
  what was hidden starts with no history, and is soft for some frames.
- The ray tracing hardware (`ArtfTraceDesc.hardware`, and shadows and reflections from rays): what bones or targets
  move has its bottom level built again each frame it moves, alone, not refitted (the hardware's refit put shadows
  where nothing was on the one device it was run on); anything else that changes meshes builds every bottom level
  again, and an instance that moves builds the top level again. Each of those, and the trees sent up when they change,
  waits for the GPU to be idle. It was run on one device, a Radeon RX 9060 XT; CI's software Vulkan has no such
  hardware, and so does not run it.
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
- Reflections from rays (`ArtfPostDesc.tracedReflections`) light what they find simply: its color under the sun and
  the sixteen lights with no shadows, its own light, and light from all around (the picture, or the sky's two colors:
  not probes, not fog); what is less than half there is gone through. One ray for each 2 x 2 pixels, as the march.
- Bones run on the GPU when Vulkan draws; targets, and bones on the CPU or Metal, go per instance on the CPU. What
  bends is not culled by its parts, and has no levels of detail.
- ASTC is its low dynamic range profile: a block in high dynamic range decodes to magenta on the CPU. The CPU makes
  blocks into pixels when first drawn, not sample by sample, for all mipmaps. A streamed texture is plain pixels.
- A material in C picks its mipmap from neighbouring pixels along a row, on the CPU, so a surface slanted the other way
  may show more detail than it should. When tracing it reads the largest resident mipmap.
- What shows through casts no shadow in maps when drawing triangles (from rays, it lets light through as it is not
  there).
- A frame in floats (`hdr`): layers go by their centers. With four samples, what comes after reads the first sample's
  depth and surface. Reflections marched across the frame see only what it shows: what is off screen or behind
  something is not reflected, and the light from all around stays there instead. Ambient occlusion reads depth, not geometry: thin
  things and their backs are guessed. Temporal antialiasing and motion blur follow instances, not what bones bend,
  which leaves a trail where its history does not fit. Motion blur takes eight points over at most 32 pixels, and
  spreads what is behind a moving thing over its edge, as it spreads the thing.
- Auto exposure meters 576 points of the frame, not all of it, and has no histogram to leave out its brightest and
  darkest: a small, very bright light in the middle of a dark frame darkens it more than a camera would. The black hole keeps its own exposure and tone mapping. Fog that
  light goes through is a grid of cells 16 pixels square, in 64 slices: a shadow's edge in it is as soft as a cell is
  wide, and it reaches 4 / density along the view. It is not traced. Metal leaves all of it out.
- Lights past the first 16 are listed up to 64 for each 64 x 64 square of the frame, and are left out of tracing and of
  what the probes gather. Of those that ask for shadows, the 32 that matter most each frame (near the eye, wide, in
  view) get them, in maps a quarter as wide as the others; 48 such maps at most, six to a point light.
- Light that bounced (probes) fills a box set when it is turned on: a scene that grows needs it set again. A wall
  stops it once probes see it, as far as an 8 x 8 map of directions can tell; a wall much thinner than the probes are
  apart, with probes on both sides near it, can still let some through; `throughWalls` leaves walls out, for less
  work. What it gives reflections is as rough as the probes are: a mirror shows its color, not what it would see.
- Particles are stepped on the calling thread, one instance each; they are not traced, unless the whole frame is.
  They bounce on a flat ground only, not on what the scene holds, and their wind is noise, not a fluid.
- A clear coat and sheen are their factors: their textures, and sheen's roughness, are not read. Sheen is Burley's
  rim, not the Charlie lobe glTF's sample viewer draws. The coat has the surface's own normal.
- A sky (`artfSceneSetSky`) scatters light once: twilight, which comes of light scattered more than once, is darker
  than a real one. It is a picture of all around, so its sun is not a disc in it, and it is made again, in tens of
  milliseconds, when the sun moves. Fog by height thins fog that light goes through, not the plain fog.
- Lights' sizes count for the sixteen lights the frame holds. Soft shadows (`ArtfShadowDesc.soft`) take 50 reads a
  pixel and light; their penumbra is at most 12 texels wide, and lights past the sixteen and fog that light goes
  through keep sharp ones.
- What is too small to see (`artfSceneSetCullSize`) goes by an instance's bounding ball: a long thin thing seen end on
  counts as large.
- A terrain's layers are a material in C, with one set of texture coordinates; what is scattered over it is placed
  once, and does not follow if the heights change.
- On the GPU, turning probes, their walls or fog that light goes through on or off makes the scene's pixel shaders
  again: that frame waits for them.
- Decals are tested against each drawn pixel, all of those in view: dozens are cheap, hundreds cost. They go on what is
  solid, not on what blends, and read one mipmap, chosen for the box's distance.
- Light from all around, from a picture, is reflected as if the eye looked along each surface's normal (the usual
  approximation): at grazing angles a rough surface's reflection is less stretched than it should be. Its maps are
  512 texels a side at most, so the picture shown behind the scene is soft at a wide view's center. Metal leaves it
  out. Paths read the sharp map where they leave the scene: a small, very bright sun in the picture makes fireflies,
  as it would in any path tracer that does not aim at it.
- A black hole bends only its own disc and stars: what the scene holds is drawn in front of it, straight.
  Its gas is painted, as the film's was, and moved by a two-dimensional fluid with no magnetic field; both
  are maps, which show their texels when the eye comes close.
- A removed mesh gives back GPU memory, but its handle's place in the scene stays used. Residency manages textures
  only, and keeps their mipmaps in main memory. A glTF file's JSON is read on the calling thread, and its images and meshes made on
  all cores; all of it can go to another thread with `artfPrepareModel`. A scene holds up to 1024 textures on the GPU (beyond that they
  draw white).
- Making levels of detail takes time, on the calling thread: about half a second for 100,000 triangles
  on a slow desktop.
- glTF: Draco compression and Basis Universal's formats are not read. `EXT_meshopt_compression` is decoded as its
  specification has it, and checked against streams made by an encoder in the tests, not yet against meshoptimizer's
  own files. An orthographic camera comes out as a perspective one from far behind, through a field a hundredth of a
  radian high. A file's lights end at a radius, as Artful's do, and are not the square law: as bright as it at a
  quarter of their reach. Each material has one set of texture coordinates, moved by the transform of its first
  texture. Transmission is drawn as blending, and bends light only when traced.
- Metal leaves out everything since 0.4: its shaders were not made again, and read the records they did.
- **Metal has never been run.** It was written without a Mac: CI builds it on macOS and puts its shaders through
  Apple's compiler, and no more than that is known. So it is chosen only by name. It needs Metal 3; it leaves out
  occlusion, black holes, materials in C and light from a picture, and sends textures up decoded. Linux and macOS are
  built and tested by CI, Linux on a software Vulkan; iOS has not been built with Apple's tools.
- On macOS and iOS the cascade bridge draws on the CPU, as the Metal layer is cascade's to hand over.

[What stands between this and a stable version](stability.md), and
[the study behind these choices](performanceAndPlatforms.md).

