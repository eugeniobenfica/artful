/* artful — a background pass: light behind the scene that a program makes (artfSceneSetBackgroundPass). The scene
 * keeps what it was given, and a frame's block, the same for both backends: what the pass's frame wrote, and the
 * levels of its bloom. The light, its bloom and its tone curve are each backend's. */
#include "scene/sceneInternal.h"

void artfSceneSetBackgroundPass(ArtfScene* s, const ArtfBackgroundPass* pass) {
    ArtfBackground* b = &s->backgroundPass;
    const ArtfBackgroundPass before = b->pass;
    const bool was = b->on;
    if (was && before.release && (!pass || pass->user != before.user)) before.release(before.user);
    if (!pass) {
        b->on = false;
        memset(&b->pass, 0, sizeof b->pass);
        artfFree(b->spirv);
        b->spirv = NULL;
        ++b->moduleStamp;
        ++b->dataStamp;
        ++b->streamStamp;
        return;
    }
    b->on = true;
    b->pass = *pass;
    /* (the shader is copied, and its stamp moves only when it is another) */
    const size_t bytes = pass->spirv ? pass->spirvBytes & ~(size_t)3 : 0;
    const bool sameModule = was && bytes == before.spirvBytes && (!bytes || !memcmp(b->spirv, pass->spirv, bytes));
    if (!sameModule) {
        artfFree(b->spirv);
        b->spirv = NULL;
        if (bytes) {
            b->spirv = artfMalloc(bytes);
            memcpy(b->spirv, pass->spirv, bytes);
        }
        ++b->moduleStamp;
    }
    b->pass.spirv = b->spirv;
    b->pass.spirvBytes = bytes;
    if (!was || pass->user != before.user || pass->data != before.data || pass->dataBytes != before.dataBytes || pass->dataVersion != before.dataVersion) ++b->dataStamp;
    if (!was || pass->user != before.user || pass->stream != before.stream || pass->streamBytes != before.streamBytes || pass->streamVersion != before.streamVersion) ++b->streamStamp;
}

const ArtfBackgroundPass* artfSceneBackgroundPass(const ArtfScene* s) { return s->backgroundPass.on ? &s->backgroundPass.pass : NULL; }

void artfBackgroundFini(ArtfScene* s) { artfSceneSetBackgroundPass(s, NULL); }

void artfBackgroundFrame(const ArtfScene* s, int width, int height, ArtfBackgroundBlock* out, uint32_t* bloomPixels) {
    const ArtfBackgroundPass* p = &s->backgroundPass.pass;
    memset(out, 0, sizeof *out);
    if (p->frame) p->frame(p->user, &s->camera, width, height, out->params);
    out->size[0] = (uint32_t)width;
    out->size[1] = (uint32_t)height;
    out->image[0] = p->exposure;
    out->image[1] = p->bloom;
    uint32_t w = (uint32_t)width, h = (uint32_t)height, at = 0, levels = 0;
    /* (there is always one, however small the frame: tone mapping reads it) */
    while (levels < ARTF_BLOOM_LEVELS && (!levels || (w > 3 && h > 3))) {
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        out->bloom[levels][0] = at;
        out->bloom[levels][1] = w;
        out->bloom[levels][2] = h;
        at += w * h;
        ++levels;
    }
    out->size[2] = levels;
    *bloomPixels = at;
}
