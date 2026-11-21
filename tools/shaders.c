/* artful-shaders — writes the GPU's shaders to a folder, for spirv-val and spirv-dis.
 *
 *   artful-shaders folder [--ir]: and each one's IR as text (name.ir), as Metal (name.metal, shaderir/msl.h) and as
 *   WGSL (name.wgsl, shaderir/wgsl.h)
 *   artful-shaders folder --debug: with debugging information, pointing at the C that builds each (irDebugInfo) */
#include "scene/sceneInternal.h"
#include "vulkan/shaders.h"
#include "holeInternal.h" /* (extensions/blackhole) */
#include "2d/vulkan2d.h"
#include "vulkan/spirv.h"
#include "shaderir/msl.h"
#include "shaderir/wgsl.h"

#include <stdio.h>
#include <string.h>

static const char* folderOut;
static const char* current; /* the shader being made */
static bool withIr;

static void finished(const IrModule* m) {
    if (!withIr || !current) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.ir", folderOut, current);
    FILE* f = fopen(path, "w");
    if (f) {
        irPrint(m, f);
        fclose(f);
    }
    /* and as Metal: as metal.c has it written, the shadow's vertices upside down */
    char why[256];
    const IrMslOptions options = {!strcmp(current, "shadowVertex")};
    char* metal = irMsl(m, &options, NULL, why, sizeof why);
    snprintf(path, sizeof path, "%s/%s.metal", folderOut, current);
    if (!metal) {
        fprintf(stderr, "%s: no Metal: %s\n", current, why);
        remove(path);
        return;
    }
    f = fopen(path, "w");
    if (f) {
        fputs(metal, f);
        fclose(f);
    }
    artfFree(metal);
    /* and as WGSL, the same way up */
    const IrWgslOptions wgslOptions = {!strcmp(current, "shadowVertex")};
    char* wgsl = irWgsl(m, &wgslOptions, NULL, why, sizeof why);
    snprintf(path, sizeof path, "%s/%s.wgsl", folderOut, current);
    if (!wgsl) {
        fprintf(stderr, "%s: no WGSL: %s\n", current, why);
        remove(path);
        return;
    }
    f = fopen(path, "w");
    if (f) {
        fputs(wgsl, f);
        fclose(f);
    }
    artfFree(wgsl);
}

static int save(const char* folder, const char* name, uint32_t* code, size_t size) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.spv", folder, name);
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    fwrite(code, 1, size, f);
    fclose(f);
    artfFree(code);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { fputs("artful-shaders folder\n", stderr); return 2; }
    artfSpvCheck = true; /* (each module's IR checked too: irCheck) */
    folderOut = argv[1];
    withIr = argc > 2 && !strcmp(argv[2], "--ir");
    artfShaderDebugInfo = argc > 2 && !strcmp(argv[2], "--debug");
    artfSpvFinished = finished;
    size_t size;
    uint32_t* code;
    int failed = 0;
