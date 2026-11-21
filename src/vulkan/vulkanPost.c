/* artful — the Vulkan backend: what comes after a frame in floats, recorded in post.c's order (the post passes, their
 * barriers from what each shader reads and writes, the parts the light buffers play). Part of vulkan.c, split by what
 * it does. */
#include "vulkan/vulkanInternal.h"

/* ---- what comes after a frame in floats: post.c's order ---- */

/* The end of a pass, on the GPU's clock: what it took is from the mark before. */
void passMark(Vulkan* v, VkCommandBuffer c, Slot* s, const char* name) {
    if (!s->timed || s->passCount >= ARTF_PASS_TIMES) return;
    const uint32_t slot = (uint32_t)(s - v->slots);
    v->vkCmdWriteTimestamp(c, STAGE_BOTTOM, v->timestamps, slot * QUERIES + 1 + s->passCount);
    s->passNames[s->passCount++] = name;
}


void postBarrier(Vulkan* v, VkCommandBuffer c) {
    memoryBarrier(v, c, STAGE_COMPUTE, STAGE_COMPUTE | STAGE_TRANSFER, ACCESS_SHADER_WRITE, ACCESS_SHADER_READ | ACCESS_SHADER_WRITE | ACCESS_TRANSFER_READ);
    v->pendingReads = v->pendingWrites = 0;
    ++v->postBarriers;
}

/* A post pass's dispatch. index: its pipeline's place in postPipelines, whose reads and writes are known; -1 one that is
 * not (traced reflections'): a barrier on each side of it. Outside postRecord, a barrier after each, as before. */
void postStepWith(Vulkan* v, VkCommandBuffer c, VkPipeline pipeline, int index, uint32_t push, uint32_t count) {
    uint32_t reads = ~0u, writes = ~0u;
    if (index >= 0) {
        reads = postPhysical(v, v->postReads[index]);
        writes = postPhysical(v, v->postWrites[index]);
    }
    const bool tracked = v->postTracking && index >= 0;
    if (v->postTracking && (((reads | writes) & v->pendingWrites) || (writes & v->pendingReads))) postBarrier(v, c);
    v->vkCmdBindPipeline(c, 1, pipeline);
    v->vkCmdPushConstants(c, v->pipelineLayout, SHADER_COMPUTE, 0, 4, &push);
    v->vkCmdDispatch(c, (count + 63) / 64, 1, 1);
    ++v->postDispatches;
    if (tracked) {
        v->pendingReads |= reads;
        v->pendingWrites |= writes;
    } else {
        postBarrier(v, c);
    }
}

void postStep(Vulkan* v, VkCommandBuffer c, int pipeline, uint32_t push, uint32_t count) { postStepWith(v, c, v->postPipelines[pipeline], pipeline, push, count); }

/* (a post pipeline's reads and writes as bits, by role: what writes to the parts' buffers rotates with them) */
uint32_t postPhysical(const Vulkan* v, uint32_t mask) {
    uint32_t out = mask & ~7u;
    for (int r = 0; r < 3; ++r)
        if (mask >> r & 1) out |= 1u << v->role[r];
    return out;
}

/* Set 1 as the parts are given now, for compute (1) or drawing (0). */
void rolesBind(Vulkan* v, VkCommandBuffer c, int point) {
    v->vkCmdBindDescriptorSets(c, point, v->pipelineLayout, 1, 1, &v->roleSets[v->role[0] * 9 + v->role[1] * 3 + v->role[2]], 0, NULL);
}

/* What a pass wrote into the second light is the light from now on: it is that buffer's part, and the second goes to
 * the buffer that is neither, nor the history. With keep, it is the history too, for the frame after (temporal
 * antialiasing, upscaling). No copy: the passes before only wrote, and the barrier after each is enough. */
void postSwap(Vulkan* v, VkCommandBuffer c, bool keep) {
    const uint8_t light = v->role[1], was = v->role[0];
    if (keep) v->role[2] = light;
    v->role[0] = light;
    v->role[1] = was != v->role[2] && was != light ? was : (uint8_t)(3 - light - v->role[2]);
    rolesBind(v, c, 1);
}


