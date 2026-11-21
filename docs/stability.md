# What stands between 0.4 and a stable Artful

An account of version 0.4.0: what has been proven, what has not, and what 1.0 needs. "Stable" here
means what it means for cascade: a program written for 1.x compiles against every 1.x, and what the
documents promise holds on the systems they name.

## Where it stands: tiers

What each backend is on each system, by what was run (Phase 0 of the plan made skipped tests visible, so "run" means
the test passed there, not that it returned early). **Stable**: run, its tests pass, and 1.0 will promise it.
**Best effort**: built and partly run; it should work, and what is found is fixed, but it is not promised.
**Experimental**: built, never run, or known to lack much.

| | CPU | Vulkan | Metal | 2D, CPU | 2D, Vulkan |
| --- | --- | --- | --- | --- | --- |
| Windows x64 | stable: all tests, GCC, clang, MSVC (CI) | stable: AMD Vega 8, Radeon RX 9060 XT (ray tracing hardware), windows | | stable | stable: read back and in a window |
| Android arm64 | stable: Galaxy S25 Ultra | best effort: the S25's Adreno 830, a window and going to the background; rotation not seen (nor `preRotate`, checked here only through a test hook) | | best effort: not yet run | best effort: not yet run |
| Linux x64 | stable: GCC and clang, sanitizers (CI) | best effort: Mesa's software Vulkan (lavapipe), no window, no GPU; light that bounced, gathered on the GPU, crashes lavapipe's compiled shader (reproduced on Windows, not yet isolated) | | best effort: not yet through CI | best effort: not yet through CI |
| macOS | best effort: all tests pass on Apple silicon (CI, 20 November 2026: 326,689 checks, the GPU's 95 skipped), no window | | experimental: run on CI's Apple Paravirtual device, 0.02–0.09% of pixels from the CPU (`backends/metal`); its shaders, from the IR, through Apple's compiler in CI; never in a window | best effort: not yet through CI | |
| iOS | experimental: never built with Apple's tools | | experimental | experimental | |
| Windows on ARM | experimental: built, never run | experimental | | experimental | experimental |
| Android armv7, x86-64 | experimental: built, never run | experimental | | experimental | experimental |

Ray tracing hardware has been run on one GPU only (Radeon RX 9060 XT): best effort until another vendor's.

## What has been proven

Run, not assumed. `artful-tests` has 184,977 checks.

On GitHub, CI builds and tests every change: Linux with GCC and Clang, all tests under Vulkan's validation layers
on a software Vulkan (Mesa's lavapipe), with one draw a mesh and with occlusion, then under AddressSanitizer and
UndefinedBehaviorSanitizer; macOS, all tests on the CPU, and Metal's shaders through Apple's compiler; Windows with
MSVC; Android for every ABI. Its first run found two faults no machine here could: a function of `metal.c` named
as one of macOS's, and copies of nothing from a null pointer, which UBSan stops on.

| | Windows, Ryzen 3 3200G, Vega 8 | Android 17, Galaxy S25 Ultra, Adreno 830 |
| --- | --- | --- |
| All tests, CPU and GPU | passes, GCC and clang | passes, clang |
| With one draw a mesh; with occlusion all along; with lod and cones off | passes | passes |
| AVX2, SSE4.1, NEON and portable kernels draw the same bits | passes | passes |
| Allocations all given back | 0 left | 0 left |
| Undefined behaviour (UBSan, trapping) | clean | not run |
| AddressSanitizer (clang, all kernels) | clean | not run |
| Vulkan's validation layers, all tests | clean | not run |
| Synchronization validation, with shaders' accesses, all tests | clean | not run |
| A device lost and a swapchain refused, injected | passes | not run |
| `spirv-val`, every shader | clean | |
| MSVC, all tests, the same bytes as GCC | passes | |
| A real window, with a page of cascade over the scene | passes, both backends | passes, both backends |
| Touch on the page driving the scene | not run | passes |
| Going to the background and back, repeatedly (a new surface each time) | not run | passes, both backends |
| Resizing and a lost device (simulated), in a real window | passes, both backends | not run |
| Vulkan failing after it took the surface: the CPU takes over and shows | not run | passes |
| A black hole: the shadow's edge within a pixel of `sqrt(27)` masses, at three distances | passes | passes |
| A spinning hole: both sides of the shadow within a pixel of Kerr's orbits of light, at three spins | passes | passes |
| A black hole's fluid: the same bytes however time is cut up; rings at their orbit's speed; a long run stays bounded | passes | passes |
| A black hole in a real window, both backends | passes | not run |

And across the two: **the same scene gives the same bytes on x86 and on ARM**, on the CPU, with lod off:
even a black hole's chaotic fluid, after 1,800 steps. What a scene works out once no longer asks the C library for a
sine, a cosine or a tangent (`src/core/trig.h`): MinGW's were a step off the nearest float for one float in 13,000,
which another system's would not have been.

Since the shading model was written once, on Windows x64 with a Radeon RX 9060 XT, with what CI does not run on
Windows: clang 23 (MSYS2) builds with no warnings and passes every test, and so does a build without Vulkan;
AddressSanitizer and UndefinedBehaviorSanitizer (clang, CI's flags; `tools/sanitize.sh`), every test, Vulkan's among
them, each kernel's instruction set, plainly, with one draw a mesh, with occlusion all along and with lod and cones
off: clean, and every allocation given back. LeakSanitizer does not run on Windows; the library's own count stands for
it. Windows on ARM (`aarch64-w64-mingw32`, the NEON kernels) builds with no warnings, and has not run.

Occlusion's promise is held to exact equality: the same pixels with it and without, on both backends,
including the frame in which a wall goes away and what it hid must appear.

## What must be done for 1.0

Each of these could turn out to be a bug that users would meet. In the order to do them.

### 1. Run on what has not been run

| Gap | Why it matters | What it takes |
| --- | --- | --- |
| Rotating an Android device | The swapchain used to take the screen's transform without turning the frame. It now asks for none and leaves the turning to the compositor, swapping the sides, and is made again only when the screen turns: written to the specification, not yet seen on a phone. | A test on a device. |
| Linux on a real GPU and in a real window | Built and tested by CI, on a software Vulkan, with no window. | A Linux machine. |
| macOS and iOS on a device | Built and tested by CI on macOS, on the CPU. The Metal backend compiles there, and its shaders pass Apple's compiler, but no line of it has run. | A Mac. |
| armv7 and x86-64 Android | Built with no warnings, never run. | A device or an emulator. |
| Windows on ARM | Built with clang (MSYS2's clangarm64 headers and libraries), NEON kernels and all, with no warnings; never run. MSVC for ARM64 has not been built: it would take the portable kernels. | A Snapdragon laptop. |
| MSVC, since 0.4 | Built and tested only by CI; this machine has no MSVC. Reading found nothing GCC-only unguarded, and clang's `-Wpedantic -Wvla` finds nothing, but MSVC's C is its own. | CI, or Visual Studio. |
| Metal on a real Mac | Run only on CI's paravirtual device, through `backends/metal`; never in a window, nor on a Mac's own GPU. | A Mac. |

### 2. Check what nothing here could check

| Gap | Why it matters | What it takes |
| --- | --- | --- |
| A device really lost | Tests inject the error a driver gives (`artfVulkanSubmitsLeft`): the renderer says so, stays down and frees cleanly, and the scene goes to a new one. A driver that really lost its device may behave otherwise. | Hard to force. |
| Long runs | Nothing has run for hours, or heated a phone until it throttles. | A soak test. |

### 3. Fix what is known to be wrong

| Problem | Effect |
| --- | --- |
| **Making levels of detail takes time.** About 3.6 microseconds a triangle on the Ryzen, for one mesh, in `artfSceneAddMesh`. | A model's meshes are made on all cores (`artfSceneLoadModel`: twice as fast on four), and `artfPrepareMesh` and `artfPrepareModel` do it on any thread. One large mesh is still one thread's work: simplifying is greedy, each step depending on the last. |
| **Spinning workers.** After each loop a worker spins for at most 20,000 pauses (tens of microseconds) before it sleeps, which covers the gaps inside a frame. Between frames they sleep. | Bounded, but its cost in battery is not measured. It has to be, on a phone, before saying it is fine. |
| **Android's log complains on leaving.** Each time the surface goes, "disconnect: not connected". Artful frees its swapchain before its surface, as it must; the cause is cascade's: its window says it let go of the surface (`syncSurface`, in `windowAndroid.c`) before it frees the presenter, so Android takes the surface back while Artful's swapchain still holds it. | No harm seen over repeated cycles. The fix is cascade's: free the presenter, then say so. |
| **Changing level pops**, unless `lod-fade` is on. | Bounded by `artfSceneSetLodError` (one pixel by default). In a slow dolly at four pixels the fade took the largest jump between frames from .34 to .04 of 255, on both backends. |

### 4. Decide what the API promises

1.0 freezes it, so these come first:

- **Threads.** The rule as it stands: a scene and a renderer belong to one thread at a time; `artfPrepareMesh`,
  `artfPrepareTexture` and `artfPrepareModel` run on any thread and touch no scene. A scene is not filled while it is
  being drawn.
- **Which features are on by default.** See below.

## What should be improved, but need not hold 1.0 back

**Rendering.** A stable renderer need not do everything, but some of these decide who can use it at all:

| What | Why | Size |
| --- | --- | --- |
| Ray tracing hardware | Done, as `ArtfTraceDesc.hardware`: the same picture, 6.7 times as fast on `city`. A refit for what bones move, and a run on other vendors' devices, are left. | Small |
| glTF's compressed files | `EXT_meshopt_compression` is read, and checked against an encoder in the tests: it should be checked against files meshoptimizer made. `KHR_texture_basisu`'s Basis formats need transcoders of their own (ETC1S, UASTC) and Zstandard; Draco a decoder. | Medium |
| Metal's gaps | All that came after 0.4 (it is built only with ARTFUL_METAL; see limits), occlusion, a background pass, materials in C, textures that stay compressed, bones on the GPU. They wait for a Mac: nothing of it can be run here. Materials need its shaders written as text, and compiled at run time. | Large |

**Speed**, measured in [the study](performanceAndPlatforms.md):

- The GPU culls meshlets in meshes of 512 or more, about 65,000 triangles, by view, by cone and, with occlusion, by the
  depth pyramid. On six spheres of 480,000 triangles on the Vega 8 it gained 23%, and 39% with a wall before them. Below
  that size it cost more than it saved.

## On the defaults

Measured, 1920 x 1080, milliseconds a frame. `city` is a wall before a crowd; `spheres` hides nothing.

| | Ryzen CPU | Vega 8 | S25 CPU | Adreno 830 |
| --- | ---: | ---: | ---: | ---: |
| `city`, both off | 459 | 46.9 | 195 | 15.7 |
| `city`, lod | 170 | 13.8 | 76 | 5.6 |
| `city`, occlusion | 56 | 2.7 | 24 | 0.85 |
| `city`, both | 37 | 2.5 | 16 | 0.81 |
| `heavy`, off | 38 | 2.5 | 12 | 1.5 |
| `heavy`, lod | 9.6 | 1.2 | 3.7 | 0.74 |
| `heavy`, occlusion | 43 | 2.8 | 15 | 1.6 |
| `spheres`, off | 15 | 1.5 | 4.6 | 0.79 |
| `spheres`, occlusion | 15 | 1.8 | 4.9 | 0.95 |
| 90,000 instances, off | 102 | 9.5 | 64 | 6.8 |
| 90,000 instances, both | 109 | 7.3 | 44 | 5.9 |

So **lod stays on**: it helps or does nothing. **Occlusion stays off**: it wins by up to 19 times where much
is hidden, and costs up to 30% where nothing is. `ARTF_FEATURE_OCCLUSION_AUTO` tells them apart at run time: the
renderer tries both for a few frames, every 240, and keeps what took less. It stays off by default because
trying costs those frames.

## The way to 1.0

1. ~~A repository, and CI on Linux, macOS, Windows and Android, with validation and spirv-val.~~ Done: what its
   first run found is fixed. Keep it green.
2. Rotation on a device, and cascade's activity built by the JDK path as well as `integrations/cascade/androidApk.sh`.
3. Battery measured.
4. Freezing the API.

The first real window on Android found a bug within a minute that 39,559 checks could not see, and CI's first run
two that 177,364 had not: what has not run has to be taken as broken until it has.
