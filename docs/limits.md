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
- What shows through casts no shadow when drawing triangles. Light from all
  around is a sky and a ground: there are no environment maps. What comes next, and in what order, is in
  [the roadmap](roadmap.md).
  High dynamic range and bloom are the black hole's alone: what the scene draws does not go through them.
- A black hole bends only its own disc and stars: what the scene holds is drawn in front of it, straight.
  Its gas is painted, as the film's was, and moved by a two-dimensional fluid with no magnetic field; both
  are maps, which show their texels when the eye comes close.
- A removed mesh gives back GPU memory, but its handle's place in the scene stays used. Residency manages textures
  only, and keeps their mipmaps in main memory. glTF files are read on the calling thread: what can go to another
  is `artfPrepareMesh` and `artfPrepareTexture`. A scene holds up to 1024 textures on the GPU (beyond that they
  draw white).
- Making levels of detail takes time, on the calling thread: about half a second for 100,000 triangles
  on a slow desktop.
- **Metal has never been run.** It was written without a Mac: it compiles for macOS and iOS, and no more than that
  is known. So it is chosen only by name. It needs Metal 3; it leaves out occlusion, black holes and materials in
  C, and sends textures up decoded. Linux, macOS and iOS have not been built with their own compilers.
- On macOS and iOS the cascade bridge draws on the CPU, as the Metal layer is cascade's to hand over.

[What stands between this and a stable version](stability.md), and
[the study behind these choices](performanceAndPlatforms.md).

