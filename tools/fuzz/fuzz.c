/* artful-fuzz-* — libFuzzer targets, one for each reader of what comes from outside: a target is this file built with
 * ARTF_FUZZ set to its name (CMake's ARTFUL_FUZZ, with clang). Each reads the bytes it is given as its reader would read
 * a file, and gives back all it made: under AddressSanitizer and UndefinedBehaviorSanitizer, a read past what was given,
 * a leak or an overflow is a finding. What a reader turns down is no finding; logging is off.
 *
 *   gltf     artfSceneLoadModelMemory, with no folder (so no other file is opened), then the scene freed
 *   image    artfImageDecode: PNG and JPEG
 *   hdr      artfImageDecodeHdr: Radiance's RGBE
 *   ktx2     artfSceneAddTextureKtx2, and its blocks made pixels (artfSceneDecodeTextures): BC1, BC3, BC7, ETC2, ASTC
 *   blocks   one block alone: BC7, or ASTC of a footprint the first byte says
 *   ies      artfIesDecode: a light's profile
 *   cube     artfLutDecodeCube: a grading table
 *   meshopt  EXT_meshopt_compression's buffers: the first bytes say which, how many and how wide
 *
 * Seeds: tools/fuzz/seeds.py writes a few of each from what the tests make; tools/fuzz/gltf.dict is a dictionary of
 * glTF's names. Run as: artful-fuzz-gltf -dict=tools/fuzz/gltf.dict corpus/gltf seeds/gltf */

#include "artful/artful.h"
#include "scene/sceneInternal.h"

#include <stdint.h>
#include <string.h>

static void quiet(void* user, const char* message) {
    (void)user;
    (void)message;
}

int LLVMFuzzerInitialize(int* argc, char*** argv) {
    (void)argc;
    (void)argv;
    artfSetLog(quiet, NULL);
    return 0;
}

#define FUZZ_IS(name) (strcmp(ARTF_FUZZ_NAME, name) == 0)
#define STR_(x) #x
#define STR(x) STR_(x)
#define ARTF_FUZZ_NAME STR(ARTF_FUZZ)

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (FUZZ_IS("gltf")) {
        ArtfScene* s = artfSceneNew();
        artfSceneLoadModelMemory(s, data, size, NULL, NULL);
        artfSceneFree(s);
    } else if (FUZZ_IS("image")) {
        int w = 0, h = 0;
        artfFree(artfImageDecode(data, size, &w, &h));
    } else if (FUZZ_IS("hdr")) {
        int w = 0, h = 0;
        artfFree(artfImageDecodeHdr(data, size, &w, &h));
    } else if (FUZZ_IS("ktx2")) {
        ArtfScene* s = artfSceneNew();
        if (artfSceneAddTextureKtx2(s, data, size)) artfSceneDecodeTextures(s);
        artfSceneFree(s);
    } else if (FUZZ_IS("blocks")) {
        static const uint8_t footprints[][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
        if (size < 17) return 0;
        uint32_t out[12 * 12];
        const uint8_t which = data[0] % 15;
        if (which == 14) artfDecodeBc7(data + 1, out);
        else artfDecodeAstc(data + 1, footprints[which][0], footprints[which][1], out);
    } else if (FUZZ_IS("ies")) {
        int v = 0, h = 0;
        artfFree(artfIesDecode(data, size, &v, &h));
    } else if (FUZZ_IS("cube")) {
        int n = 0;
        artfFree(artfLutDecodeCube(data, size, &n));
    } else if (FUZZ_IS("meshopt")) {
        if (size < 4) return 0;
        const size_t count = ((size_t)data[1] | (size_t)data[2] << 8) % 4096, mode = data[0] % 3;
        if (mode == 0) {
            const size_t stride = 4 * (1 + data[3] % 64);
            uint8_t* out = artfMalloc(count * stride + 1);
            artfMeshoptDecodeVertices(out, count, stride, data + 4, size - 4);
            artfFree(out);
        } else {
            const size_t indexSize = data[3] & 1 ? 4 : 2, n = mode == 1 ? count / 3 * 3 : count;
            uint8_t* out = artfMalloc(n * indexSize + 1);
            if (mode == 1) artfMeshoptDecodeTriangles(out, n, indexSize, data + 4, size - 4);
            else artfMeshoptDecodeIndices(out, n, indexSize, data + 4, size - 4);
            artfFree(out);
        }
    }
    return 0;
}
