/* artful — a renderer: an object with a table of functions (each backend fills one). */
#ifndef ARTFUL_RENDERER_INTERNAL_H
#define ARTFUL_RENDERER_INTERNAL_H

#include "artful/renderer.h"

typedef struct ArtfRendererVtbl {
    const char* (*description)(const ArtfRenderer* r);
    void (*resize)(ArtfRenderer* r, int width, int height);
    void (*setOverlay)(ArtfRenderer* r, const ArtfOverlay* overlay);
    bool (*draw)(ArtfRenderer* r, ArtfScene* scene);
    bool (*readPixels)(ArtfRenderer* r, uint32_t* dst);
    uint32_t* (*pixels)(ArtfRenderer* r);
    void (*stats)(const ArtfRenderer* r, ArtfFrameStats* out);
    void (*destroy)(ArtfRenderer* r);
} ArtfRendererVtbl;

struct ArtfRenderer {
    const ArtfRendererVtbl* vt;
    ArtfBackend backend;
    /* trying occlusion both ways: where it is in its round, and what each way took */
    int trialFrame;
    double trialMs[2];
    ArtfView view; /* artfRendererSetView's: each backend reads it as a frame starts */
    bool lostSaid; /* ARTF_EVENT_DEVICE_LOST went out: once */
    char lostWhy[768]; /* what the driver said of it (VK_EXT_device_fault), for that event; empty: nothing */
    /* pacing (artfRendererSetPacing): the scene's share of the frame's width now, which each backend takes as a frame
     * starts, and the most it may be (ArtfRendererOptions.renderScale; 1: no pacing); the budget, the least, the
     * heat; what frames took lately, and the frames before the next change may come */
    float renderScale, renderScaleMost;
    float paceMs, paceLeast, headroom;
    double paceTook;
    int paceWait;
    ArtfOutput output; /* what its frames are made for (artfRendererOutput) */
};

/* ArtfRendererOptions.output's numbers: light is divided by inverse's inverse (peak over paper white) before the tone
 * curve, and peak is in the output's units (HDR10, PQ's: nits / 10000; scRGB: nits / 80). */
ARTF_INLINE void artfDisplayNumbers(const ArtfRendererOptions* o, ArtfOutput output, float* inverse, float* peak) {
    const float most = o->peakNits > 0 ? o->peakNits : 1000.f, paper = o->paperWhiteNits > 0 ? o->paperWhiteNits : 203.f;
    const bool high = output == ARTF_OUTPUT_HDR10 || output == ARTF_OUTPUT_SCRGB;
    *inverse = high ? paper / artfMaxf(most, paper) : 1.f;
    *peak = output == ARTF_OUTPUT_HDR10 ? artfMaxf(most, paper) * 1e-4f : output == ARTF_OUTPUT_SCRGB ? artfMaxf(most, paper) / 80.f : 1.f;
}
/* The words of a pixel of a frame made for this output: two in scRGB, else one. */
ARTF_INLINE int artfOutputWords(ArtfOutput output) { return output == ARTF_OUTPUT_SCRGB ? 2 : 1; }

/* The passes of a frame and what each took, as the CPU's clock sees them: each mark ends the pass it names, which
 * started where the one before ended. */
typedef struct ArtfPassLog {
    ArtfPassTime passes[ARTF_PASS_TIMES];
    uint32_t count;
    double last;
} ArtfPassLog;

ARTF_INLINE void artfPassStart(ArtfPassLog* l) {
    l->count = 0;
    l->last = artfNowMs();
}
ARTF_INLINE void artfPassMark(ArtfPassLog* l, const char* name) {
    const double now = artfNowMs();
    if (l->count < ARTF_PASS_TIMES) {
        l->passes[l->count].name = name;
        l->passes[l->count++].ms = now - l->last;
    }
    l->last = now;
}

ArtfRenderer* artfSoftwareNew(const ArtfRendererOptions* options);
/* For tests. Submits that go through before the device is reported lost, when not below 0; and a swapchain the driver
 * refuses. */
extern int artfVulkanSubmitsLeft;
/* For tests: whether the renderer asks the driver what lost its device (VK_EXT_device_fault), and what it said. */
bool artfVulkanFaultReports(const ArtfRenderer* r);
/* Whether it reads textures by constant indices only (no non-uniform indexing, or ARTFUL_UNIFORM_TEXTURES). */
bool artfVulkanUniformTextures(const ArtfRenderer* r);
extern bool artfVulkanRefuseSwapchain;
/* For tests: with preRotate and no surface, the turn the frame is made with (0 to 3: 0, 90, 180, 270 degrees
 * clockwise), as a turned screen's swapchain would ask; artfRendererReadPixels then gives the image as it is. */
extern int artfVulkanTestTurn;
/* For tests and measures: no buffer in an image's memory (postBuffersMake), as before there was. */
extern bool artfVulkanNoAliasing;
/* For tests: above 0, what pacing takes a frame to have taken at renderScale, in place of what it did (ms), going
 * as the pixels drawn at another. */
extern double artfPaceTestMs;
/* For tests: a texture whose image would take more than this many bytes cannot be made, as when the GPU's memory has
 * run out. Below 0: no such limit. */
extern long long artfVulkanTextureMemoryLimit;
/* For tests: the GPU's memory as if it held no more than this many bytes in all (what the renderer holds counted, as
 * ArtfFrameStats.gpuBytes); -1: as it is. */
extern long long artfVulkanMemoryBudget;
/* For tests: every new pipeline a feature asks for (the scene's pixel shaders' variants, shadows from rays, a
 * program's materials, the tracing shader, temporal antialiasing and motion blur) is made before the frame that asks
 * for it, as on the first frame, never on a thread of its own while frames go on without it. */
extern bool artfVulkanShadersWait;
/* For tests: light that bounced gathered on the CPU's workers and sent up, as before the GPU gathered it itself (read
 * as each frame starts); and what the GPU's probes hold now, ARTF_PROBE_FLOATS of each (false: none, or not that many). */
extern bool artfVulkanProbesOnCpu;
bool artfVulkanProbesRead(ArtfRenderer* r, float* out, size_t floats);
/* For tests: bottom levels the ray tracing hardware built, [0], and refitted, [1], since the program began. */
extern uint64_t artfVulkanBottomLevels[2];
/* For tests: the frames temporal antialiasing has taken so far (each moves the place within the pixel). */
uint32_t artfVulkanTemporalFrames(const ArtfRenderer* r);

/* NULL when Vulkan cannot run here. */
ArtfRenderer* artfVulkanNew(const ArtfRendererOptions* options);
/* artfRendererWarm's */
void artfVulkanWarm(ArtfRenderer* r);
/* NULL when Metal 3 is not here. */
ArtfRenderer* artfMetalNew(const ArtfRendererOptions* options);
/* NULL when WebGPU is not here (no adapter, or not what it needs). */
ArtfRenderer* artfWebGpuNew(const ArtfRendererOptions* options);

#endif