void postRecord(Vulkan* v, VkCommandBuffer c, Slot* s, const ArtfFrameData* frame, uint32_t image) {
    /* (the scene's size, until it is upscaled; then the frame's) */
    const uint32_t pixels = (uint32_t)v->sceneWidth * (uint32_t)v->sceneHeight, framePixels = (uint32_t)v->width * (uint32_t)v->height;
    const bool scaled = pixels != framePixels;
    const Viewport frameViewport = {0, (float)v->height, (float)v->width, -(float)v->height, 0, 1};
    const Viewport* viewport = &frameViewport;
    if (v->lutPending) {
        /* (past bloom's levels and auto exposure's state) */
        const VkDeviceSize at = ((VkDeviceSize)v->postBloomSizes[5][0] + v->postBloomSizes[5][1] * v->postBloomSizes[5][2] + 1) * 16;
        const BufferCopy copy = {0, at, v->lutStaging.size < v->lutRoom * 16 ? v->lutStaging.size : v->lutRoom * 16};
        v->vkCmdCopyBuffer(c, v->lutStaging.buffer, v->postBloom.buffer, 1, &copy);
        memoryBarrier(v, c, STAGE_TRANSFER, STAGE_COMPUTE | STAGE_FRAGMENT, ACCESS_TRANSFER_WRITE, ACCESS_SHADER_READ);
        v->lutPending = false;
    }
    v->vkCmdBindDescriptorSets(c, 1, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
    v->postTracking = true;
    v->pendingReads = v->pendingWrites = 0;
    v->postBarriers = v->postDispatches = 0;
    /* (the light and the second are the two buffers that do not hold the history) */
    v->role[0] = v->role[2] ? 0 : 1;
    v->role[1] = (uint8_t)(3 - v->role[0] - v->role[2]);
    rolesBind(v, c, 1);
    postStep(v, c, POST_GATHER, 0, pixels);
    /* (what reads the scene's light is done: occlusion and velocities may write over its memory) */
    if (v->postAliased) memoryBarrier(v, c, STAGE_COMPUTE, STAGE_COMPUTE, ACCESS_SHADER_READ, ACCESS_SHADER_WRITE);
    passMark(v, c, s, "gather");
    if (frame->post[0][2] > 0) {
        postStep(v, c, POST_OCCLUSION, 0, pixels);
        postStep(v, c, POST_OCCLUSION_BLUR, 0, pixels);
        passMark(v, c, s, "ambient occlusion");
    }
    if (frame->post[1][0] > 0) {
        /* (from rays, or marched across the frame) */
        const bool traced = frame->traced & ARTF_TRACED_REFLECTIONS && v->reflectionTraced;
        postStepWith(v, c, traced ? v->reflectionTraced : v->postPipelines[POST_REFLECTION], traced ? -1 : POST_REFLECTION, 0, v->postBloomSizes[0][1] * v->postBloomSizes[0][2]);
        postStep(v, c, POST_REFLECTION_APPLY, 0, pixels);
        passMark(v, c, s, "reflections");
    }
    const bool temporal = frame->post[1][3] > 0;
    if (frame->more[3] > 0 && temporal && v->postGathered.buffer) {
        /* lights sampled: gathered over frames, before temporal antialiasing (or upscaling), as post.c's sampledRow */
        postStep(v, c, POST_SAMPLED, (v->gatheredValid ? 1u : 0u) | v->gatheredHalf << 1, pixels);
        v->gatheredHalf ^= 1;
        v->gatheredValid = true;
        postSwap(v, c, false);
        postStep(v, c, POST_SAMPLED_SPACE, 0, pixels);
        postSwap(v, c, false);
        passMark(v, c, s, "sampled light gathered");
    } else v->gatheredValid = false;
    if (v->cloudsOn) {
        /* clouds in the scene: the grid, then each pixel by what is in front of it, as post.c's */
        postStep(v, c, POST_CLOUD_COLUMNS, v->cloudColumns, v->cloudColumns * v->cloudRows);
        postStep(v, c, POST_CLOUD_APPLY, v->cloudColumns, pixels);
        postSwap(v, c, false);
        passMark(v, c, s, "clouds");
    }
    if (scaled) {
        /* (from here on, the frame's size) */
        postStep(v, c, POST_UPSCALE, (temporal && v->historyValid ? 1u : 0u) | v->lockHalf << 1, framePixels);
        v->lockHalf ^= 1; /* (what this frame's locks are, the next reads) */
        postSwap(v, c, temporal);
        passMark(v, c, s, "upscaling");
    } else if (temporal) {
        postStep(v, c, POST_TEMPORAL, v->historyValid ? 1u : 0u, pixels);
        postSwap(v, c, true);
        passMark(v, c, s, "temporal antialiasing");
    }
    v->historyValid = temporal;
    if (frame->output[2] > 0 && (scaled || temporal)) {
        postStep(v, c, POST_SHARPEN, 0, framePixels);
        postSwap(v, c, false);
        passMark(v, c, s, "sharpening");
    }
    if (frame->post[2][1] > 0 && frame->post[2][0] > 0) {
        postStep(v, c, POST_FOCUS, 0, framePixels);
        postSwap(v, c, false);
        passMark(v, c, s, "depth of field");
    }
    if (frame->motionBlur > 0) {
        const uint32_t tiles = (((uint32_t)v->width + 15) / 16) * (((uint32_t)v->height + 15) / 16);
        postStep(v, c, POST_VELOCITY, 0, framePixels);
        postStep(v, c, POST_TILE_MOST, 0, tiles);
        postStep(v, c, POST_TILE_MOST, 1, tiles);
        postStep(v, c, POST_MOTION_BLUR, 0, framePixels);
        postSwap(v, c, false);
        passMark(v, c, s, "motion blur");
    }
    if (frame->grade[0][1] > 0) {
        postStep(v, c, POST_METER, v->exposureValid ? 1u : 0u, 1);
        v->exposureValid = true;
        passMark(v, c, s, "auto exposure");
    } else {
        v->exposureValid = false;
    }
    if (frame->post[0][0] > 0) {
        for (uint32_t l = 0; l < 6; ++l) postStep(v, c, POST_BLOOM_DOWN, l, v->postBloomSizes[l][1] * v->postBloomSizes[l][2]);
        for (int l = 4; l >= 0; --l) postStep(v, c, POST_BLOOM_UP, (uint32_t)l, v->postBloomSizes[l][1] * v->postBloomSizes[l][2]);
        passMark(v, c, s, "bloom");
    }
    v->postTracking = false;
    memoryBarrier(v, c, STAGE_COMPUTE | STAGE_TRANSFER, STAGE_FRAGMENT, ACCESS_SHADER_WRITE | ACCESS_TRANSFER_WRITE, ACCESS_SHADER_READ);
    /* (the image's own size and way, turned or not: preRotate) */
    const Rect2D imageArea = {{0, 0}, {(uint32_t)v->imageWidth, (uint32_t)v->imageHeight}};
    const Viewport imageViewport = {0, (float)v->imageHeight, (float)v->imageWidth, -(float)v->imageHeight, 0, 1};
    RenderPassBeginInfo present = {ST_RENDER_PASS_BEGIN, NULL, v->presentPass, v->presentFramebuffers[image], imageArea, 0, NULL};
    v->vkCmdBeginRenderPass(c, &present, 0);
    v->vkCmdSetViewport(c, 0, 1, v->turn ? &imageViewport : viewport);
    v->vkCmdSetScissor(c, 0, 1, &imageArea);
    if (v->preRotate) {
        const uint32_t word = (uint32_t)v->turn | (uint32_t)v->width << 2 | (uint32_t)v->height << 17;
        v->vkCmdPushConstants(c, v->pipelineLayout, SHADER_FRAGMENT, 8, 4, &word);
    }
    v->vkCmdBindPipeline(c, 0, v->presentPipeline);
    v->vkCmdBindDescriptorSets(c, 0, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
    rolesBind(v, c, 0);
    v->vkCmdDraw(c, 3, 1, 0, 0);
    if (v->overlayPixels && v->overlayUsable) {
        v->vkCmdBindPipeline(c, 0, v->overlayPipeline);
        v->vkCmdDraw(c, 3, 1, 0, 0);
    }
    v->vkCmdEndRenderPass(c);
    passMark(v, c, s, "tone map and out");
}

