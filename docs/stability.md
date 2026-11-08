# What stands between 0.2 and a stable Artful

An account of version 0.2.0: what has been proven, what has not, and what 1.0 needs. "Stable" here
means what it means for cascade: a program written for 1.x compiles against every 1.x, and what the
documents promise holds on the systems they name.

## What has been proven

Run, not assumed. `artful-tests` has 177,364 checks.

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
even a black hole's chaotic fluid, after 1,800 steps.

Occlusion's promise is held to exact equality: the same pixels with it and without, on both backends,
including the frame in which a wall goes away and what it hid must appear.

## What must be done for 1.0

Each of these could turn out to be a bug that users would meet. In the order to do them.

### 1. Run on what has not been run

| Gap | Why it matters | What it takes |
| --- | --- | --- |
| Rotating an Android device | The swapchain's transform is taken as it comes, and has not been turned. | A test on a device. |
| Linux with glibc | Never built. | WSL is installed, and needs Windows to restart. |
| macOS and iOS | Never built with Apple's tools. The Metal backend has only been compiled, by clang aimed at both, with stand-in headers: no line of it has run, and its shaders have not been through Apple's compiler. | A Mac. |
| armv7 and x86-64 Android | Built with no warnings, never run. | A device or an emulator. |

CI needs the project on GitHub, which needs a repository: there is none yet.

### 2. Check what nothing here could check

| Gap | Why it matters | What it takes |
| --- | --- | --- |
| A device really lost | Tests inject the error a driver gives (`artfVulkanSubmitsLeft`): the renderer says so, stays down and frees cleanly, and the scene goes to a new one. A driver that really lost its device may behave otherwise. | Hard to force. |
| Long runs | Nothing has run for hours, or heated a phone until it throttles. | A soak test. |

### 3. Fix what is known to be wrong

| Problem | Effect |
| --- | --- |
| **Making levels of detail takes time.** About 5.6 microseconds a triangle on the Ryzen, in `artfSceneAddMesh` and `artfSceneLoadModel`. | `artfPrepareMesh` and `artfPrepareModel` do it on any thread, and this is for who does not use them. |
| **Spinning workers.** They spin between loops, which is right on a desktop and costs battery on a phone. | Not measured. It has to be, before saying it is fine. |
| **Android's log complains on leaving.** Each time the surface goes, "disconnect: not connected": it is taken away before the swapchain is freed. | No harm seen over repeated cycles, but the order should be looked at. |
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
| Ray tracing hardware | Where a GPU has it, `VK_KHR_ray_query` would run the same paths several times as fast. It needs a second way through the shader, and a device to test it on. | Medium |
| Metal's gaps | Occlusion, black holes, materials in C, textures that stay compressed, bones on the GPU. They wait for a Mac: nothing of it can be run here. Materials need its shaders written as text, and compiled at run time. | Large |

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

1. A repository, and CI green on Linux, macOS, Windows and Android, with validation and spirv-val.
   Whatever they find is the first work: expect something.
2. Rotation on a device, and cascade's activity built by the JDK path as well as `integrations/cascade/androidApk.sh`.
3. Battery measured.
4. Freezing the API.

Step 1 is the one that decides how much of the rest there is. The first real window on Android found a
bug within a minute that 39,559 checks could not see: expect the same from validation.
