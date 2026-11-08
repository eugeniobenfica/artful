/* artful — Artful as a presenter of cascade: a 3D scene under a page. This header is all that knows
 * both libraries, and it is not part of Artful: it is the artful::cascade target of
 * integrations/cascade, which links the two. Artful's own headers name no other library, and cascade
 * knows nothing of Artful: each changes on its own, and only this file follows both.
 *
 *   static ArtfCscdHost host;
 *   host.scene = artfSceneNew();
 *   CscdWindowOptions o = cscdWindowOptions();
 *   o.presenter = artfCscdPresenter;
 *   o.presenterUser = &host;
 *   CscdWindow* w = cscdWindowNew(&o);
 *   cscdViewSetTransparent(cscdWindowView(w), true);
 *
 * The window keeps its input, its input methods and its screen readers; each frame of the page goes
 * over the scene as Artful's overlay, with only what changed sent to the GPU. A presenter that goes
 * (Android takes the surface away, a device is lost) leaves the scene as it is: the next one draws it
 * without anything being loaded again. Where Artful draws on the CPU, its frame goes to cascade's own
 * software presenter with no copy. On Apple's systems, where the layer is cascade's to give, it does
 * that for now. */
#ifndef ARTFUL_CASCADE_H
#define ARTFUL_CASCADE_H

#include "artful/artful.h"
#include "cascade/gpu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ArtfCscdHost {
    ArtfScene* scene;      /* the app's: it must outlive the window */
    ArtfBackend backend;   /* Auto: Vulkan where it works, then the CPU */
    bool noVsync;
    int threads;           /* for the CPU */
    bool antialias;
    ArtfRenderer* renderer; /* the one drawing now, for artfRendererStats; set by the presenter */
} ArtfCscdHost;

typedef struct ArtfCscdPresenter {
    CscdPresenter base;
    ArtfCscdHost* host;
    ArtfRenderer* renderer;
    CscdPresenter* inner; /* cascade's own: shows the CPU's frames */
    ARTF_VEC(ArtfRect) damage; /* cascade's rectangles, as Artful's */
} ArtfCscdPresenter;

/* What cscdPresenterNew takes, as Artful calls it. */
ARTF_INLINE ArtfSurface artfCscdSurface(void* surface) {
    ArtfSurface s = {ARTF_SURFACE_NONE, NULL, NULL};
#if defined(_WIN32)
    s.kind = ARTF_SURFACE_WIN32;
    s.a = surface;
#elif defined(__ANDROID__)
    s.kind = ARTF_SURFACE_ANDROID;
    s.a = surface;
#elif !defined(__APPLE__)
    if (((const CscdWaylandWindow*)surface)->mark == CSCD_WAYLAND_MARK) {
        const CscdWaylandWindow* w = (const CscdWaylandWindow*)surface;
        s.kind = ARTF_SURFACE_WAYLAND;
        s.a = w->display;
        s.b = w->surface;
    } else {
        const CscdXlibWindow* w = (const CscdXlibWindow*)surface;
        s.kind = ARTF_SURFACE_XLIB;
        s.a = w->display;
        s.b = (void*)(uintptr_t)w->window;
    }
#else
    (void)surface;
#endif
    return s;
}

ARTF_INLINE CscdBackend artfCscdPresenterBackend(const CscdPresenter* p) {
    return artfRendererBackend(((const ArtfCscdPresenter*)p)->renderer) == ARTF_BACKEND_VULKAN ? CSCD_BACKEND_VULKAN : CSCD_BACKEND_SOFTWARE;
}

ARTF_INLINE bool artfCscdPresenterPresent(CscdPresenter* bp, const CscdImage* frame) {
    ArtfCscdPresenter* p = (ArtfCscdPresenter*)bp;
    /* (copied field by field: neither library promises the other how its rectangles are laid out) */
    p->damage.len = 0;
    artfVecReserve(&p->damage, bp->damageCount);
    for (size_t i = 0; i < bp->damageCount; ++i) {
        ArtfRect r = {bp->damage[i].x, bp->damage[i].y, bp->damage[i].w, bp->damage[i].h};
        artfVecPush(&p->damage, r);
    }
    ArtfOverlay overlay = {frame->pixels, frame->width, frame->height, frame->width, bp->partial, p->damage.items, p->damage.len};
    artfRendererSetOverlay(p->renderer, &overlay);
    if (!artfRendererDraw(p->renderer, p->host->scene)) return false;
    if (!p->inner) return true;
    CscdImage image = {frame->width, frame->height, artfRendererPixels(p->renderer)};
    p->inner->partial = false; /* (the scene moves: all of it changed) */
    return cscdPresenterPresent(p->inner, &image);
}

ARTF_INLINE void artfCscdPresenterResize(CscdPresenter* bp, int width, int height) {
    ArtfCscdPresenter* p = (ArtfCscdPresenter*)bp;
    artfRendererResize(p->renderer, width, height);
    if (p->inner) cscdPresenterResize(p->inner, width, height);
}

ARTF_INLINE const char* artfCscdPresenterDescription(const CscdPresenter* p) { return artfRendererDescription(((const ArtfCscdPresenter*)p)->renderer); }

ARTF_INLINE void artfCscdPresenterDestroy(CscdPresenter* bp) {
    ArtfCscdPresenter* p = (ArtfCscdPresenter*)bp;
    if (p->host->renderer == p->renderer) p->host->renderer = NULL;
    cscdPresenterFree(p->inner);
    artfRendererFree(p->renderer);
    artfVecFree(&p->damage);
    artfFree(p);
}

/* A CscdPresenterFn: user is the ArtfCscdHost. */
ARTF_INLINE CscdPresenter* artfCscdPresenter(void* user, void* surface, int width, int height) {
    static const CscdPresenterVtbl table = {artfCscdPresenterBackend, artfCscdPresenterPresent, artfCscdPresenterResize, artfCscdPresenterDescription, artfCscdPresenterDestroy};
    ArtfCscdHost* host = (ArtfCscdHost*)user;
    ArtfRendererOptions options;
    memset(&options, 0, sizeof options);
    options.backend = host->backend;
    options.surface = artfCscdSurface(surface);
    options.width = width;
    options.height = height;
    options.noVsync = host->noVsync;
    options.threads = host->threads;
    options.antialias = host->antialias;
    /* (with no surface of its own to draw to, the GPU has nowhere to show: the CPU draws) */
    if (options.surface.kind == ARTF_SURFACE_NONE) options.backend = ARTF_BACKEND_SOFTWARE;
    ArtfRenderer* renderer = artfRendererNew(&options);
    if (!renderer) return NULL; /* cascade's own presenter comes instead */
    ArtfCscdPresenter* p = ARTF_NEW(ArtfCscdPresenter);
    p->base.vt = &table;
    p->host = host;
    p->renderer = renderer;
    if (artfRendererBackend(renderer) == ARTF_BACKEND_SOFTWARE) p->inner = cscdPresenterNew(surface, CSCD_BACKEND_SOFTWARE, width, height);
    host->renderer = renderer;
    return &p->base;
}

#ifdef __cplusplus
}
#endif

#endif
