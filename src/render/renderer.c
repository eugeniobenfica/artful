/* artful — the choice of a backend, and the calls every backend answers. */
#include "render/rendererInternal.h"
#include "scene/sceneInternal.h"
#include "core/threads.h"

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

/* ---- pacing ---- */

enum { PACE_WAIT = 30 };

void artfRendererSetPacing(ArtfRenderer* r, float frameMs, float least) {
    r->paceMs = frameMs > 0 ? frameMs : 0;
    r->paceLeast = artfClampf(least > 0 ? least : .5f, .25f, 1.f);
    r->paceTook = 0;
    r->paceWait = 0;
    if (r->paceMs <= 0) r->renderScale = r->renderScaleMost;
}
void artfRendererSetThermalHeadroom(ArtfRenderer* r, float headroom) { r->headroom = artfClampf(headroom, 0, 2); }
float artfRendererRenderScale(const ArtfRenderer* r) { return r->renderScale; }

/* After a frame: what it took, into a mean of the last few; and, when it has settled, the size that would take the
 * budget, as the square root of the time's share (pixels go as the width squared), by steps of 1/32, down at once
 * and up only with a tenth to spare, so that it does not go back and forth around the budget. */
static void pace(ArtfRenderer* r) {
    if (r->paceMs <= 0 || r->renderScaleMost >= 1.f) return;
    ArtfFrameStats stats;
    artfRendererStats(r, &stats);
    const double took = stats.gpuMs > stats.cpuMs ? stats.gpuMs : stats.cpuMs;
    if (!(took > 0)) return;
    r->paceTook = r->paceTook > 0 ? r->paceTook * .8 + took * .2 : took;
    if (r->paceWait > 0) {
        --r->paceWait;
        return;
    }
    const float least = artfMinf(r->paceLeast, r->renderScaleMost), hot = artfClampf((r->headroom - .75f) / .25f, 0, 1);
    const double budget = r->paceMs * (1.f - .4f * hot);
    double want = r->renderScale * sqrt(budget / r->paceTook);
    if (r->paceTook <= budget && want < r->renderScale * 1.1) return; /* (within it, and not by enough to grow) */
    if (want > r->renderScale) want = r->renderScale + (want - r->renderScale) * .5; /* (up, half the way) */
    want = floor(want * 32.) / 32.;
    const float next = artfClampf((float)want, least, r->renderScaleMost);
    if (fabsf(next - r->renderScale) < 1.f / 64.f) return;
    r->renderScale = next;
    r->paceWait = PACE_WAIT;
    r->paceTook = 0;
}

bool artfRendererDraw(ArtfRenderer* r, ArtfScene* scene) {
    artfSceneTextureRoles(scene);
    scene->drawingThread = artfThreadId();
    const bool drawn = r->vt->draw(r, scene);
    scene->drawingThread = 0;
    if (drawn && artfSceneFeature(scene, ARTF_FEATURE_OCCLUSION_AUTO)) tryOcclusion(r, scene);
    if (drawn) pace(r);
    if (!drawn && !r->lostSaid) {
        r->lostSaid = true;
        if (r->lostWhy[0]) artfEventSay(ARTF_EVENT_DEVICE_LOST, r, 0, 0, "artful: the device is lost: free the renderer, and give the scene to a new one (the driver: %s)", r->lostWhy);
        else artfEventSay(ARTF_EVENT_DEVICE_LOST, r, 0, 0, "artful: the device is lost: free the renderer, and give the scene to a new one");
    }
    return drawn;
}
void artfRendererWarm(ArtfRenderer* r) {
#ifdef ARTFUL_VULKAN
    if (r && r->backend == ARTF_BACKEND_VULKAN) artfVulkanWarm(r);
#else
    (void)r;
#endif
}
bool artfRendererReadPixels(ArtfRenderer* r, uint32_t* dst) { return r->vt->readPixels(r, dst); }
uint32_t* artfRendererPixels(ArtfRenderer* r) { return r->vt->pixels(r); }
void artfRendererStats(const ArtfRenderer* r, ArtfFrameStats* out) {
    memset(out, 0, sizeof *out);
    r->vt->stats(r, out);
}

/* ---- what a frame shows ---- */

static const char* const viewNames[ARTF_VIEW_COUNT] = {"lit", "base-color", "normals", "roughness", "metallic", "ambient", "emissive",
                                                        "lod", "meshlets", "instances", "overdraw", "depth", "motion", "lights"};

const char* artfViewName(ArtfView view) { return (unsigned)view < ARTF_VIEW_COUNT ? viewNames[view] : NULL; }

bool artfViewFromName(const char* name, ArtfView* out) {
    for (int i = 0; i < ARTF_VIEW_COUNT; ++i)
        if (name && !strcmp(name, viewNames[i])) {
            *out = (ArtfView)i;
            return true;
        }
    return false;
}

void artfRendererSetView(ArtfRenderer* r, ArtfView view) { r->view = (unsigned)view < ARTF_VIEW_COUNT ? view : ARTF_VIEW_LIT; }
ArtfView artfRendererView(const ArtfRenderer* r) { return r->view; }
