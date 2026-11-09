/* artful-shaders — writes the GPU's shaders to a folder, for spirv-val and spirv-dis.
 *
 *   artful-shaders folder */
#include "scene/sceneInternal.h"
#include "vulkan/shaders.h"

#include <stdio.h>

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
    size_t size;
    uint32_t* code;
    int failed = 0;
#define SAVE(name, call) code = call; failed |= save(argv[1], name, code, size)
    SAVE("cullAll", artfShaderCull(&size, ARTF_CULL_ALL));
    SAVE("cullEarly", artfShaderCull(&size, ARTF_CULL_EARLY));
    SAVE("cullLate", artfShaderCull(&size, ARTF_CULL_LATE));
    SAVE("hizFirst", artfShaderHiz(&size, true, 1));
    SAVE("hizNext", artfShaderHiz(&size, false, 1));
    SAVE("sceneVertex", artfShaderSceneVertex(&size));
    SAVE("sceneFragment", artfShaderSceneFragment(&size, 1024, false, NULL, 0, 0, 0));
    SAVE("sceneCutout", artfShaderSceneFragment(&size, 1024, true, NULL, 0, 0, 0));
    SAVE("sceneFragmentProbes", artfShaderSceneFragment(&size, 1024, false, NULL, 0, 0, ARTF_SHADER_PROBES));
    SAVE("sceneFragmentFloats", artfShaderSceneFragment(&size, 1024, false, NULL, 0, 0, ARTF_SHADER_FLOATS | ARTF_SHADER_PROBES | ARTF_SHADER_WALLS | ARTF_SHADER_FROXELS));
    SAVE("sceneCutoutFloats", artfShaderSceneFragment(&size, 1024, true, NULL, 0, 0, ARTF_SHADER_FLOATS | ARTF_SHADER_PROBES | ARTF_SHADER_WALLS | ARTF_SHADER_FROXELS));
    SAVE("sceneFragmentFloatsPlain", artfShaderSceneFragment(&size, 1024, false, NULL, 0, 0, ARTF_SHADER_FLOATS));
    SAVE("sceneLayerList", artfShaderSceneFragment(&size, 1024, true, NULL, 0, 1, 0));
    SAVE("sceneLayerListSamples", artfShaderSceneFragment(&size, 1024, true, NULL, 0, 4, 0));
    SAVE("layerResolve", artfShaderLayerResolve(&size, 1));
    SAVE("layerResolveSamples", artfShaderLayerResolve(&size, 4));
    SAVE("trace", artfShaderTrace(&size, 1024, NULL, 0, false));
    SAVE("traceGuides", artfShaderTrace(&size, 1024, NULL, 0, true));
    SAVE("skin", artfShaderSkin(&size));
    SAVE("meshletCull", artfShaderMeshletCull(&size, ARTF_CULL_ALL));
    SAVE("meshletCullEarly", artfShaderMeshletCull(&size, ARTF_CULL_EARLY));
    SAVE("meshletCullLate", artfShaderMeshletCull(&size, ARTF_CULL_LATE));
    SAVE("shadowCull", artfShaderShadowCull(&size));
    SAVE("denoiseLight", artfShaderDenoiseLight(&size));
    SAVE("denoiseTime", artfShaderDenoiseTime(&size));
    SAVE("denoiseSpace0", artfShaderDenoiseSpace(&size, 0));
    SAVE("denoiseSpace1", artfShaderDenoiseSpace(&size, 1));
    SAVE("denoiseSpace2", artfShaderDenoiseSpace(&size, 2));
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
    SAVE("graphTrace", artfShaderTrace(&size, 1024, scene->graphs.items, 1, false));
    artfSceneFree(scene);
    SAVE("shadowVertex", artfShaderShadowVertex(&size));
    SAVE("shadowFragment", artfShaderShadowFragment(&size, 1024));
    SAVE("traceShow", artfShaderTraceShow(&size, false));
    SAVE("overlayVertex", artfShaderOverlayVertex(&size));
    SAVE("overlayFragment", artfShaderOverlayFragment(&size));
    SAVE("holeTrace", artfShaderHoleTrace(&size));
    SAVE("bloomFirst", artfShaderBloomDown(&size, true));
    SAVE("bloomNext", artfShaderBloomDown(&size, false));
    SAVE("bloomUp", artfShaderBloomUp(&size));
    SAVE("holeBackdrop", artfShaderHoleBackdrop(&size, false));
    SAVE("environmentBackdrop", artfShaderEnvironmentBackdrop(&size, false));
    SAVE("postGather", artfShaderPostGather(&size, 1));
    SAVE("postGatherSamples", artfShaderPostGather(&size, 4));
    SAVE("postOcclusion", artfShaderPostOcclusion(&size));
    SAVE("postOcclusionBlur", artfShaderPostOcclusionBlur(&size));
    SAVE("postReflection", artfShaderPostReflection(&size));
    SAVE("postReflectionApply", artfShaderPostReflectionApply(&size));
    SAVE("postFroxels", artfShaderPostFroxels(&size));
    SAVE("fogBackdrop", artfShaderFogBackdrop(&size));
    SAVE("postTemporal", artfShaderPostTemporal(&size));
    SAVE("postFocus", artfShaderPostFocus(&size));
    SAVE("postBloomDown", artfShaderPostBloomDown(&size));
    SAVE("postBloomUp", artfShaderPostBloomUp(&size));
    SAVE("postFinal", artfShaderPostFinal(&size));
    return failed;
}
