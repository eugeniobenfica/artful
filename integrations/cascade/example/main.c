/* A page of cascade over a scene of Artful: the scene moves on its own, the page is ordinary HTML and
 * CSS, and its controls drive the scene.
 *
 *   artful-cascade [--software] [--no-vsync] [--frames n] */
#include "cascade/cascade.h"
#include "cascade/window.h"

#include "artful/artful.h"
#include "artful/cascade.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING 24

typedef struct App {
    CscdWindow* window;
    CscdView* view;
    ArtfCscdHost host;
    ArtfScene* scene;
    ArtfInstance centre;
    ArtfInstance ring[RING];
    ArtfLight light;
    CscdElement *fps, *backend;
    bool paused;
    double time;
    double last, shown;
    int frames, frameLimit, framesTotal;
} App;

static double now(void) { return artfNowMs() / 1000.0; }

static void buildScene(App* a) {
    ArtfScene* s = a->scene = artfSceneNew();
    static uint32_t squares[128 * 128];
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) squares[y * 128 + x] = ((x / 16 + y / 16) & 1) ? 0xFF2E3648u : 0xFF1F2533u;
    ArtfMaterialDesc floorDesc = {artfVec3(1, 1, 1), artfSceneAddTexture(s, squares, 128, 128, 0), .15f, .28f, artfVec3(0, 0, 0)};
    ArtfTransform t = artfTransformAt(0, -1.6f, 0, 1);
    artfSceneAddInstance(s, artfSceneAddPlane(s, 60.f, 16.f), artfSceneAddMaterial(s, &floorDesc), &t);

    ArtfMaterialDesc gold = {artfVec3(.9f, .62f, .2f), 0, .7f, .22f, artfVec3(0, 0, 0)};
    ArtfMesh torus = artfSceneAddTorus(s, 1.3f, .45f, 96, 48);
    a->centre = artfSceneAddInstance(s, torus, artfSceneAddMaterial(s, &gold), NULL);

    ArtfMesh ball = artfSceneAddSphere(s, .3f, 24, 32);
    for (int i = 0; i < RING; ++i) {
        float h = (float)i / RING;
        ArtfMaterialDesc d = {artfVec3(.5f + .5f * sinf(h * 6.28f), .5f + .5f * sinf(h * 6.28f + 2.1f), .5f + .5f * sinf(h * 6.28f + 4.2f)), 0, .6f, .17f, artfVec3(0, 0, 0)};
        a->ring[i] = artfSceneAddInstance(s, ball, artfSceneAddMaterial(s, &d), NULL);
    }
    a->light = artfSceneAddLight(s, artfVec3(0, 1, 0), artfVec3(.3f, .6f, 1.4f), 8.f);
    artfSceneSetBackground(s, artfVec3(.07f, .08f, .11f));
    ArtfCamera c = {artfVec3(0, 2.6f, 7.5f), artfVec3(1.1f, -.2f, 0), artfVec3(0, 1, 0), 1.f, .1f};
    artfSceneSetCamera(s, &c);
}

static void animate(App* a) {
    const float t = (float)a->time;
    ArtfTransform m = artfTransformTrs(artfVec3(0, 0, 0), artfQuatMul(artfQuatAxisAngle(artfVec3(0, 1, 0), t * .7f), artfQuatAxisAngle(artfVec3(1, 0, 0), 1.2f)), artfVec3(1, 1, 1));
    artfInstanceSetTransform(a->scene, a->centre, &m);
    for (int i = 0; i < RING; ++i) {
        float angle = (float)i / RING * 6.2831853f + t * .5f;
        ArtfTransform b = artfTransformAt(cosf(angle) * 3.2f, sinf(angle * 3.f) * .5f, sinf(angle) * 3.2f, 1.f);
        artfInstanceSetTransform(a->scene, a->ring[i], &b);
    }
    artfLightSet(a->scene, a->light, artfVec3(cosf(t) * 2.5f, 1.4f, sinf(t) * 2.5f), artfVec3(.3f, .6f, 1.4f), 8.f);
}

static void onFrame(void* user) {
    App* a = user;
    const double t = now(), dt = t - a->last;
    a->last = t;
    if (!a->paused) a->time += dt;
    animate(a);
    ++a->frames;
    ++a->framesTotal;
    if (t - a->shown >= .5) {
        char text[96];
        snprintf(text, sizeof text, "%.0f fps", (double)a->frames / (t - a->shown));
        cscdElementSetText(a->fps, cscdSv(text));
        cscdElementSetText(a->backend, cscdSv(cscdWindowBackendDescription(a->window)));
        a->shown = t;
        a->frames = 0;
    }
    if (a->frameLimit && a->framesTotal >= a->frameLimit) {
        printf("%s\n", cscdWindowBackendDescription(a->window));
        cscdWindowClose(a->window);
    }
}

static void onPause(void* user, CscdEvent* ev) {
    (void)ev;
    App* a = user;
    a->paused = !a->paused;
    cscdElementSetText(cscdViewGet(a->view, CSCD_SV("pause")), a->paused ? CSCD_SV("Resume") : CSCD_SV("Pause"));
}

int main(int argc, char** argv) {
    static App app;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--software")) app.host.backend = ARTF_BACKEND_SOFTWARE;
        else if (!strcmp(argv[i], "--no-vsync")) app.host.noVsync = true;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) app.frameLimit = atoi(argv[++i]);
    }
#ifdef ARTFUL_EXAMPLE_SOFTWARE /* (for a phone, where a program takes no arguments) */
    app.host.backend = ARTF_BACKEND_SOFTWARE;
#endif
    buildScene(&app);
    app.host.scene = app.scene;

    CscdWindowOptions o = cscdWindowOptions();
    o.title = "Artful under cascade";
    o.width = 1100;
    o.height = 700;
    o.page = "ui/index.html";
    o.presenter = artfCscdPresenter;
    o.presenterUser = &app.host;
    app.window = cscdWindowNew(&o);
    app.view = cscdWindowView(app.window);
    cscdViewSetTransparent(app.view, true);

    app.fps = cscdViewGet(app.view, CSCD_SV("fps"));
    app.backend = cscdViewGet(app.view, CSCD_SV("backend"));
    cscdElementOnClick(cscdViewGet(app.view, CSCD_SV("pause")), onPause, &app);
    app.last = app.shown = now();
    cscdWindowOnFrame(app.window, onFrame, &app);

    int code = cscdWindowRun(app.window);
    cscdWindowFree(app.window);
    artfSceneFree(app.scene);
    return code;
}
