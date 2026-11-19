/* artful-flip: how far apart two pictures look, by FLIP (tools/flip.h).
 *
 *   artful-flip reference.png test.png [--ppd n] [--max-mean x] [--max-p99 x]
 *
 * PNG and JPEG are taken as a screen shows them (sRGB); Radiance .hdr files as light, through HDR-FLIP. Prints the
 * mean, the 99th percentile and the most of the error, 0 alike to about 1. With --max-mean or --max-p99, exits 1 when
 * the picture is further than that: a gate for pictures kept to compare against. */
#include "artful/artful.h"
#include "flip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void* readAll(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* data = n > 0 ? malloc((size_t)n) : NULL;
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = (size_t)(n > 0 ? n : 0);
    return data;
}

static bool isHdr(const char* path) {
    const size_t n = strlen(path);
    return n > 4 && (!strcmp(path + n - 4, ".hdr") || !strcmp(path + n - 4, ".HDR"));
}

int main(int argc, char** argv) {
    const char* paths[2] = {NULL, NULL};
    float ppd = 0;
    double maxMean = -1, maxP99 = -1;
    int count = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--ppd") && i + 1 < argc) ppd = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-mean") && i + 1 < argc) maxMean = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max-p99") && i + 1 < argc) maxP99 = atof(argv[++i]);
        else if (count < 2) paths[count++] = argv[i];
        else count = 3;
    }
    if (count != 2) {
        fprintf(stderr, "artful-flip reference test [--ppd n] [--max-mean x] [--max-p99 x]\n");
        return 2;
    }
    const bool hdr = isHdr(paths[0]);
    if (hdr != isHdr(paths[1])) {
        fprintf(stderr, "both pictures must be .hdr, or neither\n");
        return 2;
    }
    int w[2], h[2];
    void* pixels[2] = {NULL, NULL};
    for (int k = 0; k < 2; ++k) {
        size_t size;
        void* data = readAll(paths[k], &size);
        if (data) pixels[k] = hdr ? (void*)artfImageDecodeHdr(data, size, &w[k], &h[k]) : (void*)artfImageDecode(data, size, &w[k], &h[k]);
        free(data);
        if (!pixels[k]) {
            fprintf(stderr, "%s: cannot be read\n", paths[k]);
            return 2;
        }
    }
    if (w[0] != w[1] || h[0] != h[1]) {
        fprintf(stderr, "the pictures differ in size: %d x %d and %d x %d\n", w[0], h[0], w[1], h[1]);
        return 2;
    }
    const FlipStats s = hdr ? flipHdr(pixels[0], pixels[1], w[0], h[0], ppd, NULL) : flipFrames(pixels[0], pixels[1], w[0], h[0], ppd, NULL);
    printf("mean %.6f  p99 %.6f  max %.6f\n", s.mean, s.p99, s.max);
    artfFree(pixels[0]);
    artfFree(pixels[1]);
    if ((maxMean >= 0 && s.mean > maxMean) || (maxP99 >= 0 && s.p99 > maxP99)) return 1;
    return 0;
}
