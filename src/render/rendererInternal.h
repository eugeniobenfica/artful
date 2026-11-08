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
};

ArtfRenderer* artfSoftwareNew(const ArtfRendererOptions* options);
/* For tests. Submits that go through before the device is reported lost, when not below 0; and a swapchain the driver
 * refuses. */
extern int artfVulkanSubmitsLeft;
extern bool artfVulkanRefuseSwapchain;

/* NULL when Vulkan cannot run here. */
ArtfRenderer* artfVulkanNew(const ArtfRendererOptions* options);
/* NULL when Metal 3 is not here. */
ArtfRenderer* artfMetalNew(const ArtfRendererOptions* options);

#endif
