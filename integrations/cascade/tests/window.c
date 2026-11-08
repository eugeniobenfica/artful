/* Artful as cascade's presenter, in a real window: what a presenter goes through. Only with a screen,
 * so only when asked for (ARTFUL_TEST_WINDOW), and with cascade at hand (ARTFUL_CASCADE_DIR or an installed cascade). The
 * presenter is wrapped, to count what happens and to say the device is lost when the test wants. */
#include "cascade/cascade.h"
#include "cascade/window.h"

#include "artful/artful.h"
#include "artful/cascade.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("%s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

typedef struct Host {
    ArtfCscdHost artful;
    int made, freed, presented, resized, failNext;
    int width, height;
} Host;

typedef struct Wrapper { CscdPresenter base; CscdPresenter* inner; Host* host; } Wrapper;

static CscdBackend wrapperBackend(const CscdPresenter* p) { return cscdPresenterBackend(((const Wrapper*)p)->inner); }
static const char* wrapperDescription(const CscdPresenter* p) { return cscdPresenterDescription(((const Wrapper*)p)->inner); }
static bool wrapperPresent(CscdPresenter* bp, const CscdImage* frame) {
    Wrapper* p = (Wrapper*)bp;
    if (p->host->failNext) { p->host->failNext = 0; return false; }
    ++p->host->presented;
    p->inner->partial = bp->partial;
    p->inner->damage = bp->damage;
    p->inner->damageCount = bp->damageCount;
    return cscdPresenterPresent(p->inner, frame);
}
static void wrapperResize(CscdPresenter* bp, int width, int height) {
    Wrapper* p = (Wrapper*)bp;
    ++p->host->resized;
    p->host->width = width;
    p->host->height = height;
    cscdPresenterResize(p->inner, width, height);
}
static void wrapperDestroy(CscdPresenter* bp) {
    Wrapper* p = (Wrapper*)bp;
    ++p->host->freed;
    cscdPresenterFree(p->inner);
    free(p);
}
static const CscdPresenterVtbl wrapperTable = {wrapperBackend, wrapperPresent, wrapperResize, wrapperDescription, wrapperDestroy};

static CscdPresenter* makePresenter(void* user, void* surface, int width, int height) {
    Host* h = user;
    ++h->made;
    h->width = width;
    h->height = height;
    Wrapper* p = calloc(1, sizeof *p);
    p->base.vt = &wrapperTable;
    p->inner = artfCscdPresenter(&h->artful, surface, width, height);
    p->host = h;
    return &p->base;
}

static bool waitFor(CscdWindow* w, const Host* h, int presented) {
    const time_t end = time(NULL) + 5;
    while (h->presented < presented && time(NULL) < end) {
        cscdWindowPump(w, false);
        cscdViewInvalidate(cscdWindowView(w));
    }
    return h->presented >= presented;
}

static void testBackend(ArtfBackend backend, const char* expected) {
    Host h;
    memset(&h, 0, sizeof h);
    h.artful.backend = backend;
    h.artful.noVsync = true;
    h.artful.antialias = getenv("ARTFUL_TEST_ANTIALIAS") != NULL;
    ArtfScene* scene = h.artful.scene = artfSceneNew();
    ArtfMaterialDesc d = {artfVec3(.7f, .2f, .2f), 0, .5f, .24f, artfVec3(0, 0, 0)};
    ArtfTransform t = artfTransformAt(0, 0, 0, 1);
    artfSceneAddInstance(scene, artfSceneAddSphere(scene, 1.5f, 32, 48), artfSceneAddMaterial(scene, &d), &t);

    CscdWindowOptions o = cscdWindowOptions();
    o.title = "presenter";
    o.width = 320;
    o.height = 200;
    o.presenter = makePresenter;
    o.presenterUser = &h;
    CscdWindow* w = cscdWindowNew(&o);
    CscdView* v = cscdWindowView(w);
    cscdViewLoadHTML(v, CSCD_SV("<style>html,body{margin:0;background:transparent}#a{position:absolute;left:40px;top:40px;width:40px;height:40px;background:#7c3aed}</style><div id=a></div>"), CSCD_SV(""));
    cscdViewSetTransparent(v, true);

    CHECK(h.made == 1 && h.freed == 0);
    CHECK(h.artful.renderer != NULL);
    CHECK(waitFor(w, &h, 3));
    printf("%s: %s\n", expected, cscdWindowBackendDescription(w));
    CHECK(artfRendererBackend(h.artful.renderer) == backend);

    /* the size follows */
    const int resized = h.resized;
    cscdWindowResize(w, 420, 260);
    const time_t end = time(NULL) + 5;
    while (h.resized == resized && time(NULL) < end) cscdWindowPump(w, false);
    CHECK(h.resized > resized);
    CHECK(waitFor(w, &h, h.presented + 5));

    /* a lost device: the presenter goes, a new one comes, and the scene was not touched */
    const uint32_t instances = artfSceneInstanceCount(scene);
    h.failNext = 1;
    CHECK(waitFor(w, &h, h.presented + 3));
    CHECK(h.made == 2 && h.freed == 1);
    CHECK(h.artful.renderer != NULL);
    CHECK(artfSceneInstanceCount(scene) == instances);
    CHECK(waitFor(w, &h, h.presented + 3));

    cscdWindowFree(w);
    CHECK(h.made == h.freed);
    CHECK(h.artful.renderer == NULL);
    artfSceneFree(scene);
}

int main(void) {
    if (!getenv("ARTFUL_TEST_WINDOW")) {
        puts("no window asked for (ARTFUL_TEST_WINDOW)");
        return 0;
    }
    testBackend(ARTF_BACKEND_SOFTWARE, "software");
    ArtfRendererOptions o;
    memset(&o, 0, sizeof o);
    o.backend = ARTF_BACKEND_VULKAN;
    o.width = o.height = 8;
    ArtfRenderer* probe = artfRendererNew(&o);
    if (probe) {
        artfRendererFree(probe);
        testBackend(ARTF_BACKEND_VULKAN, "Vulkan");
    } else {
        puts("no Vulkan here: its part was not run");
    }
    printf("%d failures\n", failures);
    return failures != 0;
}
