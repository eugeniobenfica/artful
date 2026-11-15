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
};

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
extern bool artfVulkanRefuseSwapchain;
/* For tests: every new variant of the scene's pixel shaders is made before the frame that asks for it, as on the
 * first frame, never on a thread of its own while frames go on without it. */
extern bool artfVulkanShadersWait;
/* For tests: bottom levels the ray tracing hardware built, [0], and refitted, [1], since the program began. */
extern uint64_t artfVulkanBottomLevels[2];

/* NULL when Vulkan cannot run here. */
ArtfRenderer* artfVulkanNew(const ArtfRendererOptions* options);
/* artfRendererWarm's */
void artfVulkanWarm(ArtfRenderer* r);
/* NULL when Metal 3 is not here. */
ArtfRenderer* artfMetalNew(const ArtfRendererOptions* options);

#endif
