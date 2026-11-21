/* artful — the GPU's frame as a graph (frameGraph.h). */
#include "vulkan/frameGraph.h"
#include "render/rendererInternal.h"
#include "shaderir/ir.h"
#include "vulkan/shaders.h"
#include "vulkan/spirv.h"

#include <string.h>

static const char* const targetNames[FG_TARGETS] = {
    "scene's light (image)", "scene's surfaces (image)", "depth (image)", "depth pyramid", "fog grid", "light", "second light",
    "history", "depth (copy)", "surfaces (copy)", "ambient occlusion", "bloom and exposure", "velocities", "upscaling's locks", "the frame",
    "sampled light, gathered"};
/* What the next frame reads, or what is kept: no other may have its memory. */
static const bool persists[FG_TARGETS] = {[FG_HIZ] = true, [FG_HISTORY] = true, [FG_BLOOM] = true, [FG_LOCKS] = true, [FG_FRAME] = true, [FG_GATHERED] = true};

/* A shader's binding, as a target: set 0's, then set 1's (the three parts the post buffers play). */
static int targetOf(uint32_t set, uint32_t binding) {
    if (set == 1) return binding == 0 ? FG_LIGHT : binding == 1 ? FG_SECOND : binding == 2 ? FG_HISTORY : -1;
    switch (binding) {
    case 8: return FG_DEPTH_IMAGE;
    case 9: return FG_HIZ;
    case 51: return FG_DEPTH;
    case 52: return FG_SURFACE;
    case 55: return FG_OCCLUSION;
    case 56: return FG_FOG;
    case 57: return FG_BLOOM;
    case 58: return FG_FLOAT_COLOR;
    case 59: return FG_FLOAT_SURFACE;
    case 69: return FG_BLUR;
    case 71: return FG_LOCKS;
    case 72: return FG_GATHERED;
    default: return -1;
    }
}

/* ---- the passes, as vulkan.c records them, in that order ---- */

typedef uint32_t* (*Builder)(size_t* size);
static uint32_t* hizFirst(size_t* size) { return artfShaderHiz(size, true, 1); }
static uint32_t* hizNext(size_t* size) { return artfShaderHiz(size, false, 1); }
static uint32_t* gather(size_t* size) { return artfShaderPostGather(size, 1); }
static uint32_t* sceneFloats(size_t* size) {
    return artfShaderSceneFragment(size, 1024, false, NULL, 0, 0, ARTF_SHADER_FLOATS | ARTF_SHADER_PROBES | ARTF_SHADER_WALLS | ARTF_SHADER_FROXELS);
}

#define T(x) (1u << (x))
typedef struct Pass {
    const char* name;      /* as passMark names it */
    Builder shaders[4];
    uint32_t draws, reads; /* what its render pass draws into and reads (attachments), which no shader binds */
} Pass;
/* (the last pass as it is without preRotate) */
static uint32_t* postFinalPlain(size_t* size) { return artfShaderPostFinal(size, false); }

static const Pass passes[] = {
    {"cull", {0}, 0, 0},
    {"probes", {0}, 0, 0},
    {"background", {0}, 0, 0},
    {"shadows", {0}, 0, 0},
    {"fog grid", {artfShaderPostFroxels}, 0, 0},
    {"trace", {0}, 0, 0},
    {"scene, early", {sceneFloats}, T(FG_FLOAT_COLOR) | T(FG_FLOAT_SURFACE) | T(FG_DEPTH_IMAGE), 0},
    {"occlusion", {hizFirst, hizNext}, 0, 0},
    {"scene, late", {sceneFloats}, T(FG_FLOAT_COLOR) | T(FG_FLOAT_SURFACE) | T(FG_DEPTH_IMAGE), T(FG_DEPTH_IMAGE)},
    {"scene", {sceneFloats}, T(FG_FLOAT_COLOR) | T(FG_FLOAT_SURFACE) | T(FG_DEPTH_IMAGE), 0},
    {"depth for what follows", {gather}, 0, 0},
    {"layers", {0}, T(FG_FLOAT_COLOR), T(FG_DEPTH_IMAGE)},
    {"gather", {gather}, 0, 0},
    {"under the surface", {artfShaderPostUnderAcross, artfShaderPostUnderDown}, 0, 0},
    {"ambient occlusion", {artfShaderPostOcclusion, artfShaderPostOcclusionBlur}, 0, 0},
    {"reflections", {artfShaderPostReflection, artfShaderPostReflectionApply}, 0, 0},
    {"sampled light gathered", {artfShaderPostSampled, artfShaderPostSampledSpace}, 0, 0},
    {"clouds", {artfShaderPostCloudColumns, artfShaderPostCloudApply}, 0, 0},
    {"upscaling", {artfShaderPostUpscale}, 0, 0},
    {"temporal antialiasing", {artfShaderPostTemporal}, 0, 0},
    {"sharpening", {artfShaderPostSharpen}, 0, 0},
    {"depth of field", {artfShaderPostFocus}, 0, 0},
    {"motion blur", {artfShaderPostVelocity, artfShaderPostTileMost, artfShaderPostMotionBlur}, 0, 0},
    {"auto exposure", {artfShaderPostMeter}, 0, 0},
    {"bloom", {artfShaderPostBloomDown, artfShaderPostBloomUp}, 0, 0},
    {"tone map and out", {postFinalPlain}, T(FG_FRAME), 0},
};
enum { PASSES = (int)(sizeof passes / sizeof passes[0]) };

