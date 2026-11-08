# What is not there yet

What Artful does not do yet, or does only in part. What comes next, and in what order, is in [the roadmap](roadmap.md).

- When drawing triangles, layers go by the distance of their centers, whole, unless `layers-sorted` is on: then by each
  pixel's depth, up to eight in a pixel, at 96 bytes a pixel on the GPU. A layer shows only its nearest surface. Tracing
  has neither limit.
- Antialiasing is four samples, or none, and it smooths edges only: what a texture or a highlight does
  inside a triangle is not sampled again.
- Point lights are points when tracing, so only the sun has soft shadows; and glass has no Fresnel of its own,
  nor color. It reads textures in their largest mipmap. Black holes stay behind everything and do not bend what
  is traced. Without `denoise`, a scene that moves starts over each frame; with it, what was hidden starts with no
  history, and is soft for some frames. It uses compute shaders, and not the ray tracing hardware some GPUs have.
- Bones run on the GPU when Vulkan draws; targets, and bones on the CPU or Metal, go per instance on the CPU. What
  bends is not culled by its parts, and has no levels of detail.
- ASTC is its low dynamic range profile: a block in high dynamic range decodes to magenta on the CPU. The CPU makes
  blocks into pixels when first drawn, not sample by sample, for all mipmaps. A streamed texture is plain pixels.
- A material in C picks its mipmap from neighbouring pixels along a row, on the CPU, so a surface slanted the other way
  may show more detail than it should. When tracing it reads the largest resident mipmap.
- What shows through casts no shadow when drawing triangles.
- A frame in floats (`hdr`): on the GPU it has one sample a pixel, and layers go by their centers there and on the CPU.
  Reflections see only what the frame shows: what is off screen or behind something is not reflected. Ambient
  occlusion and fog read depth, not geometry: thin things and their backs are guessed. Temporal antialiasing follows
  pixels by depth and the view alone: what moves leaves a trail where its history does not fit. The black hole keeps
  its own exposure and tone mapping. Fog that light goes through is reckoned to what is solid: layers in front of the
  sky (smoke, glass) are fogged as the sky is, and fade into it. Metal leaves all of it out.
- Lights past the first 16 cast no shadow, are listed up to 64 for each 64 x 64 square of the frame, and are left out
  of tracing and of what the probes gather.
- Light that bounced (probes) leaks through walls thinner than the probes are apart, and fills a box set when it is
  turned on: a scene that grows needs it set again. It does not reach specular reflections.
- Particles are stepped on the calling thread, one instance each, and face the camera as it was when stepped. There are
  no decals.
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
- glTF: Draco and meshopt compression and Basis Universal's formats are not read, nor orthographic cameras. A file's
  lights end at a radius, as Artful's do, and are not the square law: as bright as it at a quarter of their reach.
  Each material has one set of texture coordinates, moved by the transform of its first texture.
- **Metal has never been run.** It was written without a Mac: CI builds it on macOS and puts its shaders through
  Apple's compiler, and no more than that is known. So it is chosen only by name. It needs Metal 3; it leaves out
  occlusion, black holes, materials in C and light from a picture, and sends textures up decoded. Linux and macOS are
  built and tested by CI, Linux on a software Vulkan; iOS has not been built with Apple's tools.
- On macOS and iOS the cascade bridge draws on the CPU, as the Metal layer is cascade's to hand over.

[What stands between this and a stable version](stability.md), and
[the study behind these choices](performanceAndPlatforms.md).

