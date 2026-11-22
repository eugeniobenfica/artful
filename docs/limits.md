# What is not there yet

What Artful does not do yet, or does only in part. What comes next, and in what order, is in [the roadmap](roadmap.md).

- When drawing triangles, layers go by the distance of their centers, whole, showing only their nearest surface, unless
  `layers-sorted` is on: then by transmittance, every surface, at 20 bytes a sample on the GPU and a grid of 8 x 8
  pixel columns, 64 slices deep. Surfaces in the same slice, or in a column that other layers only partly cover, are
  mixed by weight rather than ordered; the slices go by perspective's w, so an orthographic view has one. On the CPU a
  layer has at most 16 surfaces in a sample. With a frame in floats layers go whole. Tracing has none of these limits.
- Antialiasing is four samples, or none, and it smooths edges only: what a texture or a highlight does
  inside a triangle is not sampled again; a highlight is widened by how much the normal turns over a pixel and by a
  normal map's texels (Toksvig's), which takes some of its light out where the normals turn less widely than GGX's tail
  (a fine normal map's floor: .0144 where its truth is .0213), and tracing is not widened. Highlights get back the light
  their lobe loses to a second facet as a white metal's would; the rasterizer's reflection of all around is as it was,
  which for a white metal gave all of it back already. Upscaling (`renderScale`) needs a frame in floats, and temporal antialiasing to
  be more than reading between pixels; it gathers 16 places within a pixel. What is thinner than the scene's pixels is
  held by locks (ridges seen in the last 24 frames), and changes 2 of 255 a frame at half the size; transparent things
  have no mask of their own: what they leave goes by the history's box, in two or three frames. Its history is read
  bilinearly, not by Lanczos.
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
  from 1 / 4096 to 4096. Grading (lift, gamma, gain, the wheels, the look-up table) is done in ACEScct, in AP1's
  colors, before the tone curve (or, with `gradeShown`, in what a screen shows); ACES's own transforms (its RRT and
  ODTs) are not there, only its color space and log; a look-up table is read between its points, not
  by a tetrahedron, and a .cube file's domain is taken to be 0 to 1. The flare is three ghosts of what blooms, with no
  dirt on the lens. A background pass (the black hole) keeps its own exposure, bloom and tone mapping. Fog that
  light goes through is a grid of cells 16 pixels square, in 64 slices: a shadow's edge in it is as soft as a cell is
  wide, and it reaches 4 / density along the view. It is not traced. Metal leaves all of it out.
- Lights past the first 16 are listed for each 64 x 64 square of the frame and, on the GPU, each of its 16 slices of
  depth, as many as reach it (up to 4M places in all, `ArtfFrameStats.lightsOmitted` counts any past that); they are
  left out of tracing and of
  what the probes gather. Of those that ask for shadows, the 32 that matter most each frame (near the eye, wide, in
  view) get them, in maps a quarter as wide as the others; 48 such maps at most, six to a point light.
- Light that bounced (probes) fills a box set when it is turned on: a scene that grows needs it set again. A wall
  stops it once probes see it, as far as an 8 x 8 map of directions can tell; a wall much thinner than the probes are
  apart can still let some through near a box's edges (an eighth, 20 cm from a wall 10 cm thick, probes 1.7 m apart);
  a probe inside something counts for nothing, and is followed every 8 steps; none is moved out of a wall;
  `throughWalls` leaves walls out, for less work. What it gives reflections is as rough as the probes are: a mirror shows its color, not what it would see.
  Every probe but those inside something takes its rays every frame, on either backend, so the cost grows with the
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
  they are (256 across: 1.4 degrees each). With aerial perspective and a frame in floats they are also in front of what
  is drawn, followed along a ray every 4 x 4 pixels (64 steps, from where the layer begins to what is drawn): finer than
  that, what is drawn shows between the rays' clouds as they are read between; not in an orthographic view, over
  layers, or in fog that light goes through.
  Their shadow is one for the whole scene, what they take of the sun over the eye, not a map over the ground. Below the
  horizon the ground is lit by the sun alone. Fog by height thins fog that light goes through, not the plain fog.
- Lights' sizes count for the sixteen lights the frame holds. Soft shadows (`ArtfShadowDesc.soft`) take 50 reads a
  pixel and light, 13 with temporal antialiasing (gathered over frames, so they settle in a few); their penumbra is at most 12 texels wide, and lights past the sixteen and fog that light goes
  through keep sharp ones.
- Past glTF's core: what sheen takes from all around is a fit by eye to Charlie's lobe, not its integral, and nothing
  makes up for the light sheen takes from under it. The coat's normal map has a scale of 1. `KHR_materials_volume`'s
  thickness texture is left out (the distance inside is traced). A thin film is Belcour and Barla's, as glTF's
  reference shader has it, standing for what the surface reflects head on, with Schlick's toward grazing after it: head
  on the same as the reference, which mixes the film after Schlick's; a little brighter toward grazing. What comes from
  all around is reflected as if the surface were not drawn out. `specularColor` tints what a dielectric reflects head
  on, not at grazing angles.