#define SAVE(name, call) current = name; code = call; failed |= save(argv[1], name, code, size)
    SAVE("2dTiles", artf2DShaderTiles(&size));
    SAVE("2dBloomDown", artf2DShaderBloomDown(&size));
    SAVE("2dBloomUp", artf2DShaderBloomUp(&size));
    SAVE("2dFinish", artf2DShaderFinish(&size));
    SAVE("2dReflect", artf2DShaderReflect(&size));
    SAVE("2dGiScene", artf2DShaderGiScene(&size));
    SAVE("2dGiRows", artf2DShaderGiDistance(&size, false));
    SAVE("2dGiColumns", artf2DShaderGiDistance(&size, true));
    SAVE("2dGiEven", artf2DShaderGiCascade(&size, false));
    SAVE("2dGiOdd", artf2DShaderGiCascade(&size, true));
    SAVE("2dGiGather", artf2DShaderGiGather(&size));
    SAVE("2dGiSpread0", artf2DShaderGiSpread(&size, 0));
    SAVE("2dGiSpread1", artf2DShaderGiSpread(&size, 1));
    SAVE("cullAll", artfShaderCull(&size, ARTF_CULL_ALL));
    SAVE("cullEarly", artfShaderCull(&size, ARTF_CULL_EARLY));
    SAVE("cullLate", artfShaderCull(&size, ARTF_CULL_LATE));
    SAVE("hizFirst", artfShaderHiz(&size, true, 1));
    SAVE("hizNext", artfShaderHiz(&size, false, 1));
    SAVE("sceneVertex", artfShaderSceneVertex(&size, false));
    SAVE("sceneVertexMotion", artfShaderSceneVertex(&size, true));
    SAVE("sceneFragment", artfShaderSceneFragment(&size, 1024, false, NULL, 0, 0, 0));
    SAVE("sceneCutout", artfShaderSceneFragment(&size, 1024, true, NULL, 0, 0, 0));
    SAVE("layerGrid", artfShaderSceneFragment(&size, 1024, true, NULL, 0, 1 | ARTF_LISTS_GRID, 0));
    SAVE("layerGrid4", artfShaderSceneFragment(&size, 1024, true, NULL, 0, 4 | ARTF_LISTS_GRID, 0));
    SAVE("layerGrid4Post", artfShaderSceneFragment(&size, 1024, true, NULL, 0, 4 | ARTF_LISTS_GRID | ARTF_LISTS_POST_DEPTH, 0));
    SAVE("layerSums4Post", artfShaderSceneFragment(&size, 1024, true, NULL, 0, 4 | ARTF_LISTS_POST_DEPTH, 0));
    SAVE("layerPrefix", artfShaderLayerPrefix(&size));
    SAVE("sceneFragmentProbes", artfShaderSceneFragment(&size, 1024, false, NULL, 0, 0, ARTF_SHADER_PROBES));
    SAVE("sceneFragmentFloats", artfShaderSceneFragment(&size, 1024, false, NULL, 0, 0, ARTF_SHADER_FLOATS | ARTF_SHADER_PROBES | ARTF_SHADER_WALLS | ARTF_SHADER_FROXELS));
    SAVE("sceneCutoutFloats", artfShaderSceneFragment(&size, 1024, true, NULL, 0, 0, ARTF_SHADER_FLOATS | ARTF_SHADER_PROBES | ARTF_SHADER_WALLS | ARTF_SHADER_FROXELS));
    SAVE("sceneFragmentCoat", artfShaderSceneFragment(&size, 1024, false, NULL, 0, 0, ARTF_SHADER_FLOATS | ARTF_SHADER_PROBES | ARTF_SHADER_COAT));
    /* (without non-uniform indexing: textures down a tree, by constant indices) */
    SAVE("sceneFragmentUniform", artfShaderSceneFragment(&size, 64 | ARTF_TEXTURES_UNIFORM, false, NULL, 0, 0, ARTF_SHADER_FLOATS | ARTF_SHADER_PROBES | ARTF_SHADER_COAT));
    SAVE("sceneCutoutUniform", artfShaderSceneFragment(&size, 64 | ARTF_TEXTURES_UNIFORM, true, NULL, 0, 0, 0));
    SAVE("shadowFragmentUniform", artfShaderShadowFragment(&size, 64 | ARTF_TEXTURES_UNIFORM));
    SAVE("traceMoreUniform", artfShaderTrace(&size, 64 | ARTF_TEXTURES_UNIFORM, NULL, 0, false, false, true));
    SAVE("probesUniform", artfShaderProbes(&size, 64 | ARTF_TEXTURES_UNIFORM, NULL, 0, false, true, true, 64, .9f));
    SAVE("sceneFragmentSoft", artfShaderSceneFragment(&size, 1024, true, NULL, 0, 0, ARTF_SHADER_FLOATS | ARTF_SHADER_COAT | ARTF_SHADER_SOFT));
    SAVE("sceneFragmentSoftPlain", artfShaderSceneFragment(&size, 1024, false, NULL, 0, 0, ARTF_SHADER_SOFT));
    SAVE("sceneFragmentFloatsPlain", artfShaderSceneFragment(&size, 1024, false, NULL, 0, 0, ARTF_SHADER_FLOATS));
    SAVE("sceneFragmentView", artfShaderSceneFragment(&size, 1024, false, NULL, 0, 0, ARTF_SHADER_VIEW));
    SAVE("sceneCutoutView", artfShaderSceneFragment(&size, 1024, true, NULL, 0, 0, ARTF_SHADER_VIEW | ARTF_SHADER_MESHLETS));
    SAVE("sceneFragmentViewFloats", artfShaderSceneFragment(&size, 1024, false, NULL, 0, 0, ARTF_SHADER_FLOATS | ARTF_SHADER_PROBES | ARTF_SHADER_COAT | ARTF_SHADER_VIEW | ARTF_SHADER_MESHLETS));
    SAVE("sceneLayerListView", artfShaderSceneFragment(&size, 1024, true, NULL, 0, 4, ARTF_SHADER_VIEW | ARTF_SHADER_MESHLETS));
    SAVE("overdraw", artfShaderOverdraw(&size, false));
    SAVE("sceneFragmentTraced", artfShaderSceneFragment(&size, 1024, false, NULL, 0, 0, ARTF_SHADER_TRACED));
    SAVE("sceneCutoutTracedHardware", artfShaderSceneFragment(&size, 1024, true, NULL, 0, 0, ARTF_SHADER_FLOATS | ARTF_SHADER_COAT | ARTF_SHADER_TRACED | ARTF_SHADER_RAY_QUERY));
    SAVE("sceneLayerListTraced", artfShaderSceneFragment(&size, 1024, true, NULL, 0, 4, ARTF_SHADER_TRACED | ARTF_SHADER_RAY_QUERY));
    SAVE("sceneFragmentShapes", artfShaderSceneFragment(&size, 1024, false, NULL, 0, 0, ARTF_SHADER_SHAPES));
    SAVE("sceneFragmentShapesCoat", artfShaderSceneFragment(&size, 1024, true, NULL, 0, 0, ARTF_SHADER_FLOATS | ARTF_SHADER_COAT | ARTF_SHADER_SOFT | ARTF_SHADER_SHAPES));
    SAVE("sceneLayerListShapes", artfShaderSceneFragment(&size, 1024, true, NULL, 0, 1, ARTF_SHADER_COAT | ARTF_SHADER_SHAPES));
    SAVE("sceneLayerList", artfShaderSceneFragment(&size, 1024, true, NULL, 0, 1, 0));
    SAVE("sceneLayerListSamples", artfShaderSceneFragment(&size, 1024, true, NULL, 0, 4, 0));
    SAVE("layerResolve", artfShaderLayerResolve(&size, 1));
    SAVE("layerResolveSamples", artfShaderLayerResolve(&size, 4));
    SAVE("trace", artfShaderTrace(&size, 1024, NULL, 0, false, false, false));
    SAVE("traceHardware", artfShaderTrace(&size, 1024, NULL, 0, true, true, false));
    SAVE("traceGuides", artfShaderTrace(&size, 1024, NULL, 0, true, false, false));
    SAVE("traceMore", artfShaderTrace(&size, 1024, NULL, 0, false, false, true));
    SAVE("traceMoreHardware", artfShaderTrace(&size, 1024, NULL, 0, true, true, true));
    SAVE("probes", artfShaderProbes(&size, 1024, NULL, 0, false, false, true, 64, .9f));
    SAVE("probesHardware", artfShaderProbes(&size, 1024, NULL, 0, true, false, true, 64, .9f));
    SAVE("probesMoreThroughWalls", artfShaderProbes(&size, 1024, NULL, 0, true, true, false, 100, .5f));
    SAVE("skin", artfShaderSkin(&size));
    SAVE("swarm", artfShaderSwarm(&size));
    SAVE("meshletCull", artfShaderMeshletCull(&size, ARTF_CULL_ALL));
    SAVE("meshletCullEarly", artfShaderMeshletCull(&size, ARTF_CULL_EARLY));
    SAVE("meshletCullLate", artfShaderMeshletCull(&size, ARTF_CULL_LATE));
    SAVE("shadowCull", artfShaderShadowCull(&size));
    SAVE("denoiseLight", artfShaderDenoiseLight(&size));
    SAVE("denoiseTime", artfShaderDenoiseTime(&size));
    SAVE("denoiseVariance", artfShaderDenoiseVariance(&size));
    SAVE("denoiseSpace0", artfShaderDenoiseSpace(&size, 0));
    SAVE("denoiseSpace1", artfShaderDenoiseSpace(&size, 1));
    SAVE("denoiseSpace2", artfShaderDenoiseSpace(&size, 2));
    SAVE("denoiseSpace3", artfShaderDenoiseSpace(&size, 3));
    SAVE("denoiseSpace4", artfShaderDenoiseSpace(&size, 4));
    SAVE("denoiseEnd", artfShaderDenoiseEnd(&size));
    /* (with a material of every step, as spirv-val must see them too) */
    ArtfScene* scene = artfSceneNew();
    ArtfGraph* g = artfGraphNew();
    ArtfExpr uv = artfExprUv(g), p = artfExprPosition(g), n = artfExprNormal(g), eye = artfExprToEye(g), base = artfExprBaseColor(g), param = artfExprParam(g, 2);
    ArtfExpr t = artfExprTexture(g, 0, uv);
    ArtfExpr sum = artfExprAdd(g, artfExprSub(g, artfExprMul(g, p, n), artfExprDiv(g, eye, param)), artfExprMin(g, base, artfExprMax(g, t, artfExprNumber(g, .1f))));
    ArtfExpr round = artfExprMix(g, artfExprFloor(g, sum), artfExprFract(g, sum), artfExprAbs(g, artfExprSqrt(g, sum)));
    ArtfExpr wave = artfExprIf(g, artfExprSin(g, round), artfExprCos(g, round), artfExprDot(g, round, sum));
    ArtfExpr v = artfExprVec(g, artfExprX(g, wave), artfExprY(g, wave), artfExprZ(g, wave));
    artfGraphSetOutput(g, ARTF_OUT_COLOR, v);
    artfGraphSetOutput(g, ARTF_OUT_EMISSIVE, v);
    artfGraphSetOutput(g, ARTF_OUT_ROUGHNESS, artfExprW(g, wave));
    artfGraphSetOutput(g, ARTF_OUT_METALLIC, wave);
    artfGraphSetOutput(g, ARTF_OUT_ALPHA, wave);
    artfGraphSetOutput(g, ARTF_OUT_NORMAL, v);
    artfSceneAddGraph(scene, g);
    artfGraphFree(g);
    SAVE("graphFragment", artfShaderSceneFragment(&size, 1024, true, scene->graphs.items, 1, 0, 0));
    SAVE("graphTrace", artfShaderTrace(&size, 1024, scene->graphs.items, 1, false, false, true));
    SAVE("graphFragmentTraced", artfShaderSceneFragment(&size, 1024, true, scene->graphs.items, 1, 0, ARTF_SHADER_FLOATS | ARTF_SHADER_TRACED));
    SAVE("graphFragmentPlainTraced", artfShaderSceneFragment(&size, 1024, false, scene->graphs.items, 1, 0, ARTF_SHADER_TRACED | ARTF_SHADER_RAY_QUERY));
    SAVE("graphReflectionTraced", artfShaderPostReflectionTraced(&size, 1024, scene->graphs.items, 1, true));
    SAVE("graphProbes", artfShaderProbes(&size, 1024, scene->graphs.items, 1, false, true, true, 64, .9f));
    SAVE("graphFragmentMore", artfShaderSceneFragment(&size, 1024, true, scene->graphs.items, 1, 0, ARTF_SHADER_FLOATS | ARTF_SHADER_COAT | ARTF_SHADER_SHAPES | ARTF_SHADER_SOFT));
    artfSceneFree(scene);
    SAVE("shadowVertex", artfShaderShadowVertex(&size));
    SAVE("shadowFragment", artfShaderShadowFragment(&size, 1024));
    SAVE("traceShow", artfShaderTraceShow(&size, false));
    SAVE("overlayVertex", artfShaderOverlayVertex(&size));
    SAVE("overlayFragment", artfShaderOverlayFragment(&size, false));
    SAVE("holeTrace", artfBlackHoleShader(&size)); /* (the extension's: extensions/blackhole) */
    SAVE("bloomFirst", artfShaderBloomDown(&size, true));
    SAVE("bloomNext", artfShaderBloomDown(&size, false));
    SAVE("bloomUp", artfShaderBloomUp(&size));
    SAVE("backgroundBackdrop", artfShaderBackgroundBackdrop(&size, false));
    SAVE("environmentBackdrop", artfShaderEnvironmentBackdrop(&size, false));
    SAVE("postGather", artfShaderPostGather(&size, 1));
    SAVE("postGatherSamples", artfShaderPostGather(&size, 4));
    SAVE("postOcclusion", artfShaderPostOcclusion(&size));
    SAVE("postOcclusionBlur", artfShaderPostOcclusionBlur(&size));
    SAVE("postReflection", artfShaderPostReflection(&size));
    SAVE("postReflectionApply", artfShaderPostReflectionApply(&size));
    SAVE("postReflectionTraced", artfShaderPostReflectionTraced(&size, 1024, NULL, 0, false));
    SAVE("postReflectionTracedHardware", artfShaderPostReflectionTraced(&size, 1024, NULL, 0, true));
    SAVE("postFroxels", artfShaderPostFroxels(&size));
    SAVE("fogBackdrop", artfShaderFogBackdrop(&size));
    SAVE("postTemporal", artfShaderPostTemporal(&size));
    SAVE("postFocus", artfShaderPostFocus(&size));
    SAVE("postBloomDown", artfShaderPostBloomDown(&size));
    SAVE("postBloomUp", artfShaderPostBloomUp(&size));
    SAVE("postFinal", artfShaderPostFinal(&size, false));
    SAVE("postMeter", artfShaderPostMeter(&size));
    SAVE("postMotionBlur", artfShaderPostMotionBlur(&size));
    SAVE("postVelocity", artfShaderPostVelocity(&size));
    SAVE("postTileMost", artfShaderPostTileMost(&size));
    SAVE("postUpscale", artfShaderPostUpscale(&size));
    SAVE("postSharpen", artfShaderPostSharpen(&size));
    SAVE("postSampled", artfShaderPostSampled(&size));
    SAVE("postSampledSpace", artfShaderPostSampledSpace(&size));
    SAVE("postCloudColumns", artfShaderPostCloudColumns(&size));
    SAVE("postCloudApply", artfShaderPostCloudApply(&size));
    SAVE("postUnderAcross", artfShaderPostUnderAcross(&size));
    SAVE("postUnderDown", artfShaderPostUnderDown(&size));
    if (artfSpvChecksFailed) fprintf(stderr, "%d shaders' IR not sound\n", artfSpvChecksFailed);
    return failed || artfSpvChecksFailed;
}
