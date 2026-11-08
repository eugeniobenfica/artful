/* artful-blackhole — a black hole in a window, to fly around. Windows only; all it adds to the library
 * is the window.
 *
 *   drag      go around it        wheel   nearer or farther
 *   d         Doppler on and off  s       one ray a pixel, or four, with motion blur
 *   o         orbit, or stay put  space   stop time
 *   1 to 4    no spin, up to .99  Esc     leave
 *
 * The eye starts out in orbit: it goes around at the speed of one, the way the gas goes, and what
 * it sees is what one moving that fast sees.
 *
 *   artful-blackhole [--backend vulkan|software] [--size w h] [--seconds n]
 *
 * With --seconds it leaves by itself, and says in a file beside it (artful-blackhole.txt) what the
 * frames took. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include "artful/artful.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct View {
    ArtfRenderer* renderer;
    ArtfScene* scene;
    ArtfBlackHoleDesc hole;
    float around, tilt, distance;
    int width, height;
    bool dragging, paused, sized, orbiting;
    int lastX, lastY;
} View;

static void place(View* v) {
    const float c = cosf(v->tilt);
    const ArtfVec3 side = artfVec3(cosf(v->around), 0, -sinf(v->around)); /* the way the gas goes */
    const float beta = v->orbiting ? artfBlackHoleOrbitSpeed(v->hole.spin, v->distance) : 0.f;
    v->hole.velocity = artfVec3Scale(side, beta);
    /* (one who moves sees the hole displaced toward where they are going: aimed there, it stays in frame) */
    const ArtfVec3 aim = artfVec3Scale(v->hole.velocity, v->distance / sqrtf(1.f - beta * beta));
    ArtfCamera camera = {artfVec3(sinf(v->around) * c * v->distance, sinf(v->tilt) * v->distance, cosf(v->around) * c * v->distance), aim, artfVec3(0, 1, 0), .33f, .1f};
    artfSceneSetCamera(v->scene, &camera);
}