- Light under the surface is a cheap model when drawing: light wraps half a radian past where the surface turns from
  it, and comes through from behind as much as what it went through leaves, by the depth in the light's shadow map;
  so only the sun and the sixteen lights with shadows bring light through, and from what is first in their way, not
  from how thick the thing itself is. Traced, a ray inside toward each light finds how thick it is, and the wrap stops
  where the surface turns from the light. With a frame in floats, drawn, its diffuse light is also spread across the
  surface on the screen (separable, x then y, 17 taps each way): a profile of two Gaussians (d'Eon and Luebke's kind,
  not Burley's normalized diffusion, which Disney holds a patent on), as wide as subsurfaceRadius, the same in every
  color; the share of a pixel's light that is diffuse is measured where it is lit and kept to 1 in 255. It goes only
  between pixels that have light under their surface and lie at about the same distance; not over layers, and not when
  traced. With it, the surface target is four words a pixel, as it is with temporal antialiasing.
- Lights' sizes, shapes and profiles count for the sixteen lights the frame holds. A shape is the mean of point lights
  over it (each reaching as far as a point's would), as the tracer has it: drawn, its diffuse light is Gauss's 3 x 3
  points of it, and its highlight the lobe's integral over it (linearly transformed cosines) weighted at one point, 4%
  to 9% of a rectangle's light from the traced, more at grazing angles. Coat, sheen and what is under the surface take
  its nearest point; a drawn-out lobe is taken as round there. Its shadow map is drawn from its middle; its barn doors
  are not there. A
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
  out. Paths aim at the picture for what surfaces give back diffusely; what they reflect of it (a highlight, a
  mirror) is still found only by bouncing into it, so a small, very bright sun seen in a rough highlight is noisy.
  Light from the picture over 64 in one path is cut there, as all light of a path is.
- A background pass is light behind the scene, and only that: what the scene holds is drawn in front of it, and is
  not bent, lit or reflected by it; it fills the frame (alpha 1), its bloom and tone curve are its own, and Metal
  draws none. The black hole (extensions/blackhole) is one: it bends only its own disc and stars.
  Its gas is painted, as the film's was, and moved by a two-dimensional fluid with no magnetic field; both
  are maps, which show their texels when the eye comes close.
- A KTX2 file of Basis Universal's (`KHR_texture_basisu`) is kept as ASTC 4 x 4 blocks (UASTC) or ETC1 blocks (ETC1S
  without alpha), exactly, where it has all its mipmaps; a GPU that takes neither gets BC7 (from UASTC, about 45 dB from
  its texels) or BC1 and BC3 (from ETC1S, about 30 dB), made as it goes up, on Vulkan. Metal and WebGPU get pixels.
  ETC1S and UASTC 4 x 4 only (not UASTC HDR, nor basisu 2.x's XUASTC and XUBC7), one image (no arrays, cube maps or
  video).
- A texture is read between two of its texels as bytes, then made light: across a sharp edge between dark and light
  texels, a little darker than the mean of their light. Its mipmaps are means of light when only colors use it, of bytes
  otherwise; a KTX2 file's and a streamed texture's are as they were given.
- A removed mesh gives back GPU memory and its place in the scene; the next mesh takes the place with another
  generation in its handle; so does a removed texture's, and the old handle is refused (and said) where it is given. Residency keeps textures'
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
- A Vulkan device without non-uniform texture indexing reads at most 64 textures (what is beyond is white), each
  sample down a tree of ifs: its shaders are larger (the tracer's five times), and slower to build.
- **Metal has run only on CI's macOS runner** (an Apple Paravirtual device, 21 November 2026), and is not built unless
  asked for (`-DARTFUL_METAL=ON`, Apple only); so it is chosen only by name. There, against the CPU, 0.02 to 0.09% of
  pixels apart (`backends/metal`: textures and lights, a sun's shadows, what has holes, the overlay); no window has
  shown it. Its shaders are Vulkan's, written as Metal from the IR as it starts, and its C side gives them today's
  bindings; it draws what WebGPU draws (the plain frame) and leaves out the same, saying so: a frame in floats and all
  that comes after it, light from all around and from probes, lights with a shape or a profile, layers of
  transparency, tracing, materials in C, views, occlusion and a background pass; textures go up decoded. Of the image
  tests, only `backends/metal` runs on it.
- Linux and macOS are built and tested by CI, Linux on a software Vulkan; iOS has not been built
  with Apple's tools. Windows on ARM is built with clang (`aarch64-w64-mingw32`, the NEON kernels) and has never run;
  MSVC builds and tests only on CI (no MSVC here), and MSVC for ARM64 has not been built at all: it would take the
  portable kernels.
- WebGPU is built only when asked for (`-DARTFUL_WEBGPU=ON`) and chosen only by name. It draws the plain frame:
  textures (by constant indices, as many as a stage may have less two: 14 where the adapter allows 16; what is beyond
  is white), lights and those past the sixteen, shadow maps, decals, what has holes and the overlay. It leaves out,
  and says so: a frame in floats and all that comes after it, light from all around and from probes, lights with a
  shape or a profile, layers of transparency, tracing, materials in C, views, occlusion and a background pass; textures
  go up decoded. It needs `indirect-first-instance`. In a browser a frame is not read back (it goes to a canvas), and
  the waits for the adapter and the device take Emscripten's ASYNCIFY. It has run on wgpu-native and in Chromium, on
  one GPU; not in Safari or Firefox.
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
