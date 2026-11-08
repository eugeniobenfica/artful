# Faster, and everywhere: a study

Whether Artful can be faster and run on every platform, and how. Everything here was measured on a
temporary copy of version 0.1.0, on a Ryzen 3 3200G with its Vega 8, at 1920 x 1080 unless it says
otherwise. Version 0.2.0 has since done steps 1, 3, 4 and 5 of the plan below, and run on a phone:
[what stands now](stability.md) says where that left things.

In short: **yes to both, but not by tuning.** Small optimizations are exhausted, each worth under 4%.
What is left is drawing fewer triangles, worth 2 to 3 times. The CPU backend already reaches nearly
every platform in principle; what is missing is running it there, and a few small changes.

## What limits it today

| | Measured | So |
| --- | --- | --- |
| CPU, threads | 3.8 to 4.3 times faster on 4 cores than on 1 | It is limited by computing, not by memory. |
| CPU, `spheres` | 85% of the frame is shading; textures are 29% | Full-screen pixels are the cost. |
| CPU, `heavy` | Geometry and rasterizing 15 ms, triangle setup 4, walking and interpolating pixels 14, of 33 | Per-triangle work is the cost, at two or three pixels a triangle. |
| GPU, `heavy` | 1.9 ms at 480 x 270, 2.5 ms at 1920 x 1080 | The resolution barely matters. |
| GPU, 90,000 instances | 8.1 ms at 480 x 270, 8.8 ms at 1920 x 1080 | The same. |
| GPU, `spheres` | 0.4 ms to 4.5 ms from 480 x 270 to 3840 x 2160 | Only here do pixels count. |

The GPU's geometry scenes run at about a billion triangles a second: close to one per clock, which is
the kind of limit a shader cannot change. (That rate is worked out from the times and the triangle
counts, not read from a counter.)

## What does not help

Tried, measured, and not worth doing:

| Change | `spheres` | `field` | `heavy` | |
| --- | ---: | ---: | ---: | --- |
| As it is | 11.9 | 30.0 | 32.6 | ms |
| Tiles of 32 | 12.4 | 30.9 | 33.4 | |
| Tiles of 128 | 11.8 | 29.9 | 33.2 | |
| No hardware gather | 11.6 | 30.5 | 34.9 | |
| Non-temporal writes | 11.4 | 29.7 | 32.3 | |
| 32-bit edges for small triangles | 12.0 | 28.7 | 32.2 | same pixels |
| FMA | 11.5 | 30.2 | 33.6 | changes the pixels |
| Next set bit in the pixel walk | 12.0 | 29.6 | 32.1 | same pixels |
| `rsqrt` by Newton, the same bits everywhere | 12.9 | 30.3 | 32.6 | slower |
| clang 18 instead of GCC 16 | 10.9 | 31.0 | 33.0 | same pixels |

Differences this small are near the noise between runs. This processor runs AVX2 as two 128-bit
halves, so a newer one may gain more from wider vectors; that was not measured.

## What does help

**Levels of detail.** Choosing lighter meshes by hand, as an upper bound on what a real choice would
give (`heavy` with 1/9 of the triangles, far spheres in `field` with 1/4):

| Scene | Vulkan | Software |
| --- | ---: | ---: |
| `heavy` | 2.4 → 1.2 ms | 43 → 14 ms |
| `field --side 300` | 8.7 → 4.0 ms | 110 → 57 ms |

(The 43 ms of that run was slower than the 33 of the others, so the fair gain on `heavy` is 2.3 to 3
times.) It needs a mesh simplifier at load and a choice per instance in both backends. How far the picture
changes depends on the simplifier, which was not part of this test.

**Occlusion culling.** Of the triangles that reach the tiles, this many show at all: 67% in `heavy`,
48% in `field`, 28% in `field --side 300`. So in a dense scene up to 70% of that work is wasted. This
is a ceiling, for the tiles' part of the frame, not a measured gain: no occlusion culling was built.

**Culling meshlets on the GPU.** The GPU's limit is triangles, and the meshes already carry cones that
reject meshlets facing away. Not measured: the device here has no mesh shaders, and it would need a
compute pass writing indices.

**Cheaper texture sampling on the CPU**, with integer bilinear filtering: at most the 3.3 ms that
textures take of `spheres`' 12.

**Pixels of small triangles on the CPU**: the 14 ms of `heavy` above. Lighting is not it (taking it out
of the batches changes nothing): it is the walk and the scalar interpolation. Doing that eight
triangles at a time is the largest untried idea here, and the hardest.

## Platforms

Tested with what this machine has, which is the NDK's clang. There is no device, emulator, WSL, MSVC or
Mac here.

| Platform | State | What is missing |
| --- | --- | --- |
| Windows x64 | **Runs.** Both backends, GCC; the CPU backend also with clang, the same pixels. | A flag in CMake for clang, below. |
| Android arm64, armv7, x86-64 | **Builds with no warnings.** Never run. | A device or an emulator. |
| Linux x64 | Its code paths (`pthread`, `dlopen`) built, against Android's C library. Never built with glibc, never run. | WSL or a Linux machine. |
| Windows on ARM, Linux on ARM | Should build as they stand. Not tried. | A machine. |
| RISC-V | The CPU kernels build. | The rest of the library, and a machine. |
| Windows, MSVC | Not built: no compiler here. Needs C11 atomics, which need a recent MSVC and a flag, and `__cpuid` for AVX2. | Visual Studio. |
| macOS, iOS | Not built. | A Mac, and more: below. |
| Web | The CPU kernels build for WebAssembly with SIMD, and it uses it. Nothing else does yet. | See below. |
| Consoles | Not open to this project. | Their SDKs are under NDA, which also conflicts with having no dependencies. |

