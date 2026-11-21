/* artful — a frame's passes as one graph for both backends (the plan's phase 4.2, its fourth step): each pass, by the
 * name the backends give it in ArtfFrameStats.passes, with what it reads and writes of what a frame makes; and the check
 * that a frame's passes, in the order a backend ran them, keep to it. The GPU and the CPU make a frame differently
 * (the GPU draws the scene and then takes its light apart, the CPU bins it and shades tiles), so the graph is not one
 * order but what must come before what: each backend's order is one that the graph allows. */
#include "render/framePasses.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* What a frame's passes make and take. */
enum {
    R_LIST,       /* what is in view: culled, and its draws */
    R_LATE,       /* what the depth of what showed last frame did not hide: occlusion's second list */
    R_GEOMETRY,   /* the CPU's: triangles binned into tiles */
    R_SHADOWS,    /* the lights' maps */
    R_FOG,        /* fog's grid */
    R_PROBES,     /* light that bounced */
    R_TRACED,     /* what paths and rays gathered */
    R_BACKGROUND, /* a background pass's light */
    R_LIGHT,      /* the scene's light */
    R_DEPTH,      /* the scene's depth */
    R_PYRAMID,    /* the depth pyramid */
    R_POST,       /* what comes after the frame works on: the light and what goes with it, as buffers */
    R_FRAME       /* what is shown */
};
#define R(x) (1u << (x))

typedef struct FramePass {
    const char* name;
    uint32_t reads, writes;
} FramePass;

static const FramePass graph[] = {
    /* the GPU's */
    {"cull", 0, R(R_LIST)},
    {"probes", 0, R(R_PROBES)},
    {"background", 0, R(R_BACKGROUND)},
    {"shadows", 0, R(R_SHADOWS)},
    {"fog grid", R(R_SHADOWS), R(R_FOG)},
    {"trace", R(R_LIST), R(R_TRACED)},
    {"scene, early", R(R_LIST), R(R_LIGHT) | R(R_DEPTH)},
    {"occlusion", R(R_DEPTH), R(R_PYRAMID) | R(R_LATE)},
    {"scene, late", R(R_LATE) | R(R_SHADOWS) | R(R_FOG) | R(R_PROBES) | R(R_TRACED) | R(R_BACKGROUND), R(R_LIGHT) | R(R_DEPTH)},
    {"scene", R(R_LIST) | R(R_SHADOWS) | R(R_FOG) | R(R_PROBES) | R(R_TRACED) | R(R_BACKGROUND), R(R_LIGHT) | R(R_DEPTH)},
    {"depth for what follows", R(R_DEPTH), R(R_POST)},
    {"layers", R(R_DEPTH) | R(R_LIGHT), R(R_LIGHT)},
    {"gather", R(R_LIGHT) | R(R_DEPTH), R(R_POST)},
    {"overlay and out", R(R_LIGHT), R(R_FRAME)},
    /* the CPU's */
    {"prepare and cull", 0, R(R_LIST)},
    {"geometry, early", R(R_LIST), R(R_GEOMETRY)},
    {"geometry", R(R_LIST) | R(R_LATE), R(R_GEOMETRY)},
    {"geometry of layers", R(R_LIST), R(R_GEOMETRY)},
    {"tiles", R(R_GEOMETRY) | R(R_SHADOWS) | R(R_FOG) | R(R_PROBES) | R(R_TRACED) | R(R_BACKGROUND), R(R_LIGHT) | R(R_DEPTH) | R(R_POST)},
    /* both: what comes after the frame (post.c's order, and postShaders.inc's) */
    {"ambient occlusion", R(R_POST), R(R_POST)},
    {"reflections", R(R_POST), R(R_POST)},
    {"sampled light gathered", R(R_POST), R(R_POST)},
    {"upscaling", R(R_POST), R(R_POST)},
    {"temporal antialiasing", R(R_POST), R(R_POST)},
    {"sharpening", R(R_POST), R(R_POST)},
    {"depth of field", R(R_POST), R(R_POST)},
    {"motion blur", R(R_POST), R(R_POST)},
    {"auto exposure", R(R_POST), R(R_POST)},
    {"bloom", R(R_POST), R(R_POST)},
    {"tone map and out", R(R_POST), R(R_FRAME)},
};
enum { NODES = (int)(sizeof graph / sizeof graph[0]) };

/* (the CPU's occlusion: the tiles' depth of what showed, into what the rest is tested against) */
static const FramePass cpuOcclusion = {"occlusion", R(R_GEOMETRY), R(R_LATE)};

static const FramePass* nodeOf(const char* name, ArtfBackend backend) {
    if (!name) return NULL;
    if (backend == ARTF_BACKEND_SOFTWARE && !strcmp(name, cpuOcclusion.name)) return &cpuOcclusion;
    for (int k = 0; k < NODES; ++k)
        if (!strcmp(graph[k].name, name)) return &graph[k];
    return NULL;
}

int artfFramePassesOrder(const char* const* names, uint32_t count, ArtfBackend backend, char* why, size_t size) {
    if (why && size) why[0] = 0;
    int wrong = 0;
    for (uint32_t j = 0; j < count; ++j) {
        const FramePass* p = nodeOf(names[j], backend);
        if (!p) {
            if (why && !wrong) snprintf(why, size, "%s: not in the graph", names[j] ? names[j] : "?");
            ++wrong;
            continue;
        }
        /* (what it reads, if a pass of this frame makes it, one before it does: not only one after) */
        for (int res = 0; res < 32; ++res) {
            if (!(p->reads >> res & 1)) continue;
            bool before = false;
            const char* after = NULL;
            for (uint32_t k = 0; k < count; ++k) {
                const FramePass* q = k == j ? NULL : nodeOf(names[k], backend);
                if (!q || !(q->writes >> res & 1)) continue;
                if (k < j) before = true;
                else if (!after) after = q->name;
            }
            if (after && !before) {
                if (why && !wrong) snprintf(why, size, "%s reads what only %s, after it, writes", p->name, after);
                ++wrong;
            }
        }
    }
    return wrong;
}

int artfFramePassesCheck(const ArtfRenderer* r, char* why, size_t size) {
    ArtfFrameStats stats;
    artfRendererStats(r, &stats);
    const char* names[ARTF_PASS_TIMES];
    for (uint32_t k = 0; k < stats.passCount; ++k) names[k] = stats.passes[k].name;
    return artfFramePassesOrder(names, stats.passCount, artfRendererBackend(r), why, size);
}
