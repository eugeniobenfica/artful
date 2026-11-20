/* artful-soak — hours of assets coming and going, under a fixed budget: does anything grow?
 *
 *   artful-soak [--seconds n] [--backend vulkan|software] [--budget megabytes] [--every seconds] [--size w h]
 *               [--rate steps a second, 60; 0: as fast as it goes]
 *
 * Each step a few meshes and textures are made, of sizes that differ, and put in the scene; as many of the oldest go,
 * so that as many are held all along; residency keeps the textures to the budget. A frame is drawn each step. Every
 * so often it writes a line: the process's memory, the scene's meshes and textures in memory and held by the GPU,
 * what the GPU's memory holds for the renderer (ArtfFrameStats.gpuBytes), how many handles are held. At the end:
 * how fast each grew over the last hour (or the last quarter of the run, when shorter), by least squares; and, built
 * with ARTFUL_COUNT_ALLOCATIONS, what was not given back once all is freed. It fails (1) when the memory of the
 * process or the GPU's grew more than 1 MB an hour over that time, or when anything was left. */

#include "artful/artful.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static void rest(double ms) { Sleep((DWORD)ms); }
typedef struct { DWORD cb, pageFaults; SIZE_T peak, now, a, b, c, d, pagefile, peakPagefile; } Counters;
BOOL WINAPI K32GetProcessMemoryInfo(HANDLE process, void* counters, DWORD size);
static double processMb(void) {
    Counters c;
    memset(&c, 0, sizeof c);
    c.cb = sizeof c;
    return K32GetProcessMemoryInfo(GetCurrentProcess(), &c, sizeof c) ? (double)c.now / 1048576.0 : 0;
}
#elif defined(__linux__)
#include <unistd.h>
static void rest(double ms) { usleep((useconds_t)(ms * 1000)); }
static double processMb(void) {
    FILE* f = fopen("/proc/self/statm", "r");
    long size = 0, resident = 0;
    if (f) {
        if (fscanf(f, "%ld %ld", &size, &resident) != 2) resident = 0;
        fclose(f);
    }
    return (double)resident * (double)sysconf(_SC_PAGESIZE) / 1048576.0;
}
#else
#include <unistd.h>
static void rest(double ms) { usleep((useconds_t)(ms * 1000)); }
static double processMb(void) { return 0; }
#endif

#ifdef ARTFUL_COUNT_ALLOCATIONS
long artfAllocationsAlive(void);
#endif

enum { HELD = 24, MATERIALS = 8, SAMPLES = 4096 };

typedef struct Asset { ArtfMesh mesh; ArtfTexture texture; ArtfInstance instance; } Asset;

static uint32_t randomState = 12345;
static uint32_t randomOf(void) {
    randomState = randomState * 747796405u + 2891336453u;
    uint32_t x = randomState;
    x = (x ^ x >> 16) * 0x7FEB352Du;
    return x ^ x >> 15;
}

/* A slope by least squares, per hour, of the samples from first on. */
static double slopeOf(const double* t, const double* y, int first, int count) {
    double st = 0, sy = 0, stt = 0, sty = 0;
    const int n = count - first;
    if (n < 2) return 0;
    for (int i = first; i < count; ++i) { st += t[i]; sy += y[i]; stt += t[i] * t[i]; sty += t[i] * y[i]; }
    const double d = n * stt - st * st;
    return d > 0 ? (n * sty - st * sy) / d * 3600.0 : 0;
}