static LRESULT CALLBACK onMessage(HWND window, UINT message, WPARAM w, LPARAM l) {
    View* v = (View*)GetWindowLongPtrA(window, GWLP_USERDATA);
    if (v) switch (message) {
    case WM_SIZE:
        v->width = LOWORD(l);
        v->height = HIWORD(l);
        v->sized = true;
        return 0;
    case WM_LBUTTONDOWN:
        v->dragging = true;
        v->lastX = GET_X_LPARAM(l);
        v->lastY = GET_Y_LPARAM(l);
        SetCapture(window);
        return 0;
    case WM_LBUTTONUP:
        v->dragging = false;
        ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE:
        if (v->dragging) {
            v->around -= (float)(GET_X_LPARAM(l) - v->lastX) * .005f;
            v->tilt = artfClampf(v->tilt + (float)(GET_Y_LPARAM(l) - v->lastY) * .005f, -1.5f, 1.5f);
            v->lastX = GET_X_LPARAM(l);
            v->lastY = GET_Y_LPARAM(l);
        }
        return 0;
    case WM_MOUSEWHEEL:
        v->distance = artfClampf(v->distance * (GET_WHEEL_DELTA_WPARAM(w) > 0 ? .93f : 1.07f), 7.f, 300.f);
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) DestroyWindow(window);
        if (w == VK_SPACE) v->paused = !v->paused;
        if (w == 'D') v->hole.doppler = v->hole.doppler > 0 ? 0.f : 1.f;
        if (w == 'S') {
            v->hole.samples = v->hole.samples > 1 ? 1 : 2;
            v->hole.shutter = v->hole.samples > 1 ? .6f : 0.f;
        }
        if (w == 'O') v->orbiting = !v->orbiting;
        if (w >= '1' && w <= '4') {
            static const float spins[4] = {0.f, .6f, .9f, .99f};
            v->hole.spin = spins[w - '1'];
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(window, message, w, l);
}

/* The CPU's frames are in memory: showing them is up to the caller. */
static void showPixels(HWND window, const View* v) {
    const uint32_t* pixels = artfRendererPixels(v->renderer);
    if (!pixels) return;
    BITMAPINFO info;
    memset(&info, 0, sizeof info);
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = v->width;
    info.bmiHeader.biHeight = -v->height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    HDC dc = GetDC(window);
    SetDIBitsToDevice(dc, 0, 0, (DWORD)v->width, (DWORD)v->height, 0, 0, 0, (UINT)v->height, pixels, &info, DIB_RGB_COLORS);
    ReleaseDC(window, dc);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR line, int show) {
    (void)previous;
    (void)line;
    View view;
    memset(&view, 0, sizeof view);
    view.width = 1280;
    view.height = 720;
    ArtfBackend backend = ARTF_BACKEND_AUTO;
    double seconds = 0;
    for (int i = 1; i < __argc; ++i) {
        const char* a = __argv[i];
        if (!strcmp(a, "--backend") && i + 1 < __argc) { ++i; backend = !strcmp(__argv[i], "vulkan") ? ARTF_BACKEND_VULKAN : ARTF_BACKEND_SOFTWARE; }
        else if (!strcmp(a, "--size") && i + 2 < __argc) { view.width = atoi(__argv[++i]); view.height = atoi(__argv[++i]); }
        else if (!strcmp(a, "--seconds") && i + 1 < __argc) seconds = atof(__argv[++i]);
    }

    WNDCLASSA type;
    memset(&type, 0, sizeof type);
    type.lpfnWndProc = onMessage;
    type.hInstance = instance;
    type.hCursor = LoadCursor(NULL, IDC_ARROW);
    type.lpszClassName = "artful-blackhole";
    RegisterClassA(&type);
    RECT area = {0, 0, view.width, view.height};
    AdjustWindowRect(&area, WS_OVERLAPPEDWINDOW, FALSE);
    HWND window = CreateWindowA(type.lpszClassName, "Artful", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, area.right - area.left, area.bottom - area.top, NULL, NULL, instance, NULL);
    if (!window) return 1;

    ArtfRendererOptions o;
    memset(&o, 0, sizeof o);
    o.backend = backend;
    o.surface.kind = ARTF_SURFACE_WIN32;
    o.surface.a = window;
    o.width = view.width;
    o.height = view.height;
    view.renderer = artfRendererNew(&o);
    if (!view.renderer) return 1;
    view.scene = artfSceneNew();
    view.hole = artfBlackHoleDefaults();
    artfSceneSetBlackHole(view.scene, &view.hole);
    view.tilt = .06f;
    view.distance = 74.1f;
    view.orbiting = true;
    SetWindowLongPtrA(window, GWLP_USERDATA, (LONG_PTR)&view);
    ShowWindow(window, show);

    char title[200];
    const double start = artfNowMs();
    double last = start, shown = start, total = 0;
    int frames = 0, framesShown = 0;
    for (bool running = true; running;) {
        MSG message;
        while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) running = false;
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
        if (!running) break;
        if (view.sized) {
            view.sized = false;
            if (view.width > 0 && view.height > 0) artfRendererResize(view.renderer, view.width, view.height);
        }
        const double now = artfNowMs();
        if (!view.paused) {
            const float passed = (float)(now - last) * .012f;
            view.hole.time += passed;
            /* the eye's clock and the gas's are the same, so it keeps the pace an orbit has */
            if (view.orbiting) view.around += passed / (view.distance * sqrtf(view.distance) + view.hole.spin);
        }
        last = now;
        place(&view);
        artfSceneSetBlackHole(view.scene, &view.hole);
        if (!artfRendererDraw(view.renderer, view.scene)) break;
        showPixels(window, &view);
        ++frames;
        ++framesShown;
        if (now - shown > 500) {
            total = (now - shown) / framesShown;
            snprintf(title, sizeof title, "Artful: %s, %d x %d, %.1f ms a frame (%.0f fps)", artfRendererDescription(view.renderer), view.width, view.height, total, 1000.0 / total);
            SetWindowTextA(window, title);
            shown = now;
            framesShown = 0;
        }
        if (seconds > 0 && now - start > seconds * 1000) break;
    }
    if (seconds > 0) {
        FILE* f = fopen("artful-blackhole.txt", "w");
        if (f) {
            fprintf(f, "%s, %d x %d: %d frames, %.2f ms a frame\n", artfRendererDescription(view.renderer), view.width, view.height, frames, (artfNowMs() - start) / frames);
            fclose(f);
        }
    }
    artfRendererFree(view.renderer);
    artfSceneFree(view.scene);
    return 0;
}
