# Artful under a page

cascade lets an app draw in a window and show a page over it: `CscdWindowOptions.presenter` hands
the app the surface, and `cscdViewSetTransparent` leaves the page's empty parts empty. Artful fits
that as it is, through what any host would use: a surface it is given (`ArtfSurface`) and a picture
to put over the scene (`ArtfOverlay`).

The bridge is not part of Artful. It is `integrations/cascade`, a target of its own,
`artful::cascade`: one header, `artful/cascade.h`, the only file that names both libraries. Artful's
headers name no other library, and cascade does not know Artful exists, so each changes on its own and
only the bridge follows both. It does not lean on either's layout: cascade's damage rectangles are
copied into Artful's, field by field.

```cmake
add_subdirectory(artful)                                  # or find_package(artful)
add_subdirectory(cascade)                                 # or find_package(cascade)
add_subdirectory(artful/integrations/cascade artful-cascade) # or find_package(artfulCascade)
target_link_libraries(my_app PRIVATE artful::cascade)
cascade_app(my_app UI ui)
```

```c
#include "cascade/cascade.h"
#include "cascade/window.h"
#include "artful/artful.h"
#include "artful/cascade.h"

static ArtfCscdHost host;

static void onFrame(void* user) {
    /* move things: artfInstanceSetTransform, artfSceneSetCamera... */
}

int main(void) {
    host.scene = artfSceneNew();
    /* ...fill the scene... */

    CscdWindowOptions o = cscdWindowOptions();
    o.page = "ui/index.html";
    o.presenter = artfCscdPresenter;
    o.presenterUser = &host;
    CscdWindow* w = cscdWindowNew(&o);
    cscdViewSetTransparent(cscdWindowView(w), true);
    cscdWindowOnFrame(w, onFrame, NULL);   /* with this set, the window keeps drawing */

    int code = cscdWindowRun(w);
    cscdWindowFree(w);
    artfSceneFree(host.scene);              /* after the window: its presenter draws the scene */
    return code;
}
```

`integrations/cascade/example` is this in full, with a page whose controls drive the scene:

```bash
cmake -S . -B build-cascade -G Ninja -DARTFUL_CASCADE_DIR=../Cascade
cmake --build build-cascade
build-cascade/artful-cascade
```

With an installed cascade, `-DARTFUL_CASCADE=ON` in place of the folder.

`--software` draws on the CPU, and `--no-vsync` shows what a frame costs.

## What happens to a frame

The window paints the page as always, on the CPU, and hands the frame to `present`:

- it becomes Artful's overlay, with cascade's `partial` and `damage` as they are. On the GPU only the
  rectangles that changed go up: a page at rest uploads nothing, however fast the scene moves;
- Artful draws the scene and blends the overlay over it, with premultiplied alpha, and shows it.

On the CPU the page does more: a tile under an opaque part of the page is not drawn at all, so
opaque panels make the scene cheaper. There, Artful's frame goes to cascade's own software presenter,
without a copy: all the platform code for showing pixels stays cascade's.

| Backend | Frame shown by | With a page |
| --- | --- | --- |
| Vulkan | Artful's swapchain, on the surface cascade hands over | 1,240 fps at 1100 x 700 on a Vega 8; the screen's 120 at 1080 x 2340 on an Adreno 830 |
| Software | cascade's software presenter | 160 fps at 1100 x 700 on four cores; 110 to 120 at 1080 x 2340 on a Snapdragon 8 Elite |

## What a presenter goes through

The bridge keeps what cascade promises. `integrations/cascade/tests/window.c` runs the first two in a real window,
on both backends (`ARTFUL_TEST_WINDOW=1`), with the loss of the device simulated by its presenter:

- **A new size**: `resize` goes to the renderer, which remakes its swapchain before the next frame.
- **A lost device**: `artfRendererDraw` returns false, `present` returns false, and cascade frees the
  presenter and asks for another. The scene is the app's, so the new renderer draws it as it was.
- **A surface taken away** (Android): the same path, run on a phone, going to the background and back
  over and over, on both backends.
- **No Vulkan**: the renderer is the CPU's, and the window never knows.

Features belong to the scene, so they survive a new presenter:
`artfSceneSetFeature(host.scene, ARTF_FEATURE_OCCLUSION, true)` from a button of the page takes effect
on the next frame.

`host.renderer` is the renderer drawing now, for `artfRendererStats`; it is NULL between presenters.

## What to know

- Use the scene from the window's thread, as `cscdWindowOnFrame` does. Nothing may change it while
  `present` runs, and on the window's thread nothing can.
- Telling a click on the page from a click on the scene is cascade's: give the scene an element of
  its own and listen to it (cascade's rendering guide, `docs/rendering.md` in cascade).
- The page's pixels stay the CPU's where they are opaque. Translucent ones are blended by the GPU,
  which rounds in its own way, as cascade says of any presenter.
- `artfRendererReadPixels` reads back only what stayed in memory: not a frame shown in a window.
- On macOS and iOS the bridge draws on the CPU for now: the Metal layer comes from a function internal
  to cascade. Offering that through `gpu.h` would give those systems the GPU path with no other change
  here. No window has been opened on them or on Linux yet. On Android `integrations/cascade/androidApk.sh` builds the
  example as an APK, with no JDK and no Gradle.
