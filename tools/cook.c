/* artful-cook — what is slow in loading a glTF, made once and kept in a file (the plan's phase 4.3): its images
 * decoded with their mipmaps, its meshes built with their meshlets and lighter levels. Reading the file then takes the
 * place of making them (artfPrepareModelCooked; artful-render --model file.glb --cooked file.cook).
 *
 *   artful-cook model.glb [out.cook]     (out: model.glb.cook)
 *
 * It says how long making took, how long reading the file takes, and whether the two scenes hold the same. A forest's
 * merged meshes are cooked by artful-render --scene forest --hlod --cook file (artfSceneSaveHlod). */

#include "artful/artful.h"
#include "scene/sceneInternal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void* readAll(const char* path, size_t* size) {
    *size = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* data = malloc(length > 0 ? (size_t)length : 1);
    *size = fread(data, 1, length > 0 ? (size_t)length : 0, f);
    fclose(f);
    return data;
}

/* The same meshes and textures, to the byte (what a cooked model must give). */
static bool same(const ArtfScene* a, const ArtfScene* b) {
    if (a->meshes.len != b->meshes.len || a->textures.len != b->textures.len || a->instances.len != b->instances.len) return false;
    for (size_t k = 0; k < a->meshes.len; ++k) {
        const ArtfMeshData *p = &a->meshes.items[k], *q = &b->meshes.items[k];
        if (p->vertexCount != q->vertexCount || p->indexCount != q->indexCount || p->meshletCount != q->meshletCount || p->lodCount != q->lodCount) return false;
        if (p->vertexCount && memcmp(p->vertices, q->vertices, (size_t)p->vertexCount * sizeof *p->vertices)) return false;
        if (p->indexCount && memcmp(p->indices, q->indices, (size_t)p->indexCount * 4)) return false;
        if (p->meshletCount && memcmp(p->meshlets, q->meshlets, (size_t)p->meshletCount * sizeof *p->meshlets)) return false;
    }
    for (size_t k = 1; k < a->textures.len; ++k) {
        const ArtfTextureData *p = &a->textures.items[k], *q = &b->textures.items[k];
        if (p->width != q->width || p->height != q->height || p->mipCount != q->mipCount || p->light != q->light) return false;
        if (p->pixels && q->pixels && memcmp(p->pixels, q->pixels, (size_t)(p->offsets[p->mipCount - 1] + 1) * 4)) return false;
    }
    return !memcmp(a->instances.items, b->instances.items, a->instances.len * sizeof *a->instances.items);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fputs("artful-cook model.glb [out.cook]\n", stderr);
        return 2;
    }
    char out[1024];
    snprintf(out, sizeof out, "%s.cook", argv[1]);
    if (argc > 2) snprintf(out, sizeof out, "%s", argv[2]);
    char folder[1024];
    snprintf(folder, sizeof folder, "%s", argv[1]);
    char* slash = strrchr(folder, '/');
    char* back = strrchr(folder, '\\');
    if (back && (!slash || back > slash)) slash = back;
    if (slash) *slash = 0;
    else strcpy(folder, ".");
    size_t size = 0;
    void* data = readAll(argv[1], &size);
    if (!data) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    double start = artfNowMs();
    ArtfPreparedModel* made = artfPrepareModelMemory(data, size, folder);
    const double makeMs = artfNowMs() - start;
    if (!made) {
        fprintf(stderr, "not a glTF Artful reads: %s\n", argv[1]);
        return 1;
    }
    size_t cookedSize = 0;
    void* cooked = artfPreparedModelSave(made, &cookedSize);
    FILE* f = fopen(out, "wb");
    if (!f || fwrite(cooked, 1, cookedSize, f) != cookedSize) {
        fprintf(stderr, "cannot write %s\n", out);
        return 1;
    }
    fclose(f);
    start = artfNowMs();
    ArtfPreparedModel* read = artfPrepareModelCooked(data, size, folder, cooked, cookedSize);
    const double readMs = artfNowMs() - start;
    ArtfScene* a = artfSceneNew();
    ArtfScene* b = artfSceneNew();
    artfSceneAddPreparedModel(a, made, NULL);
    artfSceneAddPreparedModel(b, read, NULL);
    const bool alike = same(a, b);
    printf("%s: %.1f MB; made in %.1f ms, read in %.1f ms (%.0f%%); %s\n", out, (double)cookedSize / 1048576.0, makeMs, readMs, makeMs > 0 ? 100.0 * readMs / makeMs : 0.0,
           alike ? "the same scene" : "NOT THE SAME SCENE");
    artfSceneFree(a);
    artfSceneFree(b);
    artfFree(cooked);
    free(data);
    return alike ? 0 : 1;
}
