# What comes next, and what was decided

Decided with the project's owner, for what stands between Artful and large projects.

## Rules for all of it

- **Both backends.** Whatever is added works on the CPU and on the GPU, with the same picture.
- **The same bytes.** On the CPU, x86 and ARM keep giving equal bytes: no `powf`, `exp` or `sinf` from the C
  library in anything drawn.
- **The API may break** until 1.0, with what changed in the changelog.
- **No dependency in the library.** Tools to build and to check are fine.

## In this order

1. **Checking.** Validation layers with synchronization, `spirv-val`, AddressSanitizer and MSVC are done; Linux waits
   for WSL, which needs Windows to restart.
2. ~~**Physically based shading.**~~ Done: it replaced Blinn-Phong, with normal maps and occlusion maps.
3. ~~**Shadows when drawing.**~~ Done: cascades for the sun, a map for each spot, a cube for each point light.
4. ~~**Skinning and glTF's clips.**~~ Done: `artfAnimationSet`, bones on the GPU where Vulkan draws, eight joints a
   vertex, morph targets. Blending clips is the program's.
5. ~~**Materials in C.**~~ Done: SPIR-V comes from the one definition, and the CPU runs it eight lanes at a time.
6. ~~**Compressed textures.**~~ Done for BC1, BC3, BC7, ETC2, EAC and ASTC on both backends. The CPU decodes blocks when
   first drawn, not as it samples.
7. ~~**Removing and streaming.**~~ Done: `artfSceneRemoveMesh`, `artfSceneRemoveTexture`, a GPU allocator that reuses
   memory, `artfPrepareMesh` and `artfPrepareModel` for other threads, residency by budget (manual, assisted or
   automatic) and `artfSceneAddTextureStreamed`, for mipmaps that stay on disk. It covers textures only.
8. ~~**Denoising.**~~ Done: `ArtfTraceDesc.denoise`, on both backends, with objects' motion and with what changes over
   surfaces that stay.
9. **Metal.** Written, and never run. It says so until a Mac is found. It leaves out occlusion, black holes,
   materials in C and textures compressed on the GPU.
