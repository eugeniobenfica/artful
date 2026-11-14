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
- The ray tracing hardware (`ArtfTraceDesc.hardware`) builds every mesh's bottom level again whenever meshes change,
  bones included: what bones move each frame pays that each frame, where a refit would do. A mesh's first triangle
  goes in 24 bits, so a scene traced this way holds up to 16 million triangles. It was run on one device, a Radeon
  RX 9060 XT; CI's software Vulkan has no such hardware, and so does not run it.
- Bones run on the GPU when Vulkan draws; targets, and bones on the CPU or Metal, go per instance on the CPU. What
  bends is not culled by its parts, and has no levels of detail.
- ASTC is its low dynamic range profile: a block in high dynamic range decodes to magenta on the CPU. The CPU makes
  blocks into pixels when first drawn, not sample by sample, for all mipmaps. A streamed texture is plain pixels.
- A material in C picks its mipmap from neighbouring pixels along a row, on the CPU, so a surface slanted the other way
  may show more detail than it should. When tracing it reads the largest resident mipmap.
- What shows through casts no shadow when drawing triangles.
- A frame in floats (`hdr`): layers go by their centers. With four samples, what comes after reads the first sample's
  depth and surface. Reflections see only what the frame shows: what is off screen or behind something is not
  reflected, and the light from all around stays there instead. Ambient occlusion reads depth, not geometry: thin
  things and their backs are guessed. Temporal antialiasing and motion blur follow instances, not what bones bend,
  which leaves a trail where its history does not fit. Motion blur takes eight points over at most 32 pixels, and
  spreads what is behind a moving thing over its edge, as it spreads the thing.
- Auto exposure meters 576 points of the frame (2304 with a histogram), not all of it: a light smaller than the space
  between them can be missed, or counted for more than it covers. Its histogram has 64 bins of three eighths of a stop,
  from 1 / 4096 to 4096. Grading (lift, gamma, gain, the wheels, the look-up table, grain) is done in what a screen
  shows, the square root of light, not in a log space as Unreal's is; a look-up table is read between its points, not
  by a tetrahedron, and a .cube file's domain is taken to be 0 to 1. The flare is three ghosts of what blooms, with no
  dirt on the lens. The black hole keeps its own exposure and tone mapping. Fog that
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
- A sky (`artfSceneSetSky`) scatters light once unless `multiple` asks for more, and then every order is taken to
  scatter alike, as Hillaire's table has it. It is a picture of all around, made again, in tens of milliseconds, when
  the sun or the clouds move: the sun drawn in it is as sharp as the picture's texels, and its clouds are as fine as
  they are (256 across: 1.4 degrees each); they are not drawn in front of the scene, nor in fog that light goes through.
  Their shadow is one for the whole scene, what they take of the sun over the eye, not a map over the ground. Below the
  horizon the ground is lit by the sun alone. Fog by height thins fog that light goes through, not the plain fog.
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
- Views (`artfRendererSetView`) show what is drawn as triangles: tracing and a black hole are left out while one is
  asked, and so is everything after the frame. Overdraw counts each triangle that covers a pixel, holes and what is
  hidden included, without occlusion; the GPU and the CPU round slivers in their own ways, so in a crowd of small
  things (`city`) 2% of pixels differ by a step. The meshlets view needs geometry shaders on Vulkan (`PrimitiveId`), and
  draws whole meshes while it is on; without them it shows instances. Light complexity counts the lights whose reach a
  point is in, not their cones or shadows. Motion is what instances and the eye did, not what bones bend. Metal shows
  only the picture. Making a view's shaders, on the GPU, takes that frame a moment.
- What passes take on the GPU is between timestamps at the bottom of the pipe: where a GPU runs passes side by side,
  one's time can hold part of another's. They are of the last frame the GPU finished, two behind.
- Metal leaves out everything since 0.4: its shaders were not made again, and read the records they did.
- **Metal has never been run.** It was written without a Mac: CI builds it on macOS and puts its shaders through
  Apple's compiler, and no more than that is known. So it is chosen only by name. It needs Metal 3; it leaves out
  occlusion, black holes, materials in C and light from a picture, and sends textures up decoded. Linux and macOS are
  built and tested by CI, Linux on a software Vulkan; iOS has not been built with Apple's tools.
- On macOS and iOS the cascade bridge draws on the CPU, as the Metal layer is cascade's to hand over.

[What stands between this and a stable version](stability.md), and
[the study behind these choices](performanceAndPlatforms.md).

