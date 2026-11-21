/* artful-web: the spheres (tools/spheres.h) in a browser, the camera going around them. Built with Emscripten
 * (artful-web.html, beside its .js and .wasm), and served with the headers that let a page share memory between
 * threads (tools/web/serve.py), as threads in WebAssembly are workers that do.
 *
 *   ?backend=cpu (the default): the CPU backend, its kernels in WebAssembly's SIMD, on the browser's cores
 *   ?backend=webgpu: WebGPU, where the browser has it
 *
 * Each frame is drawn, read back and put in the canvas; what drew it and how long it took are written under it. */
#include "artful/artful.h"
#include "../spheres.h"

#include <emscripten.h>
#include <emscripten/html5.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Page {
    ArtfRenderer* r;
    ArtfScene* s;
    ArtfCamera camera;
    uint32_t* pixels;
    int width, height, frame;
    double spent; /* ms drawing, over the frames since it was last shown */
} Page;

/* (the frame, BGRA, into the canvas, RGBA) */
EM_JS(void, show, (const uint8_t* rgba, int width, int height, const char* words), {
    const canvas = document.getElementById("frame");
    const context = canvas.getContext("2d");
    const bytes = new Uint8ClampedArray(HEAPU8.subarray(rgba, rgba + width * height * 4)); /* (a copy: shared memory cannot be an image's) */
    context.putImageData(new ImageData(bytes, width, height), 0, 0);
    if (words) document.getElementById("words").textContent = UTF8ToString(words);
});

static void step(void* user) {
    Page* p = user;
    /* around the spheres, a turn in 20 seconds at 60 frames a second */
    const float a = (float)p->frame * (6.2831853f / 1200.f);
    const float x = p->camera.position.x, z = p->camera.position.z;
    ArtfCamera c = p->camera;
    c.position.x = x * cosf(a) + z * sinf(a);
    c.position.z = z * cosf(a) - x * sinf(a);
    artfSceneSetCamera(p->s, &c);
    const double t0 = artfNowMs();
    if (!artfRendererDraw(p->r, p->s) || !artfRendererReadPixels(p->r, p->pixels)) {
        show((const uint8_t*)p->pixels, p->width, p->height, "the frame could not be drawn");
        emscripten_cancel_main_loop();
        return;
    }
    p->spent += artfNowMs() - t0;
    const size_t n = (size_t)p->width * p->height;
    for (size_t i = 0; i < n; ++i) {
        const uint32_t v = p->pixels[i];
        p->pixels[i] = (v & 0xFF00FF00u) | (v >> 16 & 0xFFu) | (v & 0xFFu) << 16;
    }
    char words[256];
    const bool say = ++p->frame % 30 == 0;
    if (say) {
        snprintf(words, sizeof words, "%s: %.1f ms a frame", artfRendererDescription(p->r), p->spent / 30.0);
        p->spent = 0;
    }
    show((const uint8_t*)p->pixels, p->width, p->height, say ? words : NULL);
}

int main(int argc, char** argv) {
    static Page page;
    Page* p = &page;
    p->width = 960;
    p->height = 540;
    ArtfRendererOptions o = artfRendererDefaults(p->width, p->height);
    o.backend = ARTF_BACKEND_SOFTWARE;
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "webgpu")) {
            /* (straight onto the canvas: a browser cannot wait for a frame to be read back) */
            o.backend = ARTF_BACKEND_WEBGPU;
            o.surface.kind = ARTF_SURFACE_WEB_CANVAS;
            o.surface.a = (void*)"#gpu";
        }
    p->r = artfRendererNew(&o);
    if (!p->r) {
        show(NULL, 0, 0, "this browser cannot draw that way here");
        return 1;
    }
    p->s = artfSceneNew();
    spheresScene(p->s, &p->camera);
    p->pixels = malloc((size_t)p->width * p->height * 4);
    emscripten_set_main_loop_arg(step, p, 0, false);
    return 0;
}
