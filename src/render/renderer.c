/* artful — the choice of a backend, and the calls every backend answers. */
#include "render/rendererInternal.h"

const char* artfBackendName(ArtfBackend b) {
    switch (b) {
    case ARTF_BACKEND_VULKAN: return "Vulkan";
    case ARTF_BACKEND_SOFTWARE: return "software";
    case ARTF_BACKEND_METAL: return "Metal";
    default: return "auto";
    }
}

ArtfRenderer* artfRendererNew(const ArtfRendererOptions* o) {
    ArtfRenderer* r = NULL;
    if (o->backend == ARTF_BACKEND_METAL) {
#ifdef ARTFUL_METAL
        r = artfMetalNew(o);
#endif
        if (!r) artfLog("artful: no Metal backend here");
        return r;
    }
#ifdef ARTFUL_VULKAN
    if (o->backend != ARTF_BACKEND_SOFTWARE) r = artfVulkanNew(o);
#endif
    if (!r && o->backend != ARTF_BACKEND_VULKAN) r = artfSoftwareNew(o);
    if (!r) artfLog("artful: no %s backend here", artfBackendName(o->backend));
    return r;
}

ArtfRendererOptions artfRendererDefaults(int width, int height) {
    ArtfRendererOptions o;
    memset(&o, 0, sizeof o);
    o.width = width;
    o.height = height;
    return o;
}

void artfRendererFree(ArtfRenderer* r) { if (r) r->vt->destroy(r); }
ArtfBackend artfRendererBackend(const ArtfRenderer* r) { return r->backend; }
const char* artfRendererDescription(const ArtfRenderer* r) { return r->vt->description(r); }
void artfRendererResize(ArtfRenderer* r, int width, int height) { r->vt->resize(r, width, height); }
void artfRendererSetOverlay(ArtfRenderer* r, const ArtfOverlay* overlay) { r->vt->setOverlay(r, overlay); }
/* A round: the way it is for some frames, the other way for as many, and the better of the two until the next. The
 * first frames of a way are left out: they pay for the change. */
enum { TRIAL_ROUND = 240, TRIAL_SETTLE = 2, TRIAL_FRAMES = 8, TRIAL_WAY = TRIAL_SETTLE + TRIAL_FRAMES };

static void tryOcclusion(ArtfRenderer* r, ArtfScene* scene) {
    const int frame = r->trialFrame;
    r->trialFrame = (frame + 1) % TRIAL_ROUND;
    if (frame >= TRIAL_WAY * 2) return;
    const int way = frame / TRIAL_WAY, at = frame % TRIAL_WAY;
    if (at >= TRIAL_SETTLE) {
        ArtfFrameStats stats;
        artfRendererStats(r, &stats);
        r->trialMs[way] += stats.gpuMs > stats.cpuMs ? stats.gpuMs : stats.cpuMs;
    }
    if (at != TRIAL_WAY - 1) return;
    const bool on = artfSceneFeature(scene, ARTF_FEATURE_OCCLUSION);
    if (way == 0) {
        artfSceneSetFeature(scene, ARTF_FEATURE_OCCLUSION, !on);
    } else {
        /* now the second way: back to the first if that took less */
        if (r->trialMs[0] < r->trialMs[1]) artfSceneSetFeature(scene, ARTF_FEATURE_OCCLUSION, !on);
        r->trialMs[0] = r->trialMs[1] = 0;
    }
}

bool artfRendererDraw(ArtfRenderer* r, ArtfScene* scene) {
    const bool drawn = r->vt->draw(r, scene);
    if (drawn && artfSceneFeature(scene, ARTF_FEATURE_OCCLUSION_AUTO)) tryOcclusion(r, scene);
    return drawn;
}
bool artfRendererReadPixels(ArtfRenderer* r, uint32_t* dst) { return r->vt->readPixels(r, dst); }
uint32_t* artfRendererPixels(ArtfRenderer* r) { return r->vt->pixels(r); }
void artfRendererStats(const ArtfRenderer* r, ArtfFrameStats* out) {
    memset(out, 0, sizeof *out);
    r->vt->stats(r, out);
}