int main(int argc, char** argv) {
    double seconds = 14400, every = 60, rate = 60;
    size_t budget = 64;
    ArtfRendererOptions o;
    memset(&o, 0, sizeof o);
    o.backend = ARTF_BACKEND_VULKAN;
    o.width = 640;
    o.height = 360;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = atof(argv[++i]);
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atof(argv[++i]);
        else if (!strcmp(argv[i], "--budget") && i + 1 < argc) budget = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--backend") && i + 1 < argc) o.backend = !strcmp(argv[++i], "software") ? ARTF_BACKEND_SOFTWARE : ARTF_BACKEND_VULKAN;
        else if (!strcmp(argv[i], "--size") && i + 2 < argc) { o.width = atoi(argv[++i]); o.height = atoi(argv[++i]); }
        else { fprintf(stderr, "unknown: %s\n", argv[i]); return 2; }
    }
    ArtfRenderer* r = artfRendererNew(&o);
    if (!r) {
        fprintf(stderr, "no renderer\n");
        return 2;
    }
    ArtfScene* s = artfSceneNew();
    const ArtfCamera camera = {artfVec3(0, 6, 14), artfVec3(0, 0, 0), artfVec3(0, 1, 0), 1.f, .1f};
    artfSceneSetCamera(s, &camera);
    ArtfResidencyDesc residency;
    memset(&residency, 0, sizeof residency);
    residency.mode = ARTF_RESIDENCY_AUTOMATIC;
    residency.budget = budget << 20;
    residency.interval = 10;
    artfSceneSetResidency(s, &residency);
    ArtfMaterial materials[MATERIALS];
    ArtfMaterialDesc descs[MATERIALS];
    for (int k = 0; k < MATERIALS; ++k) {
        descs[k] = artfMaterialDefaults();
        descs[k].roughness = .2f + .1f * (float)k;
        materials[k] = artfSceneAddMaterial(s, &descs[k]);
    }
    Asset held[HELD];
    memset(held, 0, sizeof held);
    static double times[SAMPLES], process[SAMPLES], gpu[SAMPLES];
    int samples = 0;
    uint32_t* pixels = malloc((size_t)1024 * 1024 * 4);
    const double start = artfNowMs() * .001;
    double next = 0, peakProcess = 0, peakGpu = 0;
    unsigned long long steps = 0, made = 0;
    printf("seconds  process MB  meshes MB (GPU)  textures MB (GPU)  GPU MB  instances  made\n");
    for (;;) {
        const double now = artfNowMs() * .001 - start;
        /* one goes, one comes: its texture to a material of the pool, which lets the one before it go */
        const int at = (int)(steps % HELD);
        Asset* a = &held[at];
        if (a->instance) artfSceneRemoveInstance(s, a->instance);
        if (a->mesh) artfSceneRemoveMesh(s, a->mesh);
        const int side = 64 << (randomOf() % 5); /* 64 to 1024 */
        const uint32_t tint = randomOf() | 0xFF000000u;
        for (int y = 0; y < side; ++y)
            for (int x = 0; x < side; ++x) pixels[(size_t)y * side + x] = ((x ^ y) & 16) ? tint : 0xFF202020u;
        const ArtfTexture texture = artfSceneAddTexture(s, pixels, side, side, 0);
        const int m = at % MATERIALS;
        const ArtfTexture old = descs[m].texture;
        descs[m].texture = texture;
        artfMaterialSet(s, materials[m], &descs[m]);
        if (old && old != texture) {
            /* (the texture another asset of this material had: none uses it now) */
            for (int k = 0; k < HELD; ++k)
                if (held[k].texture == old) held[k].texture = 0;
            artfSceneRemoveTexture(s, old);
        }
        a->texture = texture;
        a->mesh = randomOf() & 1 ? artfSceneAddSphere(s, .5f + (float)(randomOf() % 100) * .005f, 8 + (int)(randomOf() % 24), 12 + (int)(randomOf() % 36)) : artfSceneAddCube(s, .8f);
        const ArtfTransform t = artfTransformAt((float)(at % 6) * 2.f - 5.f, 0, (float)(at / 6) * 2.f - 4.f, 1);
        a->instance = artfSceneAddInstance(s, a->mesh, materials[m], &t);
        ++made;
        if (!artfRendererDraw(r, s)) {
            fprintf(stderr, "a frame could not be drawn\n");
            break;
        }
        ++steps;
        if (now >= next || now >= seconds) {
            ArtfSceneMemory mem;
            artfSceneMemory(s, &mem);
            ArtfFrameStats stats;
            artfRendererStats(r, &stats);
            const double p = processMb(), g = (double)stats.gpuBytes / 1048576.0;
            peakProcess = p > peakProcess ? p : peakProcess;
            peakGpu = g > peakGpu ? g : peakGpu;
            if (samples < SAMPLES) {
                times[samples] = now;
                process[samples] = p;
                gpu[samples] = g;
                ++samples;
            }
            printf("%7.0f  %10.1f  %6.1f (%6.1f)  %8.1f (%7.1f)  %6.1f  %9u  %llu\n", now, p, (double)mem.meshes / 1048576.0, (double)mem.residentMeshes / 1048576.0,
                   (double)mem.textures / 1048576.0, (double)mem.residentTextures / 1048576.0, g, artfSceneInstanceCount(s), made);
            fflush(stdout);
            next = now + every;
        }
        if (now >= seconds) break;
        /* (at its rate: the machine it runs on has other work) */
        const double due = rate > 0 ? (double)steps / rate : 0, late = due - (artfNowMs() * .001 - start);
        if (late > .001) rest(late * 1000);
    }
    /* the trend: over the last hour, or the last quarter of a shorter run */
    const double from = times[samples - 1] - (seconds >= 4 * 3600 ? 3600 : seconds / 4);
    int first = 0;
    while (first < samples - 1 && times[first] < from) ++first;
    const double processSlope = slopeOf(times, process, first, samples), gpuSlope = slopeOf(times, gpu, first, samples);
    printf("peak: %.1f MB of the process, %.1f MB of the GPU's; over the last %.0f s, %+.2f MB and %+.2f MB an hour\n", peakProcess, peakGpu, times[samples - 1] - times[first],
           processSlope, gpuSlope);
    free(pixels);
    artfSceneFree(s);
    artfRendererFree(r);
    int failed = processSlope > 1.0 || gpuSlope > 1.0;
#ifdef ARTFUL_COUNT_ALLOCATIONS
    printf("allocations not given back: %ld\n", artfAllocationsAlive());
    failed |= artfAllocationsAlive() != 0;
#endif
    printf("%s\n", failed ? "it grew, or something was left" : "nothing grew, nothing was left");
    return failed;
}
