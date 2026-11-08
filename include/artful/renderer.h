/* artful — drawing a scene. A renderer draws into a window's surface or into memory, with one of three
 * backends:
 *
 *   Vulkan    all on the GPU: a whole scene in one indirect draw, culled by a compute shader. Loaded
 *             at run time: no SDK to build, and the program still starts without it.
 *   software  on the CPU: tiles on every core, AVX2 where there is AVX2. Always there. Draws into
 *             memory: showing the pixels is up to the caller.
 *   Metal     macOS and iOS, with Metal 3. Written without a Mac and never run: not chosen unless asked
 *             for by name. See src/metal/metal.c for what it leaves out.
 *
 * Frames are premultiplied BGRA, as a window's; their alpha is 255 unless the scene says otherwise (see
 * artfSceneSetBackgroundAlpha and artfInstanceSetOpacity). An overlay (an interface, say) can be given to be put over the
 * scene: that is all a host needs to show a page over a 3D view, whatever library draws the page. Nothing here knows
 * of any such library; the bridges to them are in integrations/, each a target of its own. */
#ifndef ARTFUL_RENDERER_H
#define ARTFUL_RENDERER_H

#include "artful/scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ArtfBackend { ARTF_BACKEND_AUTO, ARTF_BACKEND_VULKAN, ARTF_BACKEND_SOFTWARE, ARTF_BACKEND_METAL } ArtfBackend;

const char* artfBackendName(ArtfBackend b);

typedef enum ArtfSurfaceKind {
    ARTF_SURFACE_NONE,    /* frames stay in memory: artfRendererReadPixels */
    ARTF_SURFACE_WIN32,   /* a: the HWND */
    ARTF_SURFACE_XLIB,    /* a: the Display*, b: the Window */
    ARTF_SURFACE_WAYLAND, /* a: the wl_display*, b: the wl_surface* */
    ARTF_SURFACE_ANDROID, /* a: the ANativeWindow* */
    ARTF_SURFACE_METAL    /* a: the CAMetalLayer* */
} ArtfSurfaceKind;

typedef struct ArtfSurface {
    ArtfSurfaceKind kind;
    void* a;
    void* b;
} ArtfSurface;

/* What the CPU's kernels may use. Each gives the same pixels: this is for tests and for measuring. */
typedef enum ArtfSimd {
    ARTF_SIMD_AUTO,     /* the widest this processor has */
    ARTF_SIMD_PORTABLE, /* what the compiler makes of plain C */
    ARTF_SIMD_128,      /* SSE4.1 or NEON */
    ARTF_SIMD_256       /* AVX2 */
} ArtfSimd;

typedef struct ArtfRendererOptions {
    ArtfBackend backend; /* Auto: Vulkan where it works, then software */
    ArtfSurface surface; /* the software backend does not use it */
    int width, height;   /* device pixels */
    bool noVsync;        /* show frames at once, not at the screen's pace (Vulkan) */
    /* Software: how many threads (0: as many as the process may use, at most 16; 1: none). */
    int threads;
    ArtfSimd simd;       /* software; what the processor lacks falls back to the next one down */
    /* Four samples a pixel, at the places Vulkan's standard gives them, on both backends: edges are
     * smooth, in color and in alpha. Each pixel of a surface is still shaded once. */
    bool antialias;
    /* Vulkan: plain textures are compressed as they go up, to BC or ETC2, whichever the device takes: a quarter
     * of the memory or less, for a little of their quality. Normal maps stay as they are. */
    bool compressTextures;
    /* Vulkan: a file to keep compiled shaders in between runs, so that a program starts faster. NULL: none. */
    const char* pipelineCache;
} ArtfRendererOptions;

/* What a renderer takes when nothing else is said: fields set by name on this stay right when the struct
 * grows. */
ArtfRendererOptions artfRendererDefaults(int width, int height);

typedef struct ArtfRenderer ArtfRenderer;

/* NULL only when a backend was asked for by name and cannot run here. */
ArtfRenderer* artfRendererNew(const ArtfRendererOptions* options);
void artfRendererFree(ArtfRenderer* r);

ArtfBackend artfRendererBackend(const ArtfRenderer* r);
/* What draws: "AMD Radeon(TM) Vega 8 Graphics", "software, AVX2, 4 threads". Borrowed. */
const char* artfRendererDescription(const ArtfRenderer* r);
void artfRendererResize(ArtfRenderer* r, int width, int height);

/* A rectangle of device pixels. */
typedef struct ArtfRect { int x, y, w, h; } ArtfRect;

/* A picture to show over the scene: premultiplied BGRA, from the frame's top left corner. The pixels
 * are read in this call or in the next artfRendererDraw, so they must stay until it returns. partial:
 * only the damage rectangles differ from the overlay given before, which is what makes a page that
 * barely changes nearly free. */
typedef struct ArtfOverlay {
    const uint32_t* pixels;
    int width, height;
    int stride; /* in pixels; 0: width */
    bool partial;
    const ArtfRect* damage;
    size_t damageCount;
} ArtfOverlay;

/* NULL: none. */
void artfRendererSetOverlay(ArtfRenderer* r, const ArtfOverlay* overlay);

/* Draws the scene, with the overlay over it, and shows it where there is a surface. false: the
 * device is lost, and the renderer must be freed; the scene can go to a new one. */
bool artfRendererDraw(ArtfRenderer* r, ArtfScene* scene);

/* The last frame in dst, width * height BGRA pixels (after waiting for it, on a GPU). */
bool artfRendererReadPixels(ArtfRenderer* r, uint32_t* dst);
/* The software backend's frame, width * height BGRA pixels, valid until the next draw or resize;
 * NULL for the others. Handing this to a presenter costs no copy. */
uint32_t* artfRendererPixels(ArtfRenderer* r);

/* What the last frame cost. */
typedef struct ArtfFrameStats {
    double cpuMs;             /* inside artfRendererDraw */
    double cullMs;            /* software: the parts of cpuMs */
    double geometryMs;
    double rasterMs;
    double lensMs;            /* software: tracing a black hole's light, and its bloom */
    uint32_t instancesDrawn;  /* software; the GPU culls on its own */
    uint32_t trianglesBinned; /* software: what reached the tiles */
    uint32_t tilesSkipped;    /* software: under an opaque overlay */
    uint32_t instancesHidden; /* software: dropped by occlusion */
    uint32_t traceSamples;    /* tracing: what each pixel has gathered so far */
    double gpuMs;             /* Vulkan: what the GPU took, for the last frame it has finished */
    uint32_t shadowMapsDrawn; /* Vulkan and software: the others stayed as they were */
} ArtfFrameStats;

void artfRendererStats(const ArtfRenderer* r, ArtfFrameStats* out);

#ifdef __cplusplus
}
#endif

#endif