### What the CPU backend gives

It needs a C compiler and nothing else, so it is what makes "every platform" reachable. Two things
were checked here that this depends on:

- **Falling back works.** With Vulkan made to fail at each of seven stages of starting, Auto ended on
  the CPU every time, with no crash.
- **Pixels do not depend on the compiler or the SIMD.** GCC and clang, AVX2, 128-bit and portable code
  all gave the same bits, on this processor. No ARM processor has confirmed it yet, but the arm64 build
  has no fused operations in its kernels, which is what would have broken it.

On ARM the kernels run as the compiler vectorizes them: of their float operations about two thirds
came out as vectors. A backend of 2 x 128-bit registers written by hand, tried here in its SSE form,
took x86 from 19 to 11.6 ms on `spheres` and from 45 to 34 on `heavy`, with the same pixels. The same
shape in NEON is about 60 lines, and should give phones a similar gain, though that is an estimate
from x86.

### What Vulkan needs, and who has it

The backend asks for Vulkan 1.2 with `multiDrawIndirect`, `drawIndirectFirstInstance`, partially bound
descriptors and non-uniform texture indexing. A device without them draws on the CPU.

- **`multiDrawIndirect` does not have to be required.** Issuing one indirect draw per mesh passed all
  37,217 checks here. With 1,000 meshes it cost 0.08 ms more of CPU, and the same GPU time. This is
  the cheapest thing to do for reach: reports say older Mali GPUs lack the feature (the sources
  disagree on which), and Metal has no such call.
- **Descriptor indexing is the harder limit**: it is how one draw reaches every texture. One
  developer's list has Mali-G52 failing for this reason. Without it such devices stay on the CPU,
  unless a path with texture arrays is written.
- **Android**: its documentation says devices launching with Android 13 or later should support Vulkan
  1.3. No reliable figure was found for how many devices have each feature.

### macOS and iOS

Three options, and a Mac is needed for all of them:

1. **The CPU, through cascade's presenter**: what the bridge does today, if it builds. No new code.
2. **MoltenVK**, as cascade does: needs cascade to offer its Metal layer through `gpu.h`, and the fallback
   above, as it reportedly loops `multiDrawIndirect` on the CPU anyway. It is a third party's library,
   opened at run time; on iOS it has to ship inside the app.
3. **A Metal backend**: the only way with nothing but the system. Shaders can go as source text, which
   Metal compiles at run time, so the rule of no shader compiler holds. It is the largest item here:
   about the size of `vulkan.c`, in Objective-C or through the runtime's C calls.

### The web

The CPU backend could run on WebAssembly: its kernels already build for it, with a 128-bit backend as
for NEON. Threads need a page served with cross-origin isolation. WebGPU would be a backend of its own,
with WGSL for shaders. The sources found on which browsers support what disagree, so that needs
checking first. cascade does not target the web either, so this comes last.

## What to do, in order

| # | What | Gives | Size (estimate) |
| --- | --- | --- | --- |
| 1 | The clang flag in CMake; a draw per mesh where `multiDrawIndirect` is missing | More devices on the GPU. Both proven here. | 20 lines |
| 2 | Run what exists: `wsl --install`, a phone over `adb`, Visual Studio Build Tools, CI on GitHub's Windows, Linux and macOS runners | Turns "builds" into "runs" | Mostly setup |
| 3 | A native 128-bit backend: NEON and SSE | About 1.3 to 1.7 times on x86 without AVX2; phones should be alike | 150 lines |
| 4 | Levels of detail | 2 to 3 times on heavy scenes, both backends | 500 to 800 lines |
| 5 | Occlusion culling: a depth pyramid on the GPU, a coarse depth per tile on the CPU | Up to 3 times in dense scenes, a ceiling | 400 lines |
| 6 | The Metal layer from cascade, then MoltenVK or Metal | The GPU on Apple's systems | 20 lines to 1,500 |
| 7 | WebAssembly | The browser, on the CPU | Unknown until tried |

Steps 1 to 3 cost little and need no design. Step 4 is the one that changes what Artful can draw.

## Sources

- [Android: implementing Vulkan](https://source.android.com/docs/core/graphics/implement-vulkan)
- [gpuweb issue 1354, on multi-draw support](https://github.com/gpuweb/gpuweb/issues/1354)
- [Arm community, on Mali and multi-draw indirect](https://community.arm.com/support-forums/f/graphics-gaming-and-vr-forum/49770/do-we-need-to-repack-our-vertex-buffers-for-mali-g76-to-avoid-vk_device_lost/173461)
- [A developer's device list, with Mali-G52](https://esenthel.com/forum/showthread.php?tid=11836)
- [Apple forums, on indirect draw counts in Metal](https://developer.apple.com/forums/thread/680024)
- [Dawn, multi-draw on Metal](https://dawn.googlesource.com/dawn/+/0c44f2650310379490adaa8f6d886f546b12c0a7%5E%21)
- [MoltenVK issue 1567](https://github.com/KhronosGroup/MoltenVK/issues/1567)
- [gogpu/wgpu pull request 253, citing MoltenVK's loop](https://github.com/gogpu/wgpu/pull/253)
