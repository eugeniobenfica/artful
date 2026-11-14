/* artful — drawing a scene. A renderer draws into a window's surface or into memory, with one of three
 * backends:
 *
 *   Vulkan    all on the GPU: a whole scene in one indirect draw, culled by a compute shader. Loaded
 *             at run time: no SDK to build, and the program still starts without it.
 *   software  on the CPU: tiles on every core, AVX2 where there is AVX2. Always there. Draws into
 *             memory: showing the pixels is up to the caller.
 *   Metal     macOS and iOS, with Metal 3, built only with ARTFUL_METAL. Written without a Mac and never
 *             run, and without most of what came after 0.4 (docs/limits.md): not chosen unless asked
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
    /* The frame in floats, then what a lens and a film do to it: artfSceneSetPost (bloom, ambient occlusion,
     * reflections, fog that light goes through, temporal antialiasing, depth of field), exposure and tone mapping.
     * It costs memory and time even with no effect on; off, frames are made as they always were. */
    bool hdr;
    /* Vulkan: plain textures are compressed as they go up, to BC or ETC2, whichever the device takes: a quarter
     * of the memory or less, for a little of their quality. Normal maps stay as they are. */
    bool compressTextures;
    /* Vulkan: a file to keep compiled shaders in between runs, so that a program starts faster. NULL: none. */
    const char* pipelineCache;
    /* With hdr: the scene is drawn at this share of the frame's width and height (.5 to 1; at least .25), and made
     * whole at the frame's size after it, as Unreal's TSR and FSR 2 do: the samples of each frame land elsewhere
     * within their pixels, and temporal antialiasing (artfSceneSetPost) gathers them, following what moves. Without
     * it the scene is read between its pixels. What comes after (depth of field, motion blur, bloom, the look, the
     * overlay) and the pixels read back are the frame's size. 0 or 1: as large as the frame. */
    float renderScale;
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

/* What a frame shows, for those who build with Artful: the picture as it is, or one thing that went into it, as a
 * color. Both backends draw each the same, but for what the notes say. Nothing that comes after the frame is done in
 * a view but the lit one (bloom, exposure, fog and the rest), what is behind the scene is black, and what is traced
 * is drawn as triangles are, and a background pass is left out. Off, a view costs nothing. */
typedef enum ArtfView {
    ARTF_VIEW_LIT,        /* the picture */
    ARTF_VIEW_BASE_COLOR, /* what a surface is made of: its color, after textures, vertex colors and decals */
    ARTF_VIEW_NORMALS,    /* which way it faces, in the world, after its normal map: x, y, z as red, green, blue */
    ARTF_VIEW_ROUGHNESS,  /* grey */
    ARTF_VIEW_METALLIC,   /* grey */
    ARTF_VIEW_AMBIENT,    /* the light from all around (sky and ground, the environment or the probes) as it shows */
    ARTF_VIEW_EMISSIVE,   /* the surface's own light */
    ARTF_VIEW_LOD,        /* which level of detail is drawn: white, red, green, blue for 0 to 3 */
    ARTF_VIEW_MESHLETS,   /* a color for each meshlet of a mesh (Vulkan: with geometry shaders; else as instances) */
    ARTF_VIEW_INSTANCES,  /* a color for each instance */
    /* How many triangles cover each pixel, whatever is in front: a red that goes to yellow and white. The CPU draws
     * each pixel once, so this is the work its rasterizer does there, and the GPU's overdraw. With a frame in floats
     * the square root of it shows. */
    ARTF_VIEW_OVERDRAW,
    ARTF_VIEW_DEPTH,      /* how far along the view: white at the eye, grey at 10 units, black far away */
    /* How far each pixel moved since the frame before, by the eye's motion and its instance's: red for x and green
     * for y (down), grey (128) where it stood still, and a 32nd of the way to white or black for each pixel it went */
    ARTF_VIEW_MOTION,
    ARTF_VIEW_LIGHTS,     /* how many lights reach each point, of the frame's own and of the lists past them: as overdraw */
    ARTF_VIEW_COUNT
} ArtfView;

/* "lit", "base-color", "normals", ...; NULL past the last. */
const char* artfViewName(ArtfView view);
/* From such a name; false: there is none. */
bool artfViewFromName(const char* name, ArtfView* out);
/* From the next frame on. Metal draws only the lit one. */
void artfRendererSetView(ArtfRenderer* r, ArtfView view);
ArtfView artfRendererView(const ArtfRenderer* r);

/* A pass of a frame and what it took, in ms. On Vulkan, from timestamps, for the last frame the GPU has finished; on
 * the CPU, from the clock, for the last frame. The name is the library's own and stays. */
typedef struct ArtfPassTime {
    const char* name;
    double ms;
} ArtfPassTime;

#define ARTF_PASS_TIMES 32

/* What the last frame cost. */
typedef struct ArtfFrameStats {
    double cpuMs;             /* inside artfRendererDraw */
    double cullMs;            /* software: the parts of cpuMs */
    double geometryMs;
    double rasterMs;
    double lensMs;            /* software: the background pass (artfSceneSetBackgroundPass): its light, and its bloom */
    uint32_t instancesDrawn;  /* software; the GPU culls on its own */
    uint32_t trianglesBinned; /* software: what reached the tiles */
    uint32_t tilesSkipped;    /* software: under an opaque overlay */
    uint32_t instancesHidden; /* software: dropped by occlusion */
    uint32_t traceSamples;    /* tracing: what each pixel has gathered so far */
    double gpuMs;             /* Vulkan: what the GPU took, for the last frame it has finished */
    uint32_t shadowMapsDrawn; /* Vulkan and software: the others stayed as they were */
    /* Vulkan and software: the passes of the frame in order, each with what it took. On Vulkan they add up to gpuMs;
     * on the CPU, to cpuMs. Only what was done this frame is listed. */
    uint32_t passCount;
    ArtfPassTime passes[ARTF_PASS_TIMES];
} ArtfFrameStats;

void artfRendererStats(const ArtfRenderer* r, ArtfFrameStats* out);

#ifdef __cplusplus
}
#endif

#endif