/* (what each shader built reads and writes: the IR as it is finished) */
static uint32_t capturedReads, capturedWrites;
static void capture(const IrModule* m) {
    IrUse uses[64];
    const int n = irUses(m, uses, 64);
    for (int k = 0; k < n && k < 64; ++k) {
        const int t = targetOf(uses[k].set, uses[k].binding);
        if (t < 0) continue;
        if (uses[k].read) capturedReads |= T(t);
        if (uses[k].write) capturedWrites |= T(t);
    }
}

static void accessesOf(const Pass* p, uint32_t* reads, uint32_t* writes) {
    void (*was)(const IrModule*) = artfSpvFinished;
    artfSpvFinished = capture;
    capturedReads = capturedWrites = 0;
    for (int k = 0; k < 4 && p->shaders[k]; ++k) {
        size_t size;
        artfFree(p->shaders[k](&size));
    }
    artfSpvFinished = was;
    *reads = capturedReads | p->reads;
    *writes = capturedWrites | p->draws;
}

int artfFrameGraphUndescribed = 0;

uint64_t artfFrameGraphReport(ArtfRenderer* r, FILE* report, FILE* dot) {
    if (!r || r->backend != ARTF_BACKEND_VULKAN) return 0;
    ArtfFrameStats stats;
    artfRendererStats(r, &stats);
    uint64_t bytes[FG_TARGETS];
    artfVulkanTargetBytes(r, bytes);
    /* the passes the frame took, in its order, each with what it reads and writes */
    int order[ARTF_PASS_TIMES], count = 0, at = 0;
    uint32_t reads[ARTF_PASS_TIMES], writes[ARTF_PASS_TIMES];
    artfFrameGraphUndescribed = 0;
    for (uint32_t k = 0; k < stats.passCount; ++k) {
        /* (in the description's order: a pass found before the last one found is out of it) */
        int p = at;
        while (p < PASSES && !(stats.passes[k].name && !strcmp(stats.passes[k].name, passes[p].name))) ++p;
        if (p == PASSES) {
            ++artfFrameGraphUndescribed;
            if (report) fprintf(report, "not described, or out of order: %s\n", stats.passes[k].name ? stats.passes[k].name : "?");
            continue;
        }
        at = p + 1;
        order[count] = p;
        accessesOf(&passes[p], &reads[count], &writes[count]);
        ++count;
    }
    /* each target's life: from the first pass that touches it to the last */
    int first[FG_TARGETS], last[FG_TARGETS];
    for (int t = 0; t < FG_TARGETS; ++t) first[t] = last[t] = -1;
    for (int k = 0; k < count; ++k)
        for (int t = 0; t < FG_TARGETS; ++t)
            if ((reads[k] | writes[k]) & T(t)) {
                if (first[t] < 0) first[t] = k;
                last[t] = k;
            }
    /* (the light, the second and the history are three buffers whose parts rotate from pass to pass, not three targets:
     * whichever of them a pass touches, each buffer lives from the first to the last pass that touches any) */
    {
        int lo = -1, hi = -1;
        for (int t = FG_LIGHT; t <= FG_HISTORY; ++t)
            if (first[t] >= 0) {
                if (lo < 0 || first[t] < lo) lo = first[t];
                if (last[t] > hi) hi = last[t];
            }
        for (int t = FG_LIGHT; t <= FG_HISTORY; ++t)
            if (first[t] >= 0 || bytes[t]) {
                first[t] = lo;
                last[t] = hi;
            }
    }
    /* (what no pass writes and nothing keeps holds nothing this frame: a shader made for more than the frame does, as
     * the scene's that reads fog with no fog) */
    for (int t = 0; t < FG_TARGETS; ++t) {
        bool written = false;
        for (int k = 0; k < count; ++k) written = written || (writes[k] & T(t));
        if (!written && !persists[t]) first[t] = last[t] = -1;
    }
    /* aliasing: the targets that live only within the frame, largest first, each into the first heap none of whose
     * targets it overlaps (by passes), or a heap of its own; what is saved is what the heaps need less than the sum */
    int byBytes[FG_TARGETS], transient = 0;
    for (int t = 0; t < FG_TARGETS; ++t)
        if (!persists[t] && first[t] >= 0 && bytes[t]) byBytes[transient++] = t;
    for (int a = 1; a < transient; ++a)
        for (int b = a; b > 0 && bytes[byBytes[b - 1]] < bytes[byBytes[b]]; --b) {
            const int x = byBytes[b];
            byBytes[b] = byBytes[b - 1];
            byBytes[b - 1] = x;
        }
    int heapOf[FG_TARGETS], heaps = 0;
    uint64_t heapBytes[FG_TARGETS], sum = 0, all = 0;
    for (int t = 0; t < FG_TARGETS; ++t) all += bytes[t];
    for (int i = 0; i < transient; ++i) {
        const int t = byBytes[i];
        sum += bytes[t];
        int chosen = -1;
        for (int h = 0; h < heaps && chosen < 0; ++h) {
            bool apart = true;
            for (int j = 0; j < i && apart; ++j) {
                const int u = byBytes[j];
                if (heapOf[u] == h && !(last[u] < first[t] || last[t] < first[u])) apart = false;
            }
            if (apart) chosen = h;
        }
        if (chosen < 0) {
            chosen = heaps++;
            heapBytes[chosen] = 0;
        }
        heapOf[t] = chosen;
        if (bytes[t] > heapBytes[chosen]) heapBytes[chosen] = bytes[t];
    }
    uint64_t heapSum = 0;
    for (int h = 0; h < heaps; ++h) heapSum += heapBytes[h];
    const uint64_t saved = sum - heapSum;
    if (report) {
        fprintf(report, "%-24s %-60s %s\n", "pass", "reads", "writes");
        for (int k = 0; k < count; ++k) {
            char r_[256] = "", w_[256] = "";
            for (int t = 0; t < FG_TARGETS; ++t) {
                if (reads[k] & T(t)) { strcat(r_, targetNames[t]); strcat(r_, "; "); }
                if (writes[k] & T(t)) { strcat(w_, targetNames[t]); strcat(w_, "; "); }
            }
            fprintf(report, "%-24s %-60s %s\n", passes[order[k]].name, r_, w_);
        }
        fprintf(report, "\n%-26s %10s  %s\n", "target", "MB", "lives (passes)");
        for (int t = 0; t < FG_TARGETS; ++t) {
            if (!bytes[t] && first[t] < 0) continue;
            if (first[t] < 0) fprintf(report, "%-26s %10.1f  not used this frame\n", targetNames[t], (double)bytes[t] / 1048576.0);
            else fprintf(report, "%-26s %10.1f  %s%s to %s\n", targetNames[t], (double)bytes[t] / 1048576.0, persists[t] ? "(kept) " : "", passes[order[first[t]]].name, passes[order[last[t]]].name);
        }
        fprintf(report, "\nframe-sized targets: %.1f MB, %.1f MB of them living only within the frame; aliased, those take %.1f MB in %d heaps: %.1f MB saved, %.0f%% of the targets\n",
                (double)all / 1048576.0, (double)sum / 1048576.0, (double)heapSum / 1048576.0, heaps, (double)saved / 1048576.0, all ? 100.0 * (double)saved / (double)all : 0.0);
        uint32_t barriers = 0, dispatches = 0;
        artfVulkanPostBarriers(r, &barriers, &dispatches);
        fprintf(report, "post passes: %u dispatches, %u barriers between them\n", dispatches, barriers);
        uint64_t idle = 0;
        for (int t = 0; t < FG_TARGETS; ++t)
            if (first[t] < 0 && !persists[t]) idle += bytes[t];
        fprintf(report, "made, and not used this frame (an effect that is off): %.1f MB, %.0f%% of the targets\n", (double)idle / 1048576.0, all ? 100.0 * (double)idle / (double)all : 0.0);
    }
    if (dot) {
        fprintf(dot, "digraph frame {\n  rankdir=LR;\n  node [fontname=\"Helvetica\", fontsize=10];\n");
        for (int t = 0; t < FG_TARGETS; ++t)
            if (first[t] >= 0) fprintf(dot, "  t%d [shape=box, style=\"rounded%s\", label=\"%s\\n%.1f MB\"];\n", t, persists[t] ? ",bold" : "", targetNames[t], (double)bytes[t] / 1048576.0);
        for (int k = 0; k < count; ++k) {
            fprintf(dot, "  p%d [shape=ellipse, label=\"%s\"];\n", k, passes[order[k]].name);
            if (k) fprintf(dot, "  p%d -> p%d [style=dotted];\n", k - 1, k);
            for (int t = 0; t < FG_TARGETS; ++t) {
                if (reads[k] & T(t)) fprintf(dot, "  t%d -> p%d;\n", t, k);
                if (writes[k] & T(t)) fprintf(dot, "  p%d -> t%d [color=red];\n", k, t);
            }
        }
        fprintf(dot, "}\n");
    }
    return saved;
}
