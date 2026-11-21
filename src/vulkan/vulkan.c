/* artful — the Vulkan backend. Vulkan 1.2, opened at run time.
 *
 * All of a scene lives in a few buffers: one for every mesh's vertices, one for their indices, and the
 * instances. A frame is then the same few commands whatever the scene holds:
 *
 *   a compute shader culls the instances, picks each one's mesh by distance, and fills one draw
 *   command per mesh;
 *   one vkCmdDrawIndexedIndirect draws them all, with every texture in one descriptor array;
 *   what shows what is behind it comes after, in the order the CPU found, two draws each;
 *   one triangle puts the overlay over it.
 *
 * Tracing puts one compute shader in place of all but the overlay: see syncTrace.
 *
 * So the CPU's work does not grow with what is drawn: it writes only what changed.
 *
 * With occlusion, that is done in two parts. What showed last frame is drawn; a depth pyramid is made
 * of it; and the rest of what is in view is tested against that and drawn after. Nothing that should
 * show can be missing, and the frame after starts from what this one found. */
#include "vulkan/vulkanInternal.h"

/* ---- memory ---- */

int memoryType(const Vulkan* v, uint32_t bits, VkFlags wanted) {
    for (uint32_t i = 0; i < v->memory.typeCount; ++i)
        if ((bits >> i & 1) && (v->memory.types[i].propertyFlags & wanted) == wanted) return (int)i;
    return -1;
}


bool bufferNewFor(Vulkan* v, GpuBuffer* b, VkDeviceSize size, VkFlags usage, BufferUse use) {
    memset(b, 0, sizeof *b);
    BufferCreateInfo info = {ST_BUFFER_CREATE_INFO, NULL, 0, size ? size : 16, usage, 0, 0, NULL};
    if (v->vkCreateBuffer(v->device, &info, NULL, &b->buffer) != VK_OK) return false;
    MemoryRequirements req;
    v->vkGetBufferMemoryRequirements(v->device, b->buffer, &req);
    int type = -1;
    if (use == BUFFER_GPU) type = memoryType(v, req.typeBits, MEMORY_DEVICE_LOCAL);
    else if (use == BUFFER_WRITTEN_BY_CPU) {
        /* written by the CPU: straight into the GPU's memory where it can be (always, on an integrated
         * GPU; with resizable BAR, on others) */
        type = memoryType(v, req.typeBits, MEMORY_DEVICE_LOCAL | MEMORY_HOST_VISIBLE | MEMORY_HOST_COHERENT);
    } else {
        /* read by the CPU: from its own cache. The GPU's memory, or memory the CPU does not cache, is read a few
         * bytes at a time across the bus: a frame of 1920 x 1080 took 60 to 120 ms where it takes 1 from here */
        type = memoryType(v, req.typeBits, MEMORY_HOST_VISIBLE | MEMORY_HOST_COHERENT | MEMORY_HOST_CACHED);
    }
    if (type < 0 && use != BUFFER_GPU) type = memoryType(v, req.typeBits, MEMORY_HOST_VISIBLE | MEMORY_HOST_COHERENT);
    if (type < 0) return false;
    MemoryAllocateInfo alloc = {ST_MEMORY_ALLOCATE_INFO, NULL, req.size, (uint32_t)type};
    const bool overBudget = artfVulkanMemoryBudget >= 0 && (long long)(v->deviceBytes + req.size) > artfVulkanMemoryBudget;
    if (overBudget || v->vkAllocateMemory(v->device, &alloc, NULL, &b->memory) != VK_OK) {
        v->memoryShort = true;
        artfEventSay(ARTF_EVENT_OUT_OF_MEMORY, v, 0, req.size, "artful: the GPU's memory ran out, for a buffer of %llu bytes", (unsigned long long)req.size);
        v->vkDestroyBuffer(v->device, b->buffer, NULL);
        b->buffer = 0;
        return false;
    }
    b->held = req.size;
    v->deviceBytes += req.size;
    v->vkBindBufferMemory(v->device, b->buffer, b->memory, 0);
    b->size = info.size;
    b->serial = ++v->bufferSerial;
    if (use != BUFFER_GPU && v->vkMapMemory(v->device, b->memory, 0, ~(VkDeviceSize)0, 0, &b->mapped) != VK_OK) return false;
    return true;
}

bool bufferNew(Vulkan* v, GpuBuffer* b, VkDeviceSize size, VkFlags usage, bool hostVisible) {
    return bufferNewFor(v, b, size, usage, hostVisible ? BUFFER_WRITTEN_BY_CPU : BUFFER_GPU);
}

void bufferFree(Vulkan* v, GpuBuffer* b) {
    if (b->buffer) v->vkDestroyBuffer(v->device, b->buffer, NULL);
    if (b->memory && !b->borrowed) v->vkFreeMemory(v->device, b->memory, NULL);
    v->deviceBytes -= b->held;
    memset(b, 0, sizeof *b);
}

/* (imageNew's memory at least this large: for a buffer that lives in it when the image does not, bufferOver) */
static VkDeviceSize imageLeast;

bool imageNew(Vulkan* v, GpuImage* g, int format, int width, int height, uint32_t mips, int samples, VkFlags usage, VkFlags aspect) {
    memset(g, 0, sizeof *g);
    ImageCreateInfo info = {ST_IMAGE_CREATE_INFO, NULL, 0, 1 /* 2D */, format, {(uint32_t)width, (uint32_t)height, 1}, mips, 1, samples, 0 /* optimal */, usage, 0, 0, NULL, LAYOUT_UNDEFINED};
    if (v->vkCreateImage(v->device, &info, NULL, &g->image) != VK_OK) return false;
    MemoryRequirements req;
    v->vkGetImageMemoryRequirements(v->device, g->image, &req);
    int type = memoryType(v, req.typeBits, MEMORY_DEVICE_LOCAL);
    if (type < 0) return false;
    if (imageLeast > req.size) req.size = (imageLeast + req.alignment - 1) / req.alignment * req.alignment;
    MemoryAllocateInfo alloc = {ST_MEMORY_ALLOCATE_INFO, NULL, req.size, (uint32_t)type};
    const bool texture = (usage & IMAGE_SAMPLED) && (usage & IMAGE_TRANSFER_DST);
    const bool tooLarge = (texture && artfVulkanTextureMemoryLimit >= 0 && (long long)req.size > artfVulkanTextureMemoryLimit) ||
                          (artfVulkanMemoryBudget >= 0 && (long long)(v->deviceBytes + req.size) > artfVulkanMemoryBudget);
    if (tooLarge || v->vkAllocateMemory(v->device, &alloc, NULL, &g->memory) != VK_OK) {
        v->memoryShort = true;
        artfEventSay(ARTF_EVENT_OUT_OF_MEMORY, v, 0, req.size, "artful: the GPU's memory ran out, for an image of %llu bytes", (unsigned long long)req.size);
        v->vkDestroyImage(v->device, g->image, NULL);
        g->image = 0;
        return false;
    }
    g->held = req.size;
    g->type = type;
    v->deviceBytes += req.size;
    v->vkBindImageMemory(v->device, g->image, g->memory, 0);
    ImageViewCreateInfo view = {ST_IMAGE_VIEW_CREATE_INFO, NULL, 0, g->image, 1 /* 2D */, format, {0, 0, 0, 0}, {aspect, 0, mips, 0, 1}};
    return v->vkCreateImageView(v->device, &view, NULL, &g->view) == VK_OK;
}

/* A buffer in an image's memory, for what lives only where the image does not (the frame graph's aliasing): false
 * when the image's memory would not do (too small, or of a type the buffer cannot have), and the buffer gets its own. */
bool bufferOver(Vulkan* v, GpuBuffer* b, VkDeviceSize size, VkFlags usage, const GpuImage* g) {
    memset(b, 0, sizeof *b);
    if (!g->memory) return false;
    BufferCreateInfo info = {ST_BUFFER_CREATE_INFO, NULL, 0, size, usage, 0, 0, NULL};
    if (v->vkCreateBuffer(v->device, &info, NULL, &b->buffer) != VK_OK) return false;
    MemoryRequirements req;
    v->vkGetBufferMemoryRequirements(v->device, b->buffer, &req);
    if (!(req.typeBits >> g->type & 1) || req.size > g->held || v->vkBindBufferMemory(v->device, b->buffer, g->memory, 0) != VK_OK) {
        v->vkDestroyBuffer(v->device, b->buffer, NULL);
        b->buffer = 0;
        return false;
    }
    b->memory = g->memory;
    b->borrowed = true;
    b->size = size;
    b->serial = ++v->bufferSerial;
    return true;
}

void imageFree(Vulkan* v, GpuImage* g) {
    if (g->view) v->vkDestroyImageView(v->device, g->view, NULL);
    if (g->image) v->vkDestroyImage(v->device, g->image, NULL);
    if (g->memory) v->vkFreeMemory(v->device, g->memory, NULL);
    v->deviceBytes -= g->held;
    memset(g, 0, sizeof *g);
}

void imageBarrier(Vulkan* v, VkCommandBuffer c, VkImage image, uint32_t mips, int from, int to, VkFlags srcStage, VkFlags dstStage, VkFlags srcAccess, VkFlags dstAccess) {
    ImageMemoryBarrier b = {ST_IMAGE_MEMORY_BARRIER, NULL, srcAccess, dstAccess, from, to, ~0u, ~0u, image, {ASPECT_COLOR, 0, mips, 0, 1}};
    v->vkCmdPipelineBarrier(c, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &b);
}

void memoryBarrier(Vulkan* v, VkCommandBuffer c, VkFlags srcStage, VkFlags dstStage, VkFlags srcAccess, VkFlags dstAccess) {
    GlobalBarrier b = {ST_MEMORY_BARRIER, NULL, srcAccess, dstAccess};
    v->vkCmdPipelineBarrier(c, srcStage, dstStage, 0, 1, &b, 0, NULL, 0, NULL);
}

/* ---- barriers from what passes do (the frame graph's step 2, for the scene's passes) ---- */

/* As the frame before left them: each resource written and read by the stages that do so in a frame. */
void trackFrameStart(Vulkan* v) {
    static const VkFlags writes[TRACK_COUNT][2] = {
        [TRACK_FOG] = {STAGE_COMPUTE, ACCESS_SHADER_WRITE},
        [TRACK_HIZ] = {STAGE_COMPUTE, ACCESS_SHADER_WRITE},
        [TRACK_TRACE] = {STAGE_COMPUTE | STAGE_TRANSFER, ACCESS_SHADER_WRITE | ACCESS_TRANSFER_WRITE},
        [TRACK_LAYERS] = {STAGE_FRAGMENT | STAGE_TRANSFER, ACCESS_SHADER_WRITE | ACCESS_TRANSFER_WRITE},
        [TRACK_DEPTH] = {STAGE_EARLY_TESTS | STAGE_LATE_TESTS, ACCESS_DEPTH_WRITE},
        [TRACK_POST] = {STAGE_COMPUTE, ACCESS_SHADER_WRITE}};
    static const VkFlags reads[TRACK_COUNT] = {
        [TRACK_FOG] = STAGE_FRAGMENT, [TRACK_HIZ] = STAGE_COMPUTE, [TRACK_TRACE] = STAGE_COMPUTE | STAGE_FRAGMENT,
        [TRACK_LAYERS] = STAGE_FRAGMENT, [TRACK_DEPTH] = STAGE_FRAGMENT | STAGE_COMPUTE, [TRACK_POST] = STAGE_COMPUTE | STAGE_FRAGMENT};
    for (int t = 0; t < TRACK_COUNT; ++t) {
        v->trackWriteStage[t] = writes[t][0];
        v->trackWriteAccess[t] = writes[t][1];
        v->trackReadStage[t] = reads[t];
        v->trackVisible[t] = 0;
    }
    v->trackBarriers = 0;
}

/* A pass is about to read (readMask) and write (writeMask) these, at stage: one barrier for all it needs, after what
 * wrote what it reads or writes (its writes made visible), and after what read what it writes (an execution
 * dependency). covered: a render pass's own dependency already orders it; what it does is noted, and no barrier made. */
static void trackDo(Vulkan* v, VkCommandBuffer c, uint32_t readMask, uint32_t writeMask, VkFlags stage, VkFlags readAccess, VkFlags writeAccess, bool covered) {
    VkFlags srcStage = 0, srcAccess = 0;
    for (int t = 0; t < TRACK_COUNT; ++t) {
        const bool reads = readMask >> t & 1, writes = writeMask >> t & 1;
        if (!reads && !writes) continue;
        /* (after what wrote it: unless that write is visible to this stage already and this only reads) */
        if (v->trackWriteStage[t] && (writes || (stage & ~v->trackVisible[t]))) {
            srcStage |= v->trackWriteStage[t];
            srcAccess |= v->trackWriteAccess[t];
        }
        if (writes) srcStage |= v->trackReadStage[t];
    }
    if (srcStage && !covered) {
        memoryBarrier(v, c, srcStage, stage, srcAccess, readAccess | writeAccess);
        ++v->trackBarriers;
    }
    for (int t = 0; t < TRACK_COUNT; ++t) {
        if (writeMask >> t & 1) {
            v->trackWriteStage[t] = stage;
            v->trackWriteAccess[t] = writeAccess;
            v->trackReadStage[t] = 0;
            v->trackVisible[t] = 0;
        } else if (readMask >> t & 1) {
            v->trackReadStage[t] |= stage;
            v->trackVisible[t] |= stage;
        }
    }
}
void trackUse(Vulkan* v, VkCommandBuffer c, uint32_t readMask, uint32_t writeMask, VkFlags stage, VkFlags readAccess, VkFlags writeAccess) {
    trackDo(v, c, readMask, writeMask, stage, readAccess, writeAccess, false);
}
void trackCovered(Vulkan* v, uint32_t readMask, uint32_t writeMask, VkFlags stage, VkFlags readAccess, VkFlags writeAccess) {
    trackDo(v, NULL, readMask, writeMask, stage, readAccess, writeAccess, true);
}

/* Commands to run now, for what happens rarely: loading a mesh, say. */
VkCommandBuffer oneShotBegin(Vulkan* v) {
    CommandBufferAllocateInfo alloc = {ST_COMMAND_BUFFER_ALLOCATE, NULL, v->commandPool, 0, 1};
    VkCommandBuffer c = NULL;
    v->vkAllocateCommandBuffers(v->device, &alloc, &c);
    CommandBufferBeginInfo begin = {ST_COMMAND_BUFFER_BEGIN, NULL, 1 /* one time */, NULL};
    v->vkBeginCommandBuffer(c, &begin);
    return c;
}

bool oneShotEnd(Vulkan* v, VkCommandBuffer c) {
    v->vkEndCommandBuffer(c);
    SubmitInfo submit = {ST_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &c, 0, NULL};
    VkResult r = v->vkQueueSubmit(v->queue, 1, &submit, 0);
    if (r == VK_OK) r = v->vkQueueWaitIdle(v->queue);
    v->vkFreeCommandBuffers(v->device, v->commandPool, 1, &c);
    return r == VK_OK;
}

/* ---- the frame's targets ---- */

void targetsFree(Vulkan* v) {
    if (v->resolveFramebuffer) v->vkDestroyFramebuffer(v->device, v->resolveFramebuffer, NULL);
    v->resolveFramebuffer = 0;
    for (uint32_t i = 0; i < MAX_SWAPCHAIN_IMAGES; ++i) {
        if (v->framebuffers[i]) v->vkDestroyFramebuffer(v->device, v->framebuffers[i], NULL);
        if (v->views[i]) v->vkDestroyImageView(v->device, v->views[i], NULL);
        v->framebuffers[i] = 0;
        v->views[i] = 0;
    }
    for (uint32_t i = 0; i < MAX_SWAPCHAIN_IMAGES; ++i) {
        if (v->presentFramebuffers[i]) v->vkDestroyFramebuffer(v->device, v->presentFramebuffers[i], NULL);
        v->presentFramebuffers[i] = 0;
    }
    imageFree(v, &v->floatColor);
    imageFree(v, &v->floatSurface);
    GpuBuffer* const post[12] = {&v->postLight, &v->postDepth, &v->postSurface, &v->postScratch, &v->postHistory, &v->postOcclusion, &v->postFog, &v->postBloom, &v->postBlur, &v->postLocks, &v->postGathered, &v->postNone};
    for (int i = 0; i < 12; ++i) bufferFree(v, post[i]);
    v->historyValid = false;
    v->hasPrevious = false;
    imageFree(v, &v->depth);
    imageFree(v, &v->samples4);
    imageFree(v, &v->offscreen);
    bufferFree(v, &v->readback);
    imageFree(v, &v->overlay);
    bufferFree(v, &v->hiz);
    bufferFree(v, &v->traceSum);
    for (int i = 0; i < 7; ++i) bufferFree(v, &v->denoiseBuffers[i]);
    bufferFree(v, &v->denoiseShift);
    v->traceDone = 0;
    for (int i = 0; i < SLOTS; ++i) bufferFree(v, &v->slots[i].overlayStaging);
    v->overlayUsable = false;
}

void writeOverlayDescriptor(Vulkan* v) {
    DescriptorImageInfo image = {v->sampler, v->overlay.view, LAYOUT_SHADER_READ};
    for (int i = 0; i < SLOTS; ++i) {
        WriteDescriptorSet w = {ST_WRITE_DESCRIPTOR_SET, NULL, v->slots[i].set, 7, 0, 1, DESCRIPTOR_COMBINED, &image, NULL, NULL};
        v->vkUpdateDescriptorSets(v->device, 1, &w, 0, NULL);
    }
}

/* The pyramid's levels, from half the frame down to a texel, and what the depth buffer's size is. */
bool hizMake(Vulkan* v) {
    uint32_t w = (uint32_t)v->sceneWidth, h = (uint32_t)v->sceneHeight, at = 0, level = 0;
    v->hizTable[ARTF_HIZ_LEVELS - 1][1] = w;
    v->hizTable[ARTF_HIZ_LEVELS - 1][2] = h;
    while (level < ARTF_HIZ_LEVELS - 1 && (w > 1 || h > 1)) {
        w = (w + 1) / 2;
        h = (h + 1) / 2;
        v->hizTable[level][0] = at;
        v->hizTable[level][1] = w;
        v->hizTable[level][2] = h;
        at += w * h;
        ++level;
    }
    v->hizLevels = level;
    if (!bufferNew(v, &v->hiz, (VkDeviceSize)at * 4, BUFFER_STORAGE, false)) return false;
    const DescriptorBufferInfo pyramid = {v->hiz.buffer, 0, ~(VkDeviceSize)0};
    const DescriptorImageInfo depth = {v->depthSampler, v->depth.view, LAYOUT_DEPTH_READ};
    for (int i = 0; i < SLOTS; ++i) {
        const WriteDescriptorSet writes[2] = {{ST_WRITE_DESCRIPTOR_SET, NULL, v->slots[i].set, 9, 0, 1, DESCRIPTOR_STORAGE, NULL, &pyramid, NULL},
                                              {ST_WRITE_DESCRIPTOR_SET, NULL, v->slots[i].set, 8, 0, 1, DESCRIPTOR_COMBINED, &depth, NULL, NULL}};
        v->vkUpdateDescriptorSets(v->device, 2, writes, 0, NULL);
    }
    return true;
}

int artfVulkanSubmitsLeft = -1;


extern bool artfShaderDebugInfo; /* (spirv.h) */
bool artfVulkanRefuseSwapchain = false;
int artfVulkanTestTurn = 0;
bool artfVulkanNoAliasing = false;


/* With floats: what the scene draws into, and what comes after reads and writes, bound once for every slot. */
/* What a post shader reads and writes, from its words (irFromSpirv, irUses): set 1's parts as bits 0 to 2, set 0's
 * bindings 50 to 59 as bits 3 to 12, 69 as 55's (they are one buffer), 71 as bit 13; what else it binds is only read, and
 * left out. */
void postAccessOf(const uint32_t* code, size_t size, uint32_t* reads, uint32_t* writes) {
    *reads = *writes = ~0u; /* (not known: as all) */
    IrModule m;
    irInit(&m);
    if (code && irFromSpirv(&m, code, size / 4)) {
        IrUse uses[64];
        const int n = irUses(&m, uses, 64);
        *reads = *writes = 0;
        for (int k = 0; k < n && k < 64; ++k) {
            uint32_t bit = 0;
            if (uses[k].set == 1 && uses[k].binding < 3) bit = 1u << uses[k].binding;
            else if (uses[k].set == 0 && uses[k].binding >= 50 && uses[k].binding <= 59) bit = 1u << (3 + uses[k].binding - 50);
            else if (uses[k].set == 0 && uses[k].binding == 69) bit = 1u << (3 + 55 - 50);
            else if (uses[k].set == 0 && uses[k].binding == 71) bit = 1u << 13;
            else if (uses[k].set == 0 && uses[k].binding == 72) bit = 1u << 14;
            else if (uses[k].set == 0 && uses[k].binding == 73) bit = 1u << 15;
            else if (uses[k].set == 0 && uses[k].binding == 74) bit = 1u << 16;
            if (uses[k].read) *reads |= bit;
            if (uses[k].write) *writes |= bit;
        }
    }
    irFree(&m);
}


/* What the frame's effects need past the light, its depth and surfaces, and bloom (the frame graph's targets that
 * effects that are off leave unused: frameGraph.c). */
uint32_t postNeedsOf(const Vulkan* v, const ArtfFrameData* f, bool clouds) {
    const bool scaled = v->sceneWidth != v->width || v->sceneHeight != v->height, temporal = f->post[1][3] > 0;
    uint32_t n = 0;
    if (f->post[0][2] > 0) n |= NEED_OCCLUSION;
    if (f->motionBlur > 0) n |= NEED_BLUR | NEED_SECOND;
    if (f->post[1][1] > 0 && f->look[1] > 0) n |= NEED_FOG;
    if (scaled || temporal) n |= NEED_SECOND | NEED_HISTORY;
    if (f->post[2][1] > 0 && f->post[2][0] > 0) n |= NEED_SECOND;
    if (f->more[3] > 0 && temporal) n |= NEED_GATHERED;
    if (clouds) n |= NEED_SECOND; /* (clouds in the scene: what they make of the light goes there) */
    return n;
}

/* The post passes' buffers, as postNeeds says: what an effect that is off would use is not made, and its binding names
 * a buffer of nothing (postNone); ambient occlusion's and motion blur's velocities are one buffer, as large as the larger,
 * as no pass uses both (the frame graph: one lives in its pass, the other in its own). Made again when an effect comes
 * or goes (postNeedsOf): the device is idle then. */
bool postBuffersMake(Vulkan* v) {
    const VkDeviceSize pixels = (VkDeviceSize)v->sceneWidth * v->sceneHeight, framePixels = (VkDeviceSize)v->width * v->height;
    const VkDeviceSize fog = (VkDeviceSize)((v->sceneWidth + ARTF_FROXEL_TILE - 1) / ARTF_FROXEL_TILE) * ((v->sceneHeight + ARTF_FROXEL_TILE - 1) / ARTF_FROXEL_TILE) * ARTF_FROXEL_SLICES;
    VkDeviceSize bloom = 0;
    uint32_t w = (uint32_t)v->width, h = (uint32_t)v->height;
    for (int l = 0; l < 6; ++l) {
        w = (w + 1) / 2 ? (w + 1) / 2 : 1;
        h = (h + 1) / 2 ? (h + 1) / 2 : 1;
        v->postBloomSizes[l][0] = (uint32_t)bloom;
        v->postBloomSizes[l][1] = w;
        v->postBloomSizes[l][2] = h;
        bloom += (VkDeviceSize)w * h;
    }
    GpuBuffer* const buffers[8] = {&v->postLight, &v->postDepth, &v->postSurface, &v->postScratch, &v->postHistory, &v->postOcclusion, &v->postFog, &v->postBloom};
    for (int i = 0; i < 8; ++i) bufferFree(v, buffers[i]);
    bufferFree(v, &v->postBlur);
    bufferFree(v, &v->postLocks);
    bufferFree(v, &v->postGathered);
    v->gatheredValid = false;
    /* (lights sampled: what they gave, gathered over frames, at the scene's size) */
    if (v->postNeeds & NEED_GATHERED && !bufferNew(v, &v->postGathered, pixels * 64, BUFFER_STORAGE | BUFFER_TRANSFER_DST | BUFFER_TRANSFER_SRC, false)) return false;
    /* (upscaling's locks, with a history and the scene drawn smaller) */
    if (v->postNeeds & NEED_HISTORY && (v->sceneWidth != v->width || v->sceneHeight != v->height) &&
        !bufferNew(v, &v->postLocks, framePixels * 8, BUFFER_STORAGE | BUFFER_TRANSFER_DST | BUFFER_TRANSFER_SRC, false))
        return false;
    /* (motion blur's: each pixel's velocity, then of each tile of 16 the largest, and of its neighbours) */
    const VkDeviceSize blurTiles = (VkDeviceSize)((v->width + 15) / 16) * ((v->height + 15) / 16);
    const VkDeviceSize occlusion = v->postNeeds & NEED_OCCLUSION ? pixels * 4 : 0, blur = v->postNeeds & NEED_BLUR ? (framePixels + blurTiles * 2) * 8 : 0;
    /* (bloom's levels, and past them auto exposure's state; the surface's words, then the instance that shows at each
     * pixel; light, the second and the history end at the frame's size) */
    /* (light, the second and the history: four halves a pixel, postShaders.inc's postLight) */
    const VkDeviceSize sizes[8] = {framePixels * 8, pixels * 4, pixels * 8, v->postNeeds & NEED_SECOND ? framePixels * 8 : 0, v->postNeeds & NEED_HISTORY ? framePixels * 8 : 0,
                                   occlusion > blur ? occlusion : blur, v->postNeeds & NEED_FOG ? fog * 16 : 0, (bloom + 1 + v->lutRoom) * 16};
    /* (occlusion and motion blur's velocities live after the gather, the scene's light only until it: the same memory,
     * when it is large enough, with a barrier after the gather and the scene's pass waiting for what wrote it) */
    v->postAliased = sizes[5] && !artfVulkanNoAliasing && bufferOver(v, buffers[5], sizes[5], BUFFER_STORAGE | BUFFER_TRANSFER_DST | BUFFER_TRANSFER_SRC, &v->floatColor);
    for (int i = 0; i < 8; ++i)
        if (sizes[i] && !buffers[i]->buffer && !bufferNew(v, buffers[i], sizes[i], BUFFER_STORAGE | BUFFER_TRANSFER_DST | BUFFER_TRANSFER_SRC, false)) return false;
    if (!v->postNone.buffer && !bufferNew(v, &v->postNone, 64, BUFFER_STORAGE | BUFFER_TRANSFER_DST | BUFFER_TRANSFER_SRC, false)) return false;
#define OR_NONE(b) ((b)->buffer ? (b) : &v->postNone)
    for (int k = 0; k < SLOTS; ++k) {
        WriteDescriptorSet writes[10];
        DescriptorBufferInfo infos[8];
        for (int i = 0; i < 8; ++i) {
            const DescriptorBufferInfo info = {OR_NONE(buffers[i])->buffer, 0, ~(VkDeviceSize)0};
            infos[i] = info;
            const WriteDescriptorSet write = {ST_WRITE_DESCRIPTOR_SET, NULL, v->slots[k].set, 50 + (uint32_t)i, 0, 1, DESCRIPTOR_STORAGE, NULL, &infos[i], NULL};
            writes[i] = write;
        }
        const DescriptorImageInfo color = {v->depthSampler, v->floatColor.view, LAYOUT_SHADER_READ}, surface = {v->depthSampler, v->floatSurface.view, LAYOUT_SHADER_READ};
        const WriteDescriptorSet images[2] = {{ST_WRITE_DESCRIPTOR_SET, NULL, v->slots[k].set, 58, 0, 1, DESCRIPTOR_COMBINED, &color, NULL, NULL},
                                             {ST_WRITE_DESCRIPTOR_SET, NULL, v->slots[k].set, 59, 0, 1, DESCRIPTOR_COMBINED, &surface, NULL, NULL}};
        writes[8] = images[0];
        writes[9] = images[1];
        v->vkUpdateDescriptorSets(v->device, 10, writes, 0, NULL);
        bindStorage(v, &v->slots[k], 69, blur ? &v->postOcclusion : &v->postNone);
        bindStorage(v, &v->slots[k], 71, v->postLocks.buffer ? &v->postLocks : &v->postNone);
        bindStorage(v, &v->slots[k], 72, v->postGathered.buffer ? &v->postGathered : &v->postNone);
        bindStorage(v, &v->slots[k], 73, v->cloudField.buffer ? &v->cloudField : &v->postNone);
        bindStorage(v, &v->slots[k], 74, v->cloudGrid.buffer ? &v->cloudGrid : &v->postNone);
    }
    const GpuBuffer* const three[3] = {&v->postLight, OR_NONE(&v->postScratch), OR_NONE(&v->postHistory)};
#undef OR_NONE
    for (int k = 0; k < 27; ++k) {
        DescriptorBufferInfo infos[3];
        WriteDescriptorSet writes[3];
        for (int r = 0; r < 3; ++r) {
            const int which = r == 0 ? k / 9 : r == 1 ? k / 3 % 3 : k % 3;
            const DescriptorBufferInfo info = {three[which]->buffer, 0, ~(VkDeviceSize)0};
            infos[r] = info;
            const WriteDescriptorSet write = {ST_WRITE_DESCRIPTOR_SET, NULL, v->roleSets[k], (uint32_t)r, 0, 1, DESCRIPTOR_STORAGE, NULL, &infos[r], NULL};
            writes[r] = write;
        }
        v->vkUpdateDescriptorSets(v->device, 3, writes, 0, NULL);
    }
    v->role[0] = 0;
    v->role[1] = 1;
    v->role[2] = 2;
    v->historyValid = false;
    v->exposureValid = false;
    v->hasPrevious = false;
    v->lutVersion = 0; /* (copied again) */
    return true;
}

/* With floats: what the scene draws into, and what comes after reads and writes, bound once for every slot. */
bool postTargetsMake(Vulkan* v) {
    /* (the scene's light: with room for motion blur's velocities, which can live in its memory: postBuffersMake) */
    const VkDeviceSize blurTiles = (VkDeviceSize)((v->width + 15) / 16) * ((v->height + 15) / 16);
    imageLeast = v->sceneWidth == v->width && v->sceneHeight == v->height ? ((VkDeviceSize)v->width * v->height + blurTiles * 2) * 8 : 0;
    const bool made = imageNew(v, &v->floatColor, v->sceneFormat, v->sceneWidth, v->sceneHeight, 1, 1, IMAGE_COLOR | IMAGE_SAMPLED, ASPECT_COLOR);
    imageLeast = 0;
    if (!made) return false;
    /* (the surface word, and where motion is followed, the instance that shows + 1 or where it was) */
    if (!imageNew(v, &v->floatSurface, v->surfaceWide ? FORMAT_R32G32_UINT : FORMAT_R32_UINT, v->sceneWidth, v->sceneHeight, 1, v->samples, IMAGE_COLOR | IMAGE_SAMPLED, ASPECT_COLOR)) return false;
    return postBuffersMake(v);
}

/* The scene's look-up table, into the staging buffer, and bloom's buffer made larger when it has no room for it: the
 * copy is made as the frame's commands begin (postRecord). Rare: it waits for the GPU. */
bool syncLut(Vulkan* v, const ArtfScene* scene) {
    if (!scene->lut || (v->lutScene == scene->id && v->lutVersion == scene->lutVersion)) return true;
    const VkDeviceSize entries = (VkDeviceSize)(scene->lutFloats / 3);
    v->vkDeviceWaitIdle(v->device);
    if (entries > v->lutRoom) {
        v->lutRoom = entries;
        const VkDeviceSize bloom = (VkDeviceSize)v->postBloomSizes[5][0] + v->postBloomSizes[5][1] * v->postBloomSizes[5][2];
        bufferFree(v, &v->postBloom);
        if (!bufferNew(v, &v->postBloom, (bloom + 1 + v->lutRoom) * 16, BUFFER_STORAGE | BUFFER_TRANSFER_DST | BUFFER_TRANSFER_SRC, false)) return false;
        const DescriptorBufferInfo info = {v->postBloom.buffer, 0, ~(VkDeviceSize)0};
        for (int k = 0; k < SLOTS; ++k) {
            const WriteDescriptorSet write = {ST_WRITE_DESCRIPTOR_SET, NULL, v->slots[k].set, 57, 0, 1, DESCRIPTOR_STORAGE, NULL, &info, NULL};
            v->vkUpdateDescriptorSets(v->device, 1, &write, 0, NULL);
        }
        v->exposureValid = false; /* (its state was there) */
    }
    if (v->lutStaging.size < entries * 16) {
        bufferFree(v, &v->lutStaging);
        if (!bufferNew(v, &v->lutStaging, entries * 16, BUFFER_TRANSFER_SRC, true)) return false;
    }
    float* to = v->lutStaging.mapped;
    for (VkDeviceSize i = 0; i < entries; ++i) {
        memcpy(to + i * 4, scene->lut + i * 3, 12);
        to[i * 4 + 3] = 0;
    }
    v->lutScene = scene->id;
    v->lutVersion = scene->lutVersion;
    v->lutPending = true;
    return true;
}

bool targetsMake(Vulkan* v) {
    if (v->surface) {
        SurfaceCapabilities caps;
        if (v->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v->gpu, v->surface, &caps) != VK_OK) return false;
        /* (0xFFFFFFFF: the size is ours to say) */
        v->surfaceTransform = caps.currentTransform;
        v->surfaceWidth = caps.current.width;
        v->surfaceHeight = caps.current.height;
        /* A screen that is turned (Android) gives its size as it stands upright. The frame is drawn as the
         * eye sees it, and the compositor turns it: identity, with the sides the other way round. */
        const bool upright = caps.supportedTransforms & 1;
        const bool sideways = upright && caps.currentTransform & (0x2 | 0x8 | 0x20 | 0x80); /* 90, 270, and mirrored */
        /* (preRotate: a quarter, a half or three quarters the image takes turned, and is shown as it is) */
        const VkFlags current = caps.currentTransform;
        v->turn = v->preRotate && v->floats && caps.supportedTransforms & current ? (current == 0x2 ? 1 : current == 0x4 ? 2 : current == 0x8 ? 3 : 0) : 0;
        if (caps.current.width != 0xFFFFFFFFu) {
            v->width = (int)(sideways ? caps.current.height : caps.current.width);
            v->height = (int)(sideways ? caps.current.width : caps.current.height);
        }
        if (v->width <= 0 || v->height <= 0) return true; /* minimized: nothing to draw into yet */
        uint32_t modeCount = 0;
        int modes[16], mode = 2; /* FIFO: always there */
        v->vkGetPhysicalDeviceSurfacePresentModesKHR(v->gpu, v->surface, &modeCount, NULL);
        modeCount = modeCount > 16 ? 16 : modeCount;
        v->vkGetPhysicalDeviceSurfacePresentModesKHR(v->gpu, v->surface, &modeCount, modes);
        if (v->noVsync)
            for (uint32_t i = 0; i < modeCount; ++i) {
                if (modes[i] == 0) { mode = 0; break; }
                if (modes[i] == 1) mode = 1;
            }
        uint32_t images = caps.minImageCount + 1;
        if (caps.maxImageCount && images > caps.maxImageCount) images = caps.maxImageCount;
        VkFlags alpha = caps.supportedAlpha & 1 ? 1 : caps.supportedAlpha & (~caps.supportedAlpha + 1);
        VkSwapchainKHR old = v->swapchain;
        v->imageWidth = v->turn & 1 ? v->height : v->width;
        v->imageHeight = v->turn & 1 ? v->width : v->height;
        SwapchainCreateInfo info = {ST_SWAPCHAIN_CREATE_INFO, NULL, 0, v->surface, images, v->colorFormat, v->colorSpace, {(uint32_t)v->imageWidth, (uint32_t)v->imageHeight}, 1, IMAGE_COLOR, 0, 0, NULL,
                                    v->turn ? current : upright ? 1u : caps.currentTransform, alpha, mode, 1, old};
        VkSwapchainKHR made = 0;
        const VkResult result = artfVulkanRefuseSwapchain ? -1000000000 : v->vkCreateSwapchainKHR(v->device, &info, NULL, &made);
        if (old) v->vkDestroySwapchainKHR(v->device, old, NULL);
        v->swapchain = made;
        if (result != VK_OK) {
            /* The window goes back with the surface: on Android, whatever comes next could not reach it otherwise. */
            artfLog("artful: the driver refused a swapchain: %d", (int)result);
            v->vkDestroySurfaceKHR(v->instance, v->surface, NULL);
            v->surface = 0;
            v->swapchain = 0;
            v->lost = true;
            return false;
        }
        v->imageCount = MAX_SWAPCHAIN_IMAGES;
        v->vkGetSwapchainImagesKHR(v->device, v->swapchain, &v->imageCount, v->images);
        for (uint32_t i = 0; i < v->imageCount; ++i) {
            ImageViewCreateInfo view = {ST_IMAGE_VIEW_CREATE_INFO, NULL, 0, v->images[i], 1, v->colorFormat, {0, 0, 0, 0}, {ASPECT_COLOR, 0, 1, 0, 1}};
            if (v->vkCreateImageView(v->device, &view, NULL, &v->views[i]) != VK_OK) return false;
        }
    } else {
        v->turn = v->preRotate && v->floats ? artfVulkanTestTurn & 3 : 0;
        v->imageWidth = v->turn & 1 ? v->height : v->width;
        v->imageHeight = v->turn & 1 ? v->width : v->height;
        if (!imageNew(v, &v->offscreen, v->colorFormat, v->imageWidth, v->imageHeight, 1, 1, IMAGE_COLOR | IMAGE_TRANSFER_SRC, ASPECT_COLOR)) return false;
        v->imageCount = 1;
        v->images[0] = v->offscreen.image;
        ImageViewCreateInfo view = {ST_IMAGE_VIEW_CREATE_INFO, NULL, 0, v->offscreen.image, 1, v->colorFormat, {0, 0, 0, 0}, {ASPECT_COLOR, 0, 1, 0, 1}};
        if (v->vkCreateImageView(v->device, &view, NULL, &v->views[0]) != VK_OK) return false;
    }
    v->sceneWidth = v->width;
    v->sceneHeight = v->height;
    if (v->floats && v->renderScale < 1.f) {
        v->sceneWidth = artfMaxi((int)((float)v->width * v->renderScale + .5f), 1);
        v->sceneHeight = artfMaxi((int)((float)v->height * v->renderScale + .5f), 1);
    }
    const bool many = v->samples > 1;
    if (many && !imageNew(v, &v->samples4, v->sceneFormat, v->sceneWidth, v->sceneHeight, 1, v->samples, IMAGE_COLOR, ASPECT_COLOR)) return false;
    if (!imageNew(v, &v->depth, FORMAT_D32, v->sceneWidth, v->sceneHeight, 1, v->samples, IMAGE_DEPTH | IMAGE_SAMPLED, ASPECT_DEPTH)) return false;
    if (v->floats && !postTargetsMake(v)) return false;
    if (many) {
        /* (the resolve pass has the light, and in floats the surface too: its framebuffer, both) */
        const VkImageView resolving[2] = {v->samples4.view, v->floatSurface.view};
        FramebufferCreateInfo info = {ST_FRAMEBUFFER, NULL, 0, v->resolvePass, v->floats ? 2u : 1u, resolving, (uint32_t)v->sceneWidth, (uint32_t)v->sceneHeight, 1};
        if (v->vkCreateFramebuffer(v->device, &info, NULL, &v->resolveFramebuffer) != VK_OK) return false;
    }
    for (uint32_t i = 0; i < v->imageCount; ++i) {
        const VkImageView one[2] = {v->views[i], v->depth.view}, resolved[3] = {v->samples4.view, v->depth.view, v->views[i]};
        const VkImageView floats[3] = {v->floatColor.view, v->depth.view, v->floatSurface.view};
        const VkImageView floatsMany[4] = {v->samples4.view, v->depth.view, v->floatColor.view, v->floatSurface.view};
        FramebufferCreateInfo info = {ST_FRAMEBUFFER, NULL, 0, v->renderPass, v->floats ? (many ? 4 : 3) : many ? 3 : 2, v->floats ? (many ? floatsMany : floats) : many ? resolved : one, (uint32_t)v->sceneWidth, (uint32_t)v->sceneHeight, 1};
        if (v->vkCreateFramebuffer(v->device, &info, NULL, &v->framebuffers[i]) != VK_OK) return false;
        if (v->floats) {
            FramebufferCreateInfo present = {ST_FRAMEBUFFER, NULL, 0, v->presentPass, 1, &v->views[i], (uint32_t)v->imageWidth, (uint32_t)v->imageHeight, 1};
            if (v->vkCreateFramebuffer(v->device, &present, NULL, &v->presentFramebuffers[i]) != VK_OK) return false;
        }
    }
    if (!imageNew(v, &v->overlay, FORMAT_BGRA8, v->width, v->height, 1, 1, IMAGE_SAMPLED | IMAGE_TRANSFER_DST, ASPECT_COLOR)) return false;
    writeOverlayDescriptor(v);
    if (!hizMake(v)) return false;
    v->targetsStale = false;
    return true;
}

/* Whether the surface turned or changed size since the swapchain was made. */
bool surfaceChanged(Vulkan* v) {
    SurfaceCapabilities caps;
    if (v->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v->gpu, v->surface, &caps) != VK_OK) return true;
    return caps.currentTransform != v->surfaceTransform || caps.current.width != v->surfaceWidth || caps.current.height != v->surfaceHeight;
}

bool targetsRemake(Vulkan* v) {
    v->vkDeviceWaitIdle(v->device);
    targetsFree(v);
    if (v->overlayPixels) { v->overlayFull = true; v->overlayPending = true; }
    return targetsMake(v);
}

/* ---- pipelines ---- */

VkShaderModule shaderModule(Vulkan* v, uint32_t* code, size_t size) {
    ShaderModuleCreateInfo info = {ST_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkShaderModule module = 0;
    v->vkCreateShaderModule(v->device, &info, NULL, &module);
    artfFree(code);
    return module;
}

/* colorLoad, depthLoad: 0 clears, 1 keeps what is there; depthStore: what is drawn stays for a shader. */
/* A pipeline's blending into the scene's passes: with a frame in floats, the surface is beside the color, and only
 * what is solid writes it. */
PipelineColorBlend blendOf(const Vulkan* v, ColorBlendAttachment* pair, ColorBlendAttachment color, bool surface) {
    pair[0] = color;
    const ColorBlendAttachment kept = {0, 0, 0, 0, 0, 0, 0, surface ? 3u : 0u};
    pair[1] = kept;
    const PipelineColorBlend b = {ST_PIPELINE_COLOR_BLEND, NULL, 0, 0, 0, v->floats ? 2u : 1u, pair, {0, 0, 0, 0}};
    return b;
}

/* wide: the surface with its second word (surfaceWide). On any thread. */
bool renderPassMake(const Vulkan* v, VkRenderPass* out, bool load, bool store, int colorFinal, bool readDepth, bool wide) {
    /* (with a frame in floats, depth is kept for what comes after, and the surface goes beside the color) */
    if (v->floats) store = true;
    const int colorFrom = load ? LAYOUT_COLOR : LAYOUT_UNDEFINED, depthFrom = load ? LAYOUT_DEPTH_READ : LAYOUT_UNDEFINED;
    /* With several samples a pixel what is drawn stays in the pass, and only its mean comes out, unless
     * another pass goes on from it. (The early one has a mean too, that the late one replaces: passes
     * must match to share a framebuffer.) */
    const bool many = v->samples > 1;
    const int samples = v->samples;
    const AttachmentDescription attachments[4] = {
        {0, v->sceneFormat, samples, load ? 0 : 1, many && !store ? 1 : 0 /* store */, 2, 1, colorFrom, many ? LAYOUT_COLOR : colorFinal},
        /* (the depth buffer is not kept unless asked: on a tiler it never leaves the chip) */
        {0, FORMAT_D32, samples, load ? 0 : 1, store ? 0 : 1, 2, 1, depthFrom, store ? LAYOUT_DEPTH_READ : LAYOUT_DEPTH},
        {0, v->sceneFormat, 1, 2, 0, 2, 1, LAYOUT_UNDEFINED, colorFinal},
        {0, wide ? FORMAT_R32G32_UINT : FORMAT_R32_UINT, samples, load ? 0 : 1, 0, 2, 1, colorFrom, colorFinal}};
    /* (readDepth: only colors, which stay for the pass that comes next: a shader reads depth) */
    const AttachmentReference depth = {1, LAYOUT_DEPTH};
    /* (the surface is the last; with samples, only the light is resolved: the surface's first sample is read) */
    const uint32_t surfaceAt = readDepth ? 1u : many ? 3u : 2u;
    const AttachmentReference colors[2] = {{0, LAYOUT_COLOR}, {surfaceAt, LAYOUT_COLOR}}, resolved[2] = {{2, LAYOUT_COLOR}, {~0u, 0}};
    AttachmentDescription used[4];
    uint32_t count = 0;
    used[count++] = attachments[0];
    if (!readDepth) used[count++] = attachments[1];
    if (many && !readDepth) used[count++] = attachments[2];
    if (v->floats) used[count++] = attachments[3];
    const SubpassDescription subpass = {0, 0, 0, NULL, v->floats ? 2 : 1, colors, many && !readDepth ? resolved : NULL, readDepth ? NULL : &depth, 0, NULL};
    const VkFlags draw = STAGE_COLOR | STAGE_EARLY_TESTS | STAGE_LATE_TESTS;
    /* what came before with the same images; and, for the pyramid, what comes after. (All passes have both:
     * they must match, dependencies too, to share pipelines and a framebuffer.) */
    /* (and within: a draw after one whose pixels wrote what it writes, layers' lists drawn one at a time; by region) */
    const SubpassDependency dependencies[3] = {
        {~0u, 0, draw | STAGE_COMPUTE, draw, ACCESS_DEPTH_WRITE | ACCESS_COLOR_WRITE | ACCESS_SHADER_WRITE, ACCESS_COLOR_READ | ACCESS_COLOR_WRITE | ACCESS_DEPTH_READ | ACCESS_DEPTH_WRITE, 0},
        {0, ~0u, draw, STAGE_COMPUTE | STAGE_FRAGMENT, ACCESS_DEPTH_WRITE | ACCESS_COLOR_WRITE, ACCESS_SHADER_READ, 0},
        {0, 0, STAGE_FRAGMENT, STAGE_FRAGMENT, ACCESS_SHADER_WRITE, ACCESS_SHADER_READ | ACCESS_SHADER_WRITE, 1 /* by region */}};
    const RenderPassCreateInfo pass = {ST_RENDER_PASS, NULL, 0, count, used, 1, &subpass, 3, dependencies};
    *out = 0;
    return v->vkCreateRenderPass(v->device, &pass, NULL, out) == VK_OK;
}

/* The scene's passes: renderPass, earlyPass, latePass, midPass, resolvePass. On any thread; none if one fails. */
bool scenePassesCreate(const Vulkan* v, bool wide, int finalLayout, VkRenderPass* passes) {
    for (int i = 0; i < 5; ++i) passes[i] = 0;
    const bool ok = renderPassMake(v, &passes[0], false, false, finalLayout, false, wide) && renderPassMake(v, &passes[1], false, true, LAYOUT_COLOR, false, wide) &&
              renderPassMake(v, &passes[2], true, false, finalLayout, false, wide) && renderPassMake(v, &passes[3], true, true, LAYOUT_COLOR, false, wide) &&
              renderPassMake(v, &passes[4], true, true, LAYOUT_COLOR, true, wide);
    for (int i = 0; i < 5 && !ok; ++i) {
        if (passes[i]) v->vkDestroyRenderPass(v->device, passes[i], NULL);
        passes[i] = 0;
    }
    return ok;
}

/* What the driver compiled last time, if a file was named. */
void pipelineCacheOpen(Vulkan* v) {
    void* data = NULL;
    size_t size = 0;
    FILE* f = v->pipelineCachePath ? fopen(v->pipelineCachePath, "rb") : NULL;
    if (f) {
        fseek(f, 0, SEEK_END);
        const long length = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (length > 0) {
            data = artfMalloc((size_t)length);
            size = fread(data, 1, (size_t)length, f);
        }
        fclose(f);
    }
    /* (the driver looks at the header, and leaves what is from another device or version) */
    const PipelineCacheCreateInfo info = {17 /* ST_PIPELINE_CACHE */, NULL, 0, size, data};
    if (v->vkCreatePipelineCache(v->device, &info, NULL, &v->pipelineCache) != VK_OK) v->pipelineCache = 0;
    artfFree(data);
}

void pipelineCacheSave(Vulkan* v) {
    size_t size = 0;
    if (!v->pipelineCachePath || !v->pipelineCache || v->vkGetPipelineCacheData(v->device, v->pipelineCache, &size, NULL) != VK_OK || !size) return;
    void* data = artfMalloc(size);
    if (v->vkGetPipelineCacheData(v->device, v->pipelineCache, &size, data) == VK_OK) {
        FILE* f = fopen(v->pipelineCachePath, "wb");
        if (f) {
            fwrite(data, 1, size, f);
            fclose(f);
        } else {
            artfLog("artful: could not write %s", v->pipelineCachePath);
        }
    }
    artfFree(data);
}

/* Depth alone, kept for the frame's pass to read; and it waits for the frame before to be done with it. What is there
 * stays: a map that did not change is not drawn again. */
bool shadowPassMake(Vulkan* v) {
    const AttachmentDescription attachment = {0, FORMAT_D32, 1, 0, 0, 2, 1, LAYOUT_DEPTH_READ, LAYOUT_DEPTH_READ};
    const AttachmentReference depth = {0, LAYOUT_DEPTH};
    const SubpassDescription subpass = {0, 0, 0, NULL, 0, NULL, NULL, &depth, 0, NULL};
    const VkFlags tests = STAGE_EARLY_TESTS | STAGE_LATE_TESTS;
    /* (what reads the maps: the frame's pixels, and the fog grid's compute, before and after) */
    const SubpassDependency dependencies[2] = {
        {~0u, 0, STAGE_FRAGMENT | STAGE_COMPUTE, tests, ACCESS_SHADER_READ, ACCESS_DEPTH_READ | ACCESS_DEPTH_WRITE, 0},
        {0, ~0u, tests, STAGE_FRAGMENT | STAGE_COMPUTE, ACCESS_DEPTH_WRITE, ACCESS_SHADER_READ, 0}};
    const RenderPassCreateInfo pass = {ST_RENDER_PASS, NULL, 0, 1, &attachment, 1, &subpass, 2, dependencies};
    return v->vkCreateRenderPass(v->device, &pass, NULL, &v->shadowPass) == VK_OK;
}

/* An atlas of this size, in the layout the frame's pass reads. */
bool shadowAtlasMake(Vulkan* v, int width, int height) {
    if (v->shadowFramebuffer) v->vkDestroyFramebuffer(v->device, v->shadowFramebuffer, NULL);
    v->shadowFramebuffer = 0;
    imageFree(v, &v->shadowAtlas);
    if (!imageNew(v, &v->shadowAtlas, FORMAT_D32, width, height, 1, 1, IMAGE_DEPTH | IMAGE_SAMPLED, ASPECT_DEPTH)) return false;
    FramebufferCreateInfo info = {ST_FRAMEBUFFER, NULL, 0, v->shadowPass, 1, &v->shadowAtlas.view, (uint32_t)width, (uint32_t)height, 1};
    if (v->vkCreateFramebuffer(v->device, &info, NULL, &v->shadowFramebuffer) != VK_OK) return false;
    VkCommandBuffer c = oneShotBegin(v);
    const ImageMemoryBarrier b = {ST_IMAGE_MEMORY_BARRIER, NULL, 0, ACCESS_SHADER_READ, LAYOUT_UNDEFINED, LAYOUT_DEPTH_READ, ~0u, ~0u, v->shadowAtlas.image, {ASPECT_DEPTH, 0, 1, 0, 1}};
    v->vkCmdPipelineBarrier(c, STAGE_TOP, STAGE_FRAGMENT, 0, 0, NULL, 0, NULL, 1, &b);
    if (!oneShotEnd(v, c)) return false;
    memset(v->shadowKeys, 0, sizeof v->shadowKeys);
    v->shadowWidth = width;
    v->shadowHeight = height;
    const DescriptorImageInfo image = {v->depthSampler, v->shadowAtlas.view, LAYOUT_DEPTH_READ};
    for (int i = 0; i < SLOTS; ++i) {
        const WriteDescriptorSet w = {ST_WRITE_DESCRIPTOR_SET, NULL, v->slots[i].set, 25, 0, 1, DESCRIPTOR_COMBINED, &image, NULL, NULL};
        v->vkUpdateDescriptorSets(v->device, 1, &w, 0, NULL);
    }
    return true;
}


Features featuresOf(const Vulkan* v) {
    const Features f = {v->probes, v->walls, v->froxels, v->coats, v->softShadows, v->shapes, v->viewShaders, v->meshletShaders, v->tracedShadows, v->tracedHardware};
    return f;
}

void featuresSet(Vulkan* v, const Features* f) {
    v->probes = f->probes;
    v->walls = f->walls;
    v->froxels = f->froxels;
    v->coats = f->coats;
    v->softShadows = f->softShadows;
    v->shapes = f->shapes;
    v->viewShaders = f->viewShaders;
    v->meshletShaders = f->meshletShaders;
    v->tracedShadows = f->tracedShadows;
    v->tracedHardware = f->tracedHardware;
}

uint32_t variantOf(bool floats, const Features* f) {
    return (floats ? ARTF_SHADER_FLOATS : 0u) | (f->probes ? ARTF_SHADER_PROBES : 0u) | (f->probes && f->walls ? ARTF_SHADER_WALLS : 0u) | (f->froxels ? ARTF_SHADER_FROXELS : 0u) |
           (f->coats ? ARTF_SHADER_COAT : 0u) | (f->softShadows ? ARTF_SHADER_SOFT : 0u) | (f->viewShaders ? ARTF_SHADER_VIEW : 0u) | (f->meshletShaders ? ARTF_SHADER_MESHLETS : 0u) | (f->tracedShadows ? ARTF_SHADER_TRACED : 0u) | (f->shapes ? ARTF_SHADER_SHAPES : 0u) | (f->tracedShadows && f->tracedHardware ? ARTF_SHADER_RAY_QUERY : 0u);
}

uint32_t shaderVariant(const Vulkan* v) {
    const Features f = featuresOf(v);
    return variantOf(v->floats, &f);
}


void pipelineRetire(Vulkan* v, VkPipeline pipeline) {
    if (!pipeline) return;
    Retired r;
    memset(&r, 0, sizeof r);
    r.pipeline = pipeline;
    r.frame = v->frameNumber;
    artfVecPush(&v->retired, r);
}

void structureRetire(Vulkan* v, VkAccelerationStructureKHR structure) {
    if (!structure) return;
    Retired r;
    memset(&r, 0, sizeof r);
    r.structure = structure;
    r.frame = v->frameNumber;
    artfVecPush(&v->retired, r);
}

/* (the buffer is the list's now: b is left empty) */
void bufferRetire(Vulkan* v, GpuBuffer* b) {
    if (!b->buffer && !b->memory) return;
    Retired r;
    memset(&r, 0, sizeof r);
    r.buffer = *b;
    r.frame = v->frameNumber;
    artfVecPush(&v->retired, r);
    memset(b, 0, sizeof *b);
}

/* all: the device is idle. Otherwise those whose last frame the slot's fence has seen done: frames before
 * frameNumber - SLOTS + 1 are. (Structures go before the buffers that hold them: they were retired first.) */
void retiredFree(Vulkan* v, bool all) {
    size_t kept = 0;
    for (size_t i = 0; i < v->retired.len; ++i) {
        Retired r = v->retired.items[i];
        if (all || r.frame + SLOTS - 1 <= v->frameNumber) {
            if (r.pipeline) v->vkDestroyPipeline(v->device, r.pipeline, NULL);
            if (r.structure) v->vkDestroyAccelerationStructureKHR(v->device, r.structure, NULL);
            bufferFree(v, &r.buffer);
        } else {
            v->retired.items[kept++] = r;
        }
    }
    v->retired.len = kept;
}

/* The scene's pixel shader's code: with shadows from rays, what they meet may be of the program's own materials. */
uint32_t* sceneFragmentCode(const Vulkan* v, bool cutout, const ArtfGraphData* graphs, uint32_t count, uint32_t variant, size_t* size) {
    if (!cutout && !(variant & ARTF_SHADER_TRACED)) graphs = NULL, count = 0;
    return artfShaderSceneFragment(size, v->maxTextures, cutout, graphs, count, 0, variant);
}

/* The overdraw view's: each triangle adds its step wherever it is, whatever is in front (artfShaderOverdraw). */
bool overdrawPipelinesMake(Vulkan* v) {
    if (v->overdrawPipelines[0]) return true;
    size_t size;
    uint32_t* code = artfShaderOverdraw(&size, v->floats);
    VkShaderModule fragment = shaderModule(v, code, size);
    if (!fragment) return false;
    const VertexBinding binding = {0, sizeof(ArtfPackedVertex), 0};
    const VertexAttribute attributes[3] = {{0, 0, FORMAT_RGBA16_UNORM, 0}, {1, 0, FORMAT_RG16_SNORM, 8}, {2, 0, FORMAT_RG16_FLOAT, 12}};
    const PipelineVertexInput vertexInput = {ST_PIPELINE_VERTEX_INPUT, NULL, 0, 1, &binding, 3, attributes};
    const PipelineInputAssembly assembly = {ST_PIPELINE_INPUT_ASSEMBLY, NULL, 0, 3 /* triangle list */, 0};
    const PipelineViewport viewport = {ST_PIPELINE_VIEWPORT, NULL, 0, 1, NULL, 1, NULL};
    const PipelineRasterization rasterBack = {ST_PIPELINE_RASTERIZATION, NULL, 0, 0, 0, 0, 2 /* back */, 0 /* counter-clockwise */, 0, 0, 0, 0, 1.f};
    const PipelineRasterization rasterNone = {ST_PIPELINE_RASTERIZATION, NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1.f};
    const PipelineMultisample multisample = {ST_PIPELINE_MULTISAMPLE, NULL, 0, v->samples, 0, 0, NULL, 0, 0};
    PipelineDepthStencil depthOff;
    memset(&depthOff, 0, sizeof depthOff);
    depthOff.sType = ST_PIPELINE_DEPTH_STENCIL;
    const ColorBlendAttachment added = {1, 1 /* one */, 1 /* one */, 0, 0 /* zero */, 1 /* one: alpha stays */, 0, 15};
    ColorBlendAttachment pair[2];
    const PipelineColorBlend blend = blendOf(v, pair, added, false);
    const int states[2] = {0, 1};
    const PipelineDynamic dynamic = {ST_PIPELINE_DYNAMIC, NULL, 0, 2, states};
    const PipelineShaderStage stages[2] = {{ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_VERTEX, v->sceneVertexModule, "main", NULL}, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_FRAGMENT, fragment, "main", NULL}};
    const GraphicsPipelineCreateInfo info[2] = {
        {ST_GRAPHICS_PIPELINE, NULL, 0, 2, stages, &vertexInput, &assembly, NULL, &viewport, &rasterBack, &multisample, &depthOff, &blend, &dynamic, v->pipelineLayout, v->renderPass, 0, 0, -1},
        {ST_GRAPHICS_PIPELINE, NULL, 0, 2, stages, &vertexInput, &assembly, NULL, &viewport, &rasterNone, &multisample, &depthOff, &blend, &dynamic, v->pipelineLayout, v->renderPass, 0, 0, -1}};
    const bool ok = v->vkCreateGraphicsPipelines(v->device, v->pipelineCache, 2, info, NULL, v->overdrawPipelines) == VK_OK;
    v->vkDestroyShaderModule(v->device, fragment, NULL);
    return ok;
}

/* The pipeline of what is plain and solid, from its pixel shader, in pass (renderPass's or a new one's). On any
 * thread: it reads only what stays while it is made (passes are made again only by shaderJobEnd). */
bool scenePipelineCreate(Vulkan* v, VkShaderModule fragment, VkRenderPass pass, VkPipeline* out) {
    const VertexBinding binding = {0, sizeof(ArtfPackedVertex), 0};
    const VertexAttribute attributes[3] = {{0, 0, FORMAT_RGBA16_UNORM, 0}, {1, 0, FORMAT_RG16_SNORM, 8}, {2, 0, FORMAT_RG16_FLOAT, 12}};
    const PipelineVertexInput vertexInput = {ST_PIPELINE_VERTEX_INPUT, NULL, 0, 1, &binding, 3, attributes};
    const PipelineInputAssembly assembly = {ST_PIPELINE_INPUT_ASSEMBLY, NULL, 0, 3 /* triangle list */, 0};
    const PipelineViewport viewport = {ST_PIPELINE_VIEWPORT, NULL, 0, 1, NULL, 1, NULL};
    const PipelineRasterization rasterBack = {ST_PIPELINE_RASTERIZATION, NULL, 0, 0, 0, 0, 2 /* back */, 0 /* counter-clockwise */, 0, 0, 0, 0, 1.f};
    const PipelineMultisample multisample = {ST_PIPELINE_MULTISAMPLE, NULL, 0, v->samples, 0, 0, NULL, 0, 0};
    PipelineDepthStencil depthOn;
    memset(&depthOn, 0, sizeof depthOn);
    depthOn.sType = ST_PIPELINE_DEPTH_STENCIL;
    depthOn.depthTest = 1;
    depthOn.depthWrite = 1;
    depthOn.depthCompare = 4; /* greater: depth is reversed */
    const ColorBlendAttachment opaque = {0, 0, 0, 0, 0, 0, 0, 15};
    ColorBlendAttachment pairOff[2];
    const PipelineColorBlend blendOff = blendOf(v, pairOff, opaque, true);
    const int states[2] = {0, 1};
    const PipelineDynamic dynamic = {ST_PIPELINE_DYNAMIC, NULL, 0, 2, states};
    const PipelineShaderStage stages[2] = {{ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_VERTEX, v->sceneVertexModule, "main", NULL}, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_FRAGMENT, fragment, "main", NULL}};
    const GraphicsPipelineCreateInfo info = {ST_GRAPHICS_PIPELINE, NULL, 0, 2, stages, &vertexInput, &assembly, NULL, &viewport, &rasterBack, &multisample, &depthOn, &blendOff, &dynamic, v->pipelineLayout, pass, 0, 0, -1};
    *out = 0;
    return v->vkCreateGraphicsPipelines(v->device, v->pipelineCache, 1, &info, NULL, out) == VK_OK;
}

/* What depends on a scene's materials of its own: all that is drawn with the cutout shader, which has a branch
 * for each. That is what plain surfaces do not pay for. */
/* made[4]: a layer's depth, a layer, what has holes, particles. On any thread, as scenePipelineCreate. */
bool cutoutPipelinesCreate(Vulkan* v, VkShaderModule fragment, VkRenderPass pass, VkPipeline* made) {
    const VertexBinding binding = {0, sizeof(ArtfPackedVertex), 0};
    const VertexAttribute attributes[3] = {{0, 0, FORMAT_RGBA16_UNORM, 0}, {1, 0, FORMAT_RG16_SNORM, 8}, {2, 0, FORMAT_RG16_FLOAT, 12}};
    const PipelineVertexInput vertexInput = {ST_PIPELINE_VERTEX_INPUT, NULL, 0, 1, &binding, 3, attributes};
    const PipelineInputAssembly assembly = {ST_PIPELINE_INPUT_ASSEMBLY, NULL, 0, 3 /* triangle list */, 0};
    const PipelineViewport viewport = {ST_PIPELINE_VIEWPORT, NULL, 0, 1, NULL, 1, NULL};
    const PipelineRasterization rasterNone = {ST_PIPELINE_RASTERIZATION, NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1.f};
    const PipelineMultisample multisample = {ST_PIPELINE_MULTISAMPLE, NULL, 0, v->samples, 0, 0, NULL, 0, 0};
    PipelineDepthStencil depthOn;
    memset(&depthOn, 0, sizeof depthOn);
    depthOn.sType = ST_PIPELINE_DEPTH_STENCIL;
    depthOn.depthTest = 1;
    depthOn.depthWrite = 1;
    depthOn.depthCompare = 4; /* greater: depth is reversed */
    PipelineDepthStencil depthSame = depthOn;
    depthSame.depthWrite = 0;
    depthSame.depthCompare = 2; /* equal */
    const ColorBlendAttachment opaque = {0, 0, 0, 0, 0, 0, 0, 15};
    const ColorBlendAttachment premultiplied = {1, 1 /* one */, 7 /* one minus source alpha */, 0, 1, 7, 0, 15};
    const ColorBlendAttachment noColor = {0, 0, 0, 0, 0, 0, 0, 0};
    ColorBlendAttachment pairOff[2], pairOn[2], pairNone[2];
    const PipelineColorBlend blendOff = blendOf(v, pairOff, opaque, true);
    const PipelineColorBlend blendOn = blendOf(v, pairOn, premultiplied, false);
    const PipelineColorBlend blendNone = blendOf(v, pairNone, noColor, false);
    const int states[2] = {0, 1};
    const PipelineDynamic dynamic = {ST_PIPELINE_DYNAMIC, NULL, 0, 2, states};
    const PipelineShaderStage stages[2] = {{ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_VERTEX, v->sceneVertexModule, "main", NULL}, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_FRAGMENT, fragment, "main", NULL}};
    const GraphicsPipelineCreateInfo info[4] = {
        /* a layer: its depth alone, so that its nearest surface is known; then that surface, blended */
        {ST_GRAPHICS_PIPELINE, NULL, 0, 2, stages, &vertexInput, &assembly, NULL, &viewport, &rasterNone, &multisample, &depthOn, &blendNone, &dynamic, v->pipelineLayout, pass, 0, 0, -1},
        {ST_GRAPHICS_PIPELINE, NULL, 0, 2, stages, &vertexInput, &assembly, NULL, &viewport, &rasterNone, &multisample, &depthSame, &blendOn, &dynamic, v->pipelineLayout, pass, 0, 0, -1},
        /* what has holes or backs: solid, but it decides itself which pixels it has */
        {ST_GRAPHICS_PIPELINE, NULL, 0, 2, stages, &vertexInput, &assembly, NULL, &viewport, &rasterNone, &multisample, &depthOn, &blendOff, &dynamic, v->pipelineLayout, pass, 0, 0, -1},
        /* particles: a quad has one surface, so one pass does what the two do for a layer */
        {ST_GRAPHICS_PIPELINE, NULL, 0, 2, stages, &vertexInput, &assembly, NULL, &viewport, &rasterNone, &multisample, &depthOn, &blendOn, &dynamic, v->pipelineLayout, pass, 0, 0, -1}};
    for (int i = 0; i < 4; ++i) made[i] = 0;
    return v->vkCreateGraphicsPipelines(v->device, v->pipelineCache, 4, info, NULL, made) == VK_OK;
}

/* What the cutout shader draws, and what is made from it when first asked (the layers' lists), retired. */
void cutoutPipelinesRetire(Vulkan* v) {
    VkPipeline* const old[6] = {&v->layerDepthPipeline, &v->layerPipeline, &v->cutoutPipeline, &v->layerListPipeline, &v->particlePipeline, &v->layerGridPipeline};
    for (int i = 0; i < 6; ++i) {
        pipelineRetire(v, *old[i]);
        *old[i] = 0;
    }
}

/* What is behind the scene, what was traced, the environment and the fog over what is behind, in pass, as
 * pipelinesMake has them in a frame in floats: made[4]. On any thread, as scenePipelineCreate. */
bool behindPipelinesCreate(Vulkan* v, VkRenderPass pass, VkPipeline* made) {
    for (int i = 0; i < 4; ++i) made[i] = 0;
    size_t size;
    uint32_t* code = artfShaderOverlayVertex(&size);
    VkShaderModule whole = shaderModule(v, code, size);
    code = artfShaderBackgroundBackdrop(&size, true);
    VkShaderModule backdrop = shaderModule(v, code, size);
    code = artfShaderTraceShow(&size, true);
    VkShaderModule traced = shaderModule(v, code, size);
    code = artfShaderEnvironmentBackdrop(&size, true);
    VkShaderModule environment = shaderModule(v, code, size);
    code = artfShaderFogBackdrop(&size);
    VkShaderModule fog = shaderModule(v, code, size);
    bool ok = whole && backdrop && traced && environment && fog;
    if (ok) {
        const PipelineVertexInput noInput = {ST_PIPELINE_VERTEX_INPUT, NULL, 0, 0, NULL, 0, NULL};
        const PipelineInputAssembly assembly = {ST_PIPELINE_INPUT_ASSEMBLY, NULL, 0, 3 /* triangle list */, 0};
        const PipelineViewport viewport = {ST_PIPELINE_VIEWPORT, NULL, 0, 1, NULL, 1, NULL};
        const PipelineRasterization rasterNone = {ST_PIPELINE_RASTERIZATION, NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1.f};
        const PipelineMultisample multisample = {ST_PIPELINE_MULTISAMPLE, NULL, 0, v->samples, 0, 0, NULL, 0, 0};
        PipelineDepthStencil depthOff;
        memset(&depthOff, 0, sizeof depthOff);
        depthOff.sType = ST_PIPELINE_DEPTH_STENCIL;
        const ColorBlendAttachment opaque = {0, 0, 0, 0, 0, 0, 0, 15};
        const ColorBlendAttachment premultiplied = {1, 1 /* one */, 7 /* one minus source alpha */, 0, 1, 7, 0, 15};
        const ColorBlendAttachment overBehind = {1, 8 /* destination alpha */, 6 /* source alpha */, 0, 0 /* zero */, 1 /* one */, 0, 15};
        ColorBlendAttachment pairBehind[2], pairOn[2], pairFog[2];
        const PipelineColorBlend blendBehind = blendOf(v, pairBehind, opaque, false), blendOn = blendOf(v, pairOn, premultiplied, false), fogBlend = blendOf(v, pairFog, overBehind, false);
        const int states[2] = {0, 1};
        const PipelineDynamic dynamic = {ST_PIPELINE_DYNAMIC, NULL, 0, 2, states};
        const VkShaderModule fragments[4] = {backdrop, traced, environment, fog};
        const PipelineColorBlend* const blends[4] = {&blendBehind, &blendOn, &blendBehind, &fogBlend};
        for (int i = 0; i < 4 && ok; ++i) {
            const PipelineShaderStage stages[2] = {{ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_VERTEX, whole, "main", NULL}, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_FRAGMENT, fragments[i], "main", NULL}};
            const GraphicsPipelineCreateInfo info = {ST_GRAPHICS_PIPELINE, NULL, 0, 2, stages, &noInput, &assembly, NULL, &viewport, &rasterNone, &multisample, &depthOff, blends[i], &dynamic, v->pipelineLayout, pass, 0, 0, -1};
            ok = v->vkCreateGraphicsPipelines(v->device, v->pipelineCache, 1, &info, NULL, &made[i]) == VK_OK;
        }
    }
    const VkShaderModule modules[5] = {whole, backdrop, traced, environment, fog};
    for (int i = 0; i < 5; ++i)
        if (modules[i]) v->vkDestroyShaderModule(v->device, modules[i], NULL);
    return ok;
}

/* The layers' lists: made[0] lists each pixel's layers (the cutout shader, of variant, that lists), in pass; made[1]
 * resolves them, in resolvePass with samples, pass without. Those asked for (list, resolve). On any thread, as
 * scenePipelineCreate. */
bool layerListPipelinesCreate(Vulkan* v, const ArtfGraphData* graphs, uint32_t count, uint32_t variant, VkRenderPass pass, VkRenderPass resolvePass, bool list, bool resolve, VkPipeline* made) {
    made[0] = made[1] = made[2] = 0;
    size_t size;
    uint32_t* code;
    VkShaderModule fragment = 0, whole = 0, resolving = 0, gridding = 0;
    if (list) {
        const int lists = v->samples | (v->depthCoverage ? ARTF_LISTS_POST_DEPTH : 0);
        code = artfShaderSceneFragment(&size, v->maxTextures, true, graphs, count, lists, variant & ~(uint32_t)ARTF_SHADER_FLOATS);
        fragment = shaderModule(v, code, size);
        code = artfShaderSceneFragment(&size, v->maxTextures, true, graphs, count, lists | ARTF_LISTS_GRID, variant & ~(uint32_t)ARTF_SHADER_FLOATS);
        gridding = shaderModule(v, code, size);
    }
    if (resolve) {
        code = artfShaderOverlayVertex(&size);
        whole = shaderModule(v, code, size);
        code = artfShaderLayerResolve(&size, v->samples);
        resolving = shaderModule(v, code, size);
    }
    bool ok = (!list || (fragment && gridding)) && (!resolve || (whole && resolving));
    if (ok) {
        const VertexBinding binding = {0, sizeof(ArtfPackedVertex), 0};
        const VertexAttribute attributes[3] = {{0, 0, FORMAT_RGBA16_UNORM, 0}, {1, 0, FORMAT_RG16_SNORM, 8}, {2, 0, FORMAT_RG16_FLOAT, 12}};
        const PipelineVertexInput vertexInput = {ST_PIPELINE_VERTEX_INPUT, NULL, 0, 1, &binding, 3, attributes};
        const PipelineVertexInput noInput = {ST_PIPELINE_VERTEX_INPUT, NULL, 0, 0, NULL, 0, NULL};
        const PipelineInputAssembly assembly = {ST_PIPELINE_INPUT_ASSEMBLY, NULL, 0, 3 /* triangle list */, 0};
        const PipelineViewport viewport = {ST_PIPELINE_VIEWPORT, NULL, 0, 1, NULL, 1, NULL};
        const PipelineRasterization rasterNone = {ST_PIPELINE_RASTERIZATION, NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1.f};
        const PipelineMultisample multisample = {ST_PIPELINE_MULTISAMPLE, NULL, 0, v->samples, 0, 0, NULL, 0, 0};
        /* (a layer's surfaces, by sample: each tested against its own depth) */
        const PipelineMultisample bySample = {ST_PIPELINE_MULTISAMPLE, NULL, 0, v->samples, v->samples > 1 && !v->depthCoverage, 1.f, NULL, 0, 0};
        PipelineDepthStencil depthLook, depthOff;
        memset(&depthLook, 0, sizeof depthLook);
        depthLook.sType = ST_PIPELINE_DEPTH_STENCIL;
        depthLook.depthTest = 1;
        depthLook.depthCompare = 4; /* greater: depth is reversed */
        memset(&depthOff, 0, sizeof depthOff);
        depthOff.sType = ST_PIPELINE_DEPTH_STENCIL;
        const ColorBlendAttachment noColor = {0, 0, 0, 0, 0, 0, 0, 0};
        const ColorBlendAttachment premultiplied = {1, 1 /* one */, 7 /* one minus source alpha */, 0, 1, 7, 0, 15};
        ColorBlendAttachment pairNone[2], pairOn[2];
        const PipelineColorBlend blendNone = blendOf(v, pairNone, noColor, false);
        const PipelineColorBlend blendOn = blendOf(v, pairOn, premultiplied, false);
        const int states[2] = {0, 1};
        const PipelineDynamic dynamic = {ST_PIPELINE_DYNAMIC, NULL, 0, 2, states};
        const PipelineShaderStage listStages[2] = {{ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_VERTEX, v->sceneVertexModule, "main", NULL}, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_FRAGMENT, fragment, "main", NULL}};
        const PipelineShaderStage resolveStages[2] = {{ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_VERTEX, whole, "main", NULL}, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_FRAGMENT, resolving, "main", NULL}};
        const PipelineShaderStage gridStages[2] = {{ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_VERTEX, v->sceneVertexModule, "main", NULL}, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_FRAGMENT, gridding, "main", NULL}};
        /* (the resolve goes in the pass the sums were drawn in, after them, by sample as they are) */
        (void)resolvePass;
        const GraphicsPipelineCreateInfo info[3] = {
            {ST_GRAPHICS_PIPELINE, NULL, 0, 2, listStages, &vertexInput, &assembly, NULL, &viewport, &rasterNone, &bySample, &depthLook, &blendNone, &dynamic, v->pipelineLayout, pass, 0, 0, -1},
            {ST_GRAPHICS_PIPELINE, NULL, 0, 2, resolveStages, &noInput, &assembly, NULL, &viewport, &rasterNone, &multisample, &depthOff, &blendOn, &dynamic, v->pipelineLayout, pass, 0, 0, -1},
            {ST_GRAPHICS_PIPELINE, NULL, 0, 2, gridStages, &vertexInput, &assembly, NULL, &viewport, &rasterNone, &bySample, &depthLook, &blendNone, &dynamic, v->pipelineLayout, pass, 0, 0, -1}};
        if (list) ok = v->vkCreateGraphicsPipelines(v->device, v->pipelineCache, 1, &info[0], NULL, &made[0]) == VK_OK;
        if (list && ok) ok = v->vkCreateGraphicsPipelines(v->device, v->pipelineCache, 1, &info[2], NULL, &made[2]) == VK_OK;
        if (resolve && ok) ok = v->vkCreateGraphicsPipelines(v->device, v->pipelineCache, 1, &info[1], NULL, &made[1]) == VK_OK;
    }
    const VkShaderModule modules[4] = {fragment, whole, resolving, gridding};
    for (int i = 0; i < 4; ++i)
        if (modules[i]) v->vkDestroyShaderModule(v->device, modules[i], NULL);
    return ok;
}

/* A compute pipeline from code. On any thread, as scenePipelineCreate. */
bool computePipelineCreate(Vulkan* v, uint32_t* code, size_t size, VkPipeline* out) {
    *out = 0;
    VkShaderModule module = shaderModule(v, code, size);
    if (!module) return false;
    const ComputePipelineCreateInfo info = {ST_COMPUTE_PIPELINE, NULL, 0, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_COMPUTE, module, "main", NULL}, v->pipelineLayout, 0, -1};
    const bool ok = v->vkCreateComputePipelines(v->device, v->pipelineCache, 1, &info, NULL, out) == VK_OK;
    v->vkDestroyShaderModule(v->device, module, NULL);
    return ok;
}

/* ---- pipelines made off the frame ----
 *
 * A feature that asks for other pipelines has them made on a thread of its own: the frames go on with those they had
 * until they are made, a few frames later; then they take the new ones, and the old are destroyed once the frames that
 * drew with them are done. Until then:
 * - another variant of the scene's pixel shaders (probes, their walls, fog's grid, a coat, soft shadows, shapes, a
 *   view, shadows from rays): frames are drawn without the feature (shadows from rays: with the shadow maps), or still
 *   with it when it was turned off;
 * - a program's new materials: the cutout shader there has no branch for them, and draws them by their material's own
 *   values (its color and texture, roughness, metal, emission), which is what their graph starts from;
 * - temporal antialiasing or motion blur, whose surface takes a second word, and so other passes, targets and pipelines
 *   drawn in them: frames are drawn without them (or still with them). The frame that takes the new passes waits for
 *   the device, to make the targets again, as it did when they were made before the frame;
 * - the tracing shader: the frame is rasterized; for new materials, the one there draws them by their own values, and
 *   what was gathered starts over when the new one comes;
 * - reflections from rays: those from the screen.
 * The first frame makes what it asks for before it draws, as does the frame after artfRendererWarm, and a scene whose
 * materials of its own are not those the pipelines there were made for (another scene's); artfVulkanShadersWait (the
 * tests) makes every change so. That waits for the compiling, never for the device. */
bool artfVulkanShadersWait = false;

/* The scene's materials of its own, copied: the program may change the scene's while a thread makes their shaders. */
ArtfGraphData* graphsCopy(const ArtfScene* scene) {
    const uint32_t count = (uint32_t)scene->graphs.len;
    if (!count) return NULL;
    ArtfGraphData* graphs = artfMalloc(count * sizeof *graphs);
    for (uint32_t i = 0; i < count; ++i) {
        graphs[i] = scene->graphs.items[i];
        graphs[i].nodes = artfMalloc((size_t)graphs[i].count * sizeof(ArtfGraphNode) + 1);
        memcpy(graphs[i].nodes, scene->graphs.items[i].nodes, (size_t)graphs[i].count * sizeof(ArtfGraphNode));
    }
    return graphs;
}

void graphsFree(ArtfGraphData* graphs, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) artfFree(graphs[i].nodes);
    artfFree(graphs);
}

/* Pipelines made for the first count materials of their own of the scene madeFor draw this scene: those past count by
 * their own values. (Graphs are only added; another scene's would be other branches.) */
bool graphsPrefix(uint32_t count, uint64_t madeFor, const ArtfScene* scene) {
    return count <= scene->graphs.len && (!count || madeFor == scene->id);
}


void shaderJobRun(void* user) {
    ShaderJob* j = user;
    Vulkan* v = j->v;
    bool ok = true;
    if (j->passesNew) {
        ok = scenePassesCreate(v, j->wide, LAYOUT_SHADER_READ, j->passes) && behindPipelinesCreate(v, j->passes[0], j->behind);
        j->pass = j->passes[0];
        j->resolvePass = j->passes[4];
    }
    VkShaderModule plain = 0, cutout = 0;
    if (ok) {
        size_t size;
        uint32_t* code = sceneFragmentCode(v, false, j->graphs, j->graphCount, j->variant, &size);
        plain = shaderModule(v, code, size);
        code = sceneFragmentCode(v, true, j->graphs, j->graphCount, j->variant, &size);
        cutout = shaderModule(v, code, size);
        ok = plain && cutout && scenePipelineCreate(v, plain, j->pass, &j->plain) && cutoutPipelinesCreate(v, cutout, j->pass, j->cutout);
    }
    if (ok && (j->lists || j->resolve)) ok = layerListPipelinesCreate(v, j->graphs, j->graphCount, j->variant, j->pass, j->resolvePass, j->lists, j->resolve, j->lister);
    if (plain) v->vkDestroyShaderModule(v->device, plain, NULL);
    if (cutout) v->vkDestroyShaderModule(v->device, cutout, NULL);
    j->ok = ok;
}

/* The scene's pixel shaders for want and its materials of its own, in passes for wide: on a thread of their own, or
 * now. */
void shaderJobStart(Vulkan* v, const ArtfScene* scene, const Features* want, bool wide, bool now) {
    ShaderJob* j = ARTF_NEW(ShaderJob);
    j->v = v;
    j->features = *want;
    j->variant = variantOf(v->floats, want);
    j->graphs = graphsCopy(scene);
    j->graphCount = (uint32_t)scene->graphs.len;
    j->scene = scene->id;
    j->wide = wide;
    j->passesNew = wide != v->surfaceWide;
    j->lists = v->layerListPipeline != 0;
    j->resolve = j->passesNew && v->layerResolvePipeline;
    j->pass = v->renderPass;
    j->resolvePass = v->resolvePass;
    v->shaderJob = j;
    if (now) shaderJobRun(j);
    else j->task = artfTaskStart(shaderJobRun, j);
}

/* Waits for what is being made (if anything), and takes it if use and it can draw the scene; or throws it away, as no
 * frame has drawn with it. false: use, and making failed. */
bool shaderJobEnd(Vulkan* v, const ArtfScene* scene, bool use) {
    ShaderJob* j = v->shaderJob;
    if (!j) return true;
    artfTaskJoin(j->task);
    v->shaderJob = NULL;
    const bool failed = use && !j->ok;
    if (use && j->ok && scene && graphsPrefix(j->graphCount, j->scene, scene)) {
        if (j->passesNew) {
            /* other passes: the frames drawn in the old ones are done with them, and the targets are made again */
            v->vkDeviceWaitIdle(v->device);
            VkRenderPass* const passes[5] = {&v->renderPass, &v->earlyPass, &v->latePass, &v->midPass, &v->resolvePass};
            VkPipeline* const behind[4] = {&v->backdropPipeline, &v->traceShowPipeline, &v->environmentPipeline, &v->fogPipeline};
            for (int i = 0; i < 5; ++i) {
                if (*passes[i]) v->vkDestroyRenderPass(v->device, *passes[i], NULL);
                *passes[i] = j->passes[i];
            }
            for (int i = 0; i < 4; ++i) {
                if (*behind[i]) v->vkDestroyPipeline(v->device, *behind[i], NULL);
                *behind[i] = j->behind[i];
            }
            /* (the overdraw view's, made again when it is asked for) */
            for (int i = 0; i < 2; ++i) {
                if (v->overdrawPipelines[i]) v->vkDestroyPipeline(v->device, v->overdrawPipelines[i], NULL);
                v->overdrawPipelines[i] = 0;
            }
            if (v->layerResolvePipeline) v->vkDestroyPipeline(v->device, v->layerResolvePipeline, NULL);
            v->layerResolvePipeline = j->lister[1];
            v->surfaceWide = j->wide;
            v->targetsStale = true;
        }
        if (j->features.tracedHardware && !v->tracedHardware) artfLog("artful: shadows from rays go through the ray tracing hardware");
        pipelineRetire(v, v->scenePipeline);
        cutoutPipelinesRetire(v);
        v->scenePipeline = j->plain;
        v->layerDepthPipeline = j->cutout[0];
        v->layerPipeline = j->cutout[1];
        v->cutoutPipeline = j->cutout[2];
        v->particlePipeline = j->cutout[3];
        v->layerListPipeline = j->lister[0];
        v->layerGridPipeline = j->lister[2];
        featuresSet(v, &j->features);
        v->graphCount = j->graphCount;
        v->graphScene = j->scene;
    } else {
        VkPipeline* const made[12] = {&j->plain, &j->cutout[0], &j->cutout[1], &j->cutout[2], &j->cutout[3], &j->lister[0], &j->lister[1], &j->lister[2], &j->behind[0], &j->behind[1], &j->behind[2], &j->behind[3]};
        for (int i = 0; i < 12; ++i)
            if (*made[i]) v->vkDestroyPipeline(v->device, *made[i], NULL);
        for (int i = 0; i < 5; ++i)
            if (j->passes[i]) v->vkDestroyRenderPass(v->device, j->passes[i], NULL);
    }
    graphsFree(j->graphs, j->graphCount);
    artfFree(j);
    return !failed;
}


ComputeKey computeKeyOf(const Vulkan* v, const ArtfScene* scene, int kind) {
    ComputeKey k = {false, v->rayQuery, false, (uint32_t)scene->graphs.len, scene->id};
    if (kind == MAKE_TRACE) {
        k.guides = scene->trace.desc.denoise;
        k.hardware = scene->trace.desc.hardware && v->rayQuery;
        k.more = scene->moreMaterials > 0 || artfSceneShapedLights(scene);
    }
    return k;
}

bool computeFlagsSame(const ComputeKey* a, const ComputeKey* b) { return a->guides == b->guides && a->hardware == b->hardware && a->more == b->more; }

void computeJobRun(void* user) {
    ComputeJob* j = user;
    Vulkan* v = j->v;
    size_t size;
    uint32_t* code = j->kind == MAKE_TRACE ? artfShaderTrace(&size, v->maxTextures, j->graphs, j->key.graphCount, j->key.guides, j->key.hardware, j->key.more)
                                           : artfShaderPostReflectionTraced(&size, v->maxTextures, j->graphs, j->key.graphCount, j->key.hardware);
    j->ok = computePipelineCreate(v, code, size, &j->made);
    if (j->ok && j->denoise) {
        code = artfShaderDenoiseLight(&size);
        j->ok = computePipelineCreate(v, code, size, &j->denoised[0]);
        code = artfShaderDenoiseTime(&size);
        j->ok = j->ok && computePipelineCreate(v, code, size, &j->denoised[1]);
        code = artfShaderDenoiseVariance(&size);
        j->ok = j->ok && computePipelineCreate(v, code, size, &j->denoised[2]);
        for (int i = 0; i < ARTF_DENOISE_PASSES; ++i) {
            code = artfShaderDenoiseSpace(&size, i);
            j->ok = j->ok && computePipelineCreate(v, code, size, &j->denoised[3 + i]);
        }
        code = artfShaderDenoiseEnd(&size);
        j->ok = j->ok && computePipelineCreate(v, code, size, &j->denoised[3 + ARTF_DENOISE_PASSES]);
    }
}

void computeJobStart(Vulkan* v, const ArtfScene* scene, int kind, const ComputeKey* key, bool now) {
    ComputeJob* j = ARTF_NEW(ComputeJob);
    j->v = v;
    j->kind = kind;
    j->key = *key;
    j->graphs = graphsCopy(scene);
    j->denoise = kind == MAKE_TRACE && key->guides && !v->denoiseTime;
    v->computeJobs[kind] = j;
    if (now) computeJobRun(j);
    else j->task = artfTaskStart(computeJobRun, j);
}

/* As shaderJobEnd: taken if it is made for what the scene asks, with materials of its own it can draw. */
bool computeJobEnd(Vulkan* v, const ArtfScene* scene, int kind, bool use) {
    ComputeJob* j = v->computeJobs[kind];
    if (!j) return true;
    artfTaskJoin(j->task);
    v->computeJobs[kind] = NULL;
    const bool failed = use && !j->ok;
    bool taken = false;
    if (use && j->ok && scene) {
        const ComputeKey want = computeKeyOf(v, scene, kind);
        taken = computeFlagsSame(&j->key, &want) && graphsPrefix(j->key.graphCount, j->key.scene, scene);
    }
    if (taken) {
        VkPipeline* have = kind == MAKE_TRACE ? &v->tracePipeline : &v->reflectionTraced;
        pipelineRetire(v, *have);
        *have = j->made;
        v->computeKeys[kind] = j->key;
        if (kind == MAKE_TRACE) {
            if (j->key.hardware) artfLog("artful: rays go through the ray tracing hardware");
            v->traceDone = 0; /* (what was gathered with another shader starts over) */
            VkPipeline* const filter[DENOISE_PIPELINES] = {&v->denoiseLight, &v->denoiseTime, &v->denoiseVariance, &v->denoiseSpace[0], &v->denoiseSpace[1], &v->denoiseSpace[2], &v->denoiseSpace[3], &v->denoiseSpace[4], &v->denoiseEnd};
            for (int i = 0; i < DENOISE_PIPELINES && j->denoise; ++i) {
                pipelineRetire(v, *filter[i]);
                *filter[i] = j->denoised[i];
            }
        }
    } else {
        if (j->made) v->vkDestroyPipeline(v->device, j->made, NULL);
        for (int i = 0; i < DENOISE_PIPELINES; ++i)
            if (j->denoised[i]) v->vkDestroyPipeline(v->device, j->denoised[i], NULL);
    }
    graphsFree(j->graphs, j->key.graphCount);
    artfFree(j);
    return !failed;
}

/* What the scene asks of a compute shader of kind, against the one there: usable, it can draw the scene (its
 * materials of their own past those it was made for by their own values). One that is not what is asked is made: now
 * if wait, otherwise on a thread of its own. false: making it failed. */
bool computeFollow(Vulkan* v, const ArtfScene* scene, int kind, bool wait, bool* usable) {
    const ComputeKey want = computeKeyOf(v, scene, kind);
    VkPipeline* have = kind == MAKE_TRACE ? &v->tracePipeline : &v->reflectionTraced;
    const ComputeKey* had = &v->computeKeys[kind];
    *usable = *have && computeFlagsSame(had, &want) && (kind != MAKE_TRACE || !want.guides || v->denoiseTime) && graphsPrefix(had->graphCount, had->scene, scene);
    if (*usable && had->graphCount == want.graphCount) return true;
    if (!*usable && *have) {
        pipelineRetire(v, *have);
        *have = 0;
    }
    if (wait) {
        computeJobEnd(v, scene, kind, false);
        computeJobStart(v, scene, kind, &want, true);
        if (!computeJobEnd(v, scene, kind, true)) return false;
        *usable = *have != 0;
    } else if (!v->computeJobs[kind]) {
        computeJobStart(v, scene, kind, &want, false);
    }
    return true;
}


/* The layers in view, each surface once: in one call where a device can. */
static void layerDraws(Vulkan* v, VkCommandBuffer c, const Slot* s, uint32_t layerCount) {
    if (v->multiDraw) {
        v->vkCmdDrawIndexedIndirect(c, s->layerCommands.buffer, 0, layerCount, 20);
        return;
    }
    for (uint32_t i = 0; i < layerCount; ++i) v->vkCmdDrawIndexedIndirect(c, s->layerCommands.buffer, (VkDeviceSize)i * 20, 1, 20);
}

bool cloudsSync(Vulkan* v, const ArtfScene* scene, const ArtfFrameData* frame) {
    v->cloudsOn = false;
    if (!scene->clouds || artfOrtho(frame) || !v->postNone.buffer) return true;
    const uint32_t columns = ((uint32_t)v->sceneWidth + ARTF_CLOUD_TILE - 1) / ARTF_CLOUD_TILE, rows = ((uint32_t)v->sceneHeight + ARTF_CLOUD_TILE - 1) / ARTF_CLOUD_TILE;
    const bool fieldNew = !v->cloudField.buffer, gridNew = columns != v->cloudColumns || rows != v->cloudRows || !v->cloudGrid.buffer;
    if (fieldNew || gridNew || !v->cloudFieldHeld || v->cloudGeneration != scene->cloudsGeneration) {
        /* (rare: when the sky or the size changes; the frames in flight read what is there) */
        v->vkDeviceWaitIdle(v->device);
        if (fieldNew && !bufferNew(v, &v->cloudField, (VkDeviceSize)ARTF_CLOUD_FLOATS * 4, BUFFER_STORAGE, true)) return false;
        if (gridNew) {
            bufferFree(v, &v->cloudGrid);
            if (!bufferNew(v, &v->cloudGrid, (VkDeviceSize)columns * rows * ARTF_CLOUD_CELL * 4, BUFFER_STORAGE, false)) return false;
            v->cloudColumns = columns;
            v->cloudRows = rows;
        }
        memcpy(v->cloudField.mapped, scene->clouds, (size_t)ARTF_CLOUD_FLOATS * 4);
        v->cloudGeneration = scene->cloudsGeneration;
        v->cloudFieldHeld = true;
        for (int k = 0; k < SLOTS; ++k) {
            bindStorage(v, &v->slots[k], 73, &v->cloudField);
            bindStorage(v, &v->slots[k], 74, &v->cloudGrid);
        }
    }
    v->cloudsOn = true;
    return true;
}

/* The columns of the grid of layers by transmittance, for the scene's size. */
static uint32_t layerColumns(const Vulkan* v) {
    return (uint32_t)((v->sceneWidth + ARTF_LAYER_TILE - 1) / ARTF_LAYER_TILE) * (uint32_t)((v->sceneHeight + ARTF_LAYER_TILE - 1) / ARTF_LAYER_TILE);
}

/* Layers by each pixel's depth, by transmittance, when a scene first asks: the grid and each sample's sums, for this
 * size, and what fills and reads them. */
bool layerListsMake(Vulkan* v, const ArtfScene* scene) {
    const VkDeviceSize samples = (VkDeviceSize)v->sceneWidth * v->sceneHeight * (VkDeviceSize)v->samples;
    const VkDeviceSize gridBytes = (VkDeviceSize)layerColumns(v) * ARTF_LAYER_SLICES * 4, sumBytes = samples * ARTF_LAYER_SUMS * 4;
    if (v->layerCounts.size < gridBytes || v->layerFragments.size < sumBytes) {
        v->vkDeviceWaitIdle(v->device);
        bufferFree(v, &v->layerCounts);
        bufferFree(v, &v->layerFragments);
        if (!bufferNew(v, &v->layerCounts, gridBytes, BUFFER_STORAGE | BUFFER_TRANSFER_DST, false)) return false;
        if (!bufferNew(v, &v->layerFragments, sumBytes, BUFFER_STORAGE | BUFFER_TRANSFER_DST, false)) return false;
        v->layerSumsFresh = true; /* (to 0 once: then each resolve leaves what it read at 0) */
        for (int i = 0; i < SLOTS; ++i) {
            bindStorage(v, &v->slots[i], 44, &v->layerCounts);
            bindStorage(v, &v->slots[i], 45, &v->layerFragments);
        }
    }
    if (!v->layerPrefixPipeline) {
        size_t size;
        uint32_t* code = artfShaderLayerPrefix(&size);
        if (!computePipelineCreate(v, code, size, &v->layerPrefixPipeline)) return false;
    }
    if (v->layerListPipeline && v->layerGridPipeline && v->layerResolvePipeline) return true;
    VkPipeline made[3];
    const bool list = !v->layerListPipeline || !v->layerGridPipeline, resolve = !v->layerResolvePipeline;
    if (list) {
        pipelineRetire(v, v->layerListPipeline);
        pipelineRetire(v, v->layerGridPipeline);
    }
    const bool ok = layerListPipelinesCreate(v, scene->graphs.items, v->graphCount, shaderVariant(v), v->renderPass, v->resolvePass, list, resolve, made);
    if (list) {
        v->layerListPipeline = made[0];
        v->layerGridPipeline = made[2];
    }
    if (resolve) v->layerResolvePipeline = made[1];
    return ok;
}

bool pipelinesMake(Vulkan* v) {
    pipelineCacheOpen(v);
    if (!shadowPassMake(v)) return false;
    const int frameLayout = v->surface ? LAYOUT_PRESENT : LAYOUT_TRANSFER_SRC;
    /* (with floats, the scene's passes leave their images to be read, and presentPass makes the frame) */
    const int finalLayout = v->floats ? LAYOUT_SHADER_READ : frameLayout;
    if (v->floats) {
        const AttachmentDescription frameColor = {0, v->colorFormat, 1, 2 /* don't care */, 0 /* store */, 2, 1, LAYOUT_UNDEFINED, frameLayout};
        const AttachmentReference ref = {0, LAYOUT_COLOR};
        const SubpassDescription sub = {0, 0, 0, NULL, 1, &ref, NULL, NULL, 0, NULL};
        const SubpassDependency deps[1] = {{~0u, 0, STAGE_COMPUTE | STAGE_COLOR, STAGE_FRAGMENT | STAGE_COLOR, ACCESS_SHADER_WRITE | ACCESS_COLOR_WRITE, ACCESS_SHADER_READ | ACCESS_COLOR_WRITE, 0}};
        const RenderPassCreateInfo present = {ST_RENDER_PASS, NULL, 0, 1, &frameColor, 1, &sub, 1, deps};
        if (v->vkCreateRenderPass(v->device, &present, NULL, &v->presentPass) != VK_OK) return false;
    }
    VkRenderPass passes[5];
    if (!scenePassesCreate(v, v->surfaceWide, finalLayout, passes)) return false;
    v->renderPass = passes[0];
    v->earlyPass = passes[1];
    v->latePass = passes[2];
    v->midPass = passes[3];
    v->resolvePass = passes[4];

    /* the one descriptor set */
    const VkFlags graphics = SHADER_VERTEX | SHADER_FRAGMENT;
    const DescriptorSetLayoutBinding bindings[64] = {
        {0, DESCRIPTOR_UNIFORM, 1, graphics | SHADER_COMPUTE, NULL}, {1, DESCRIPTOR_STORAGE, 1, graphics | SHADER_COMPUTE, NULL},
        {2, DESCRIPTOR_STORAGE, 1, graphics | SHADER_COMPUTE, NULL}, {3, DESCRIPTOR_STORAGE, 1, graphics | SHADER_COMPUTE, NULL},
        {4, DESCRIPTOR_STORAGE, 1, graphics | SHADER_COMPUTE, NULL}, {5, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL},
        {6, DESCRIPTOR_COMBINED, v->maxTextures, SHADER_FRAGMENT | SHADER_COMPUTE, NULL}, {7, DESCRIPTOR_COMBINED, 1, SHADER_FRAGMENT, NULL},
        {8, DESCRIPTOR_COMBINED, 1, SHADER_COMPUTE | SHADER_FRAGMENT, NULL}, {9, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, {10, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL},
        {11, DESCRIPTOR_UNIFORM, 1, SHADER_FRAGMENT | SHADER_COMPUTE, NULL}, {12, DESCRIPTOR_STORAGE, 1, SHADER_FRAGMENT | SHADER_COMPUTE, NULL},
        {13, DESCRIPTOR_STORAGE, 1, SHADER_FRAGMENT | SHADER_COMPUTE, NULL}, {14, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, {15, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL},
        {16, DESCRIPTOR_STORAGE, 1, SHADER_VERTEX, NULL},
        /* (what rays read: the pixel shader too, for shadows from rays) */
        {17, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE | SHADER_FRAGMENT, NULL}, {18, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE | SHADER_FRAGMENT, NULL}, {19, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE | SHADER_FRAGMENT, NULL},
        {20, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE | SHADER_FRAGMENT, NULL}, {21, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE | SHADER_FRAGMENT, NULL}, {22, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE | SHADER_FRAGMENT, NULL},
        {23, DESCRIPTOR_UNIFORM, 1, SHADER_COMPUTE | SHADER_FRAGMENT, NULL},
        {24, DESCRIPTOR_UNIFORM, 1, graphics | SHADER_COMPUTE, NULL}, {25, DESCRIPTOR_COMBINED, 1, SHADER_FRAGMENT | SHADER_COMPUTE, NULL}, {26, DESCRIPTOR_STORAGE, 1, SHADER_VERTEX | SHADER_COMPUTE, NULL},
        {27, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, {28, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, {29, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, {30, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL},
        {31, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, {32, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, {33, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL},
        {34, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, {35, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, {36, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL},
        {37, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, {38, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, {39, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL},
        {40, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, {41, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, {42, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, {43, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL},
        {44, DESCRIPTOR_STORAGE, 1, SHADER_FRAGMENT | SHADER_COMPUTE, NULL}, {45, DESCRIPTOR_STORAGE, 1, SHADER_FRAGMENT, NULL},
        {46, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, {47, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL},
        {48, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, {49, DESCRIPTOR_STORAGE, 1, graphics | SHADER_COMPUTE, NULL},
        {60, DESCRIPTOR_STORAGE, 1, SHADER_FRAGMENT | SHADER_COMPUTE, NULL}, {61, DESCRIPTOR_STORAGE, 1, SHADER_FRAGMENT | SHADER_COMPUTE, NULL},
        {62, DESCRIPTOR_STORAGE, 1, SHADER_FRAGMENT | SHADER_COMPUTE, NULL}, {63, DESCRIPTOR_STORAGE, 1, SHADER_FRAGMENT | SHADER_COMPUTE, NULL},
        /* what comes after a frame in floats */
        {50, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE | SHADER_FRAGMENT, NULL}, {51, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, {52, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL},
        {53, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, {54, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, {55, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL},
        {56, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE | SHADER_FRAGMENT, NULL}, {57, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE | SHADER_FRAGMENT, NULL},
        {58, DESCRIPTOR_COMBINED, 1, SHADER_COMPUTE, NULL}, {59, DESCRIPTOR_COMBINED, 1, SHADER_COMPUTE, NULL}};
    /* (and with the ray tracing hardware, the top level) */
    /* (64 bindings, 54 without floats; then what moves (68, 69) with them, the top level (64), the meshlets view's table
     * (65) and emitters of many (66, 67)) */
    DescriptorSetLayoutBinding all[96]; /* (room past what is listed: one more binding must not run past it) */
    uint32_t bindingCount = v->floats ? 64 : 54;
    memcpy(all, bindings, bindingCount * sizeof *all);
    if (v->floats) {
        /* (what the vertices were a frame ago; motion blur's velocities) */
        const DescriptorSetLayoutBinding past = {68, DESCRIPTOR_STORAGE, 1, SHADER_VERTEX, NULL}, blur = {69, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL};
        const DescriptorSetLayoutBinding locks = {71, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}; /* (upscaling's locks) */
        const DescriptorSetLayoutBinding gathered = {72, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}; /* (lights sampled, gathered) */
        const DescriptorSetLayoutBinding cloudField = {73, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, cloudGrid = {74, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}; /* (clouds in the scene) */
        all[bindingCount++] = past;
        all[bindingCount++] = blur;
        all[bindingCount++] = locks;
        all[bindingCount++] = gathered;
        all[bindingCount++] = cloudField;
        all[bindingCount++] = cloudGrid;
    }
    if (v->rayQuery) {
        const DescriptorSetLayoutBinding top = {64, DESCRIPTOR_AS, 1, SHADER_COMPUTE | SHADER_FRAGMENT, NULL};
        all[bindingCount++] = top;
    }
    /* (the meshlets view's table: written when it is first asked for) */
    const DescriptorSetLayoutBinding meshletIds = {65, DESCRIPTOR_STORAGE, 1, SHADER_FRAGMENT, NULL};
    all[bindingCount++] = meshletIds;
    /* (emitters of many: their particles, and this frame's rows for them) */
    const DescriptorSetLayoutBinding swarmState = {66, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, swarmRows = {67, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL};
    all[bindingCount++] = swarmState;
    all[bindingCount++] = swarmRows;
    /* (what a step of the probes makes: artfShaderProbes) */
    const DescriptorSetLayoutBinding probesNext = {70, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL};
    all[bindingCount++] = probesNext;
    if (bindingCount > sizeof all / sizeof all[0]) return false; /* (as a check: all must hold them) */
    const DescriptorSetLayoutCreateInfo layout = {ST_DESCRIPTOR_SET_LAYOUT, NULL, 0, bindingCount, all};
    if (v->vkCreateDescriptorSetLayout(v->device, &layout, NULL, &v->setLayout) != VK_OK) return false;
    /* (set 1: light, the second light and the history, by the part each plays: see roleSets) */
    const DescriptorSetLayoutBinding roles[3] = {{0, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE | SHADER_FRAGMENT, NULL}, {1, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, {2, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}};
    const DescriptorSetLayoutCreateInfo roleLayout = {ST_DESCRIPTOR_SET_LAYOUT, NULL, 0, 3, roles};
    if (v->vkCreateDescriptorSetLayout(v->device, &roleLayout, NULL, &v->roleLayout) != VK_OK) return false;
    const VkDescriptorSetLayout setLayouts[2] = {v->setLayout, v->roleLayout};
    const PushConstantRange push[3] = {{SHADER_COMPUTE, 0, 4}, {SHADER_VERTEX, 4, 4}, {SHADER_FRAGMENT, 8, 4}}; /* (8: preRotate's turn) */
    const PipelineLayoutCreateInfo pipelineLayout = {ST_PIPELINE_LAYOUT, NULL, 0, 2, setLayouts, 3, push};
    if (v->vkCreatePipelineLayout(v->device, &pipelineLayout, NULL, &v->pipelineLayout) != VK_OK) return false;

    /* (54 storage bindings of the 64, then 65 to 70) */
    const DescriptorPoolSize sizes[4] = {{DESCRIPTOR_UNIFORM, SLOTS * 4}, {DESCRIPTOR_STORAGE, SLOTS * 80}, {DESCRIPTOR_COMBINED, SLOTS * (v->maxTextures + 5)}, {DESCRIPTOR_AS, SLOTS}};
    const DescriptorPoolCreateInfo pool = {ST_DESCRIPTOR_POOL, NULL, 0, SLOTS, v->rayQuery ? 4u : 3u, sizes};
    if (v->vkCreateDescriptorPool(v->device, &pool, NULL, &v->descriptorPool) != VK_OK) return false;
    for (int i = 0; i < SLOTS; ++i) {
        const DescriptorSetAllocateInfo alloc = {ST_DESCRIPTOR_SET_ALLOCATE, NULL, v->descriptorPool, 1, &v->setLayout};
        if (v->vkAllocateDescriptorSets(v->device, &alloc, &v->slots[i].set) != VK_OK) return false;
    }
    if (v->floats) {
        const DescriptorPoolSize roleSizes[1] = {{DESCRIPTOR_STORAGE, 27 * 3}};
        const DescriptorPoolCreateInfo rolePool = {ST_DESCRIPTOR_POOL, NULL, 0, 27, 1, roleSizes};
        if (v->vkCreateDescriptorPool(v->device, &rolePool, NULL, &v->rolePool) != VK_OK) return false;
        VkDescriptorSetLayout layouts[27];
        for (int i = 0; i < 27; ++i) layouts[i] = v->roleLayout;
        const DescriptorSetAllocateInfo alloc = {ST_DESCRIPTOR_SET_ALLOCATE, NULL, v->rolePool, 27, layouts};
        if (v->vkAllocateDescriptorSets(v->device, &alloc, v->roleSets) != VK_OK) return false;
    }
    const SamplerCreateInfo sampler = {ST_SAMPLER, NULL, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1.f, 0, 0, 0, 1000.f, 0, 0};
    if (v->vkCreateSampler(v->device, &sampler, NULL, &v->sampler) != VK_OK) return false;
    const SamplerCreateInfo nearest = {ST_SAMPLER, NULL, 0, 0, 0, 0, 2, 2, 2, 0, 0, 1.f, 0, 0, 0, 0.f, 0, 0};
    if (v->vkCreateSampler(v->device, &nearest, NULL, &v->depthSampler) != VK_OK) return false;

    /* the shaders, built here */
    size_t size;
    enum { COMPUTE_SHADERS = 8 };
    VkShaderModule compute[COMPUTE_SHADERS];
    for (int i = 0; i < 3; ++i) {
        uint32_t* cullCode = artfShaderCull(&size, (ArtfCullPhase)i);
        compute[i] = shaderModule(v, cullCode, size);
    }
    uint32_t* code = artfShaderHiz(&size, true, v->samples);
    compute[3] = shaderModule(v, code, size);
    code = artfShaderHiz(&size, false, 1);
    compute[4] = shaderModule(v, code, size);
    code = artfShaderBloomDown(&size, true);
    compute[5] = shaderModule(v, code, size);
    code = artfShaderBloomDown(&size, false);
    compute[6] = shaderModule(v, code, size);
    code = artfShaderBloomUp(&size);
    compute[7] = shaderModule(v, code, size);
    code = artfShaderSceneVertex(&size, v->floats);
    VkShaderModule sceneVertex = shaderModule(v, code, size);
    code = artfShaderSceneFragment(&size, v->maxTextures, false, NULL, 0, 0, shaderVariant(v));
    VkShaderModule sceneFragment = shaderModule(v, code, size);
    code = artfShaderOverlayVertex(&size);
    VkShaderModule overlayVertex = shaderModule(v, code, size);
    const ArtfShaderDisplay display = {v->display, v->displayInverse, v->displayPeak};
    code = artfShaderOverlayFragmentFor(&size, v->preRotate && v->floats, &display);
    VkShaderModule overlayFragment = shaderModule(v, code, size);
    code = artfShaderBackgroundBackdrop(&size, v->floats);
    VkShaderModule backdropFragment = shaderModule(v, code, size);
    code = artfShaderTraceShow(&size, v->floats);
    VkShaderModule traceFragment = shaderModule(v, code, size);
    code = artfShaderEnvironmentBackdrop(&size, v->floats);
    VkShaderModule environmentFragment = shaderModule(v, code, size);
    code = artfShaderShadowVertex(&size);
    VkShaderModule shadowVertex = shaderModule(v, code, size);
    code = artfShaderShadowFragment(&size, v->maxTextures);
    VkShaderModule shadowFragment = shaderModule(v, code, size);
    for (int i = 0; i < COMPUTE_SHADERS; ++i)
        if (!compute[i]) return false;
    v->sceneVertexModule = sceneVertex;
    if (!sceneVertex || !sceneFragment || !overlayVertex || !overlayFragment || !backdropFragment || !traceFragment || !shadowVertex || !shadowFragment || !environmentFragment) return false;

    ComputePipelineCreateInfo computeInfo[COMPUTE_SHADERS];
    for (int i = 0; i < COMPUTE_SHADERS; ++i) {
        const ComputePipelineCreateInfo c = {ST_COMPUTE_PIPELINE, NULL, 0, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_COMPUTE, compute[i], "main", NULL}, v->pipelineLayout, 0, -1};
        computeInfo[i] = c;
    }
    VkPipeline computeMade[COMPUTE_SHADERS];
    memset(computeMade, 0, sizeof computeMade);
    bool ok = v->vkCreateComputePipelines(v->device, v->pipelineCache, COMPUTE_SHADERS, computeInfo, NULL, computeMade) == VK_OK;
    memcpy(v->cullPipelines, computeMade, sizeof v->cullPipelines);
    v->hizFirstPipeline = computeMade[3];
    v->hizNextPipeline = computeMade[4];
    v->bloomFirstPipeline = computeMade[5];
    v->bloomNextPipeline = computeMade[6];
    v->bloomUpPipeline = computeMade[7];

    const VertexBinding binding = {0, sizeof(ArtfPackedVertex), 0};
    const VertexAttribute attributes[3] = {{0, 0, FORMAT_RGBA16_UNORM, 0}, {1, 0, FORMAT_RG16_SNORM, 8}, {2, 0, FORMAT_RG16_FLOAT, 12}};
    const PipelineVertexInput vertexInput = {ST_PIPELINE_VERTEX_INPUT, NULL, 0, 1, &binding, 3, attributes};
    const PipelineVertexInput noInput = {ST_PIPELINE_VERTEX_INPUT, NULL, 0, 0, NULL, 0, NULL};
    const PipelineInputAssembly assembly = {ST_PIPELINE_INPUT_ASSEMBLY, NULL, 0, 3 /* triangle list */, 0};
    const PipelineViewport viewport = {ST_PIPELINE_VIEWPORT, NULL, 0, 1, NULL, 1, NULL};
    const PipelineRasterization rasterBack = {ST_PIPELINE_RASTERIZATION, NULL, 0, 0, 0, 0, 2 /* back */, 0 /* counter-clockwise */, 0, 0, 0, 0, 1.f};
    const PipelineRasterization rasterNone = {ST_PIPELINE_RASTERIZATION, NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1.f};
    const PipelineMultisample multisample = {ST_PIPELINE_MULTISAMPLE, NULL, 0, v->samples, 0, 0, NULL, 0, 0};
    PipelineDepthStencil depthOn;
    memset(&depthOn, 0, sizeof depthOn);
    depthOn.sType = ST_PIPELINE_DEPTH_STENCIL;
    depthOn.depthTest = 1;
    depthOn.depthWrite = 1;
    depthOn.depthCompare = 4; /* greater: depth is reversed */
    PipelineDepthStencil depthOff;
    memset(&depthOff, 0, sizeof depthOff);
    depthOff.sType = ST_PIPELINE_DEPTH_STENCIL;
    const ColorBlendAttachment opaque = {0, 0, 0, 0, 0, 0, 0, 15};
    const ColorBlendAttachment premultiplied = {1, 1 /* one */, 7 /* one minus source alpha */, 0, 1, 7, 0, 15};
    ColorBlendAttachment pairOff[2], pairOn[2], pairBehind[2];
    const PipelineColorBlend blendOff = blendOf(v, pairOff, opaque, true);
    const PipelineColorBlend blendOn = blendOf(v, pairOn, premultiplied, false);
    const PipelineColorBlend blendBehind = blendOf(v, pairBehind, opaque, false); /* (what is behind the scene has no surface) */
    /* the overlay goes over the frame: with floats, in presentPass, after what comes after */
    const PipelineColorBlend overlayBlend = {ST_PIPELINE_COLOR_BLEND, NULL, 0, 0, 0, 1, &premultiplied, {0, 0, 0, 0}};
    const int states[2] = {0, 1}; /* viewport, scissor: a new size needs no new pipeline */
    const PipelineDynamic dynamic = {ST_PIPELINE_DYNAMIC, NULL, 0, 2, states};
    const PipelineShaderStage sceneStages[2] = {{ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_VERTEX, sceneVertex, "main", NULL}, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_FRAGMENT, sceneFragment, "main", NULL}};
    const PipelineShaderStage overlayStages[2] = {{ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_VERTEX, overlayVertex, "main", NULL}, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_FRAGMENT, overlayFragment, "main", NULL}};
    const PipelineShaderStage backdropStages[2] = {{ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_VERTEX, overlayVertex, "main", NULL}, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_FRAGMENT, backdropFragment, "main", NULL}};
    const PipelineShaderStage traceStages[2] = {overlayStages[0], {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_FRAGMENT, traceFragment, "main", NULL}};
    const PipelineShaderStage environmentStages[2] = {overlayStages[0], {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_FRAGMENT, environmentFragment, "main", NULL}};
    const PipelineShaderStage shadowStages[2] = {{ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_VERTEX, shadowVertex, "main", NULL}, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_FRAGMENT, shadowFragment, "main", NULL}};
    const PipelineMultisample oneSample = {ST_PIPELINE_MULTISAMPLE, NULL, 0, 1, 0, 0, NULL, 0, 0};
    PipelineDepthStencil depthNear = depthOn;
    depthNear.depthCompare = 1; /* less: these maps are not reversed */
    const PipelineColorBlend noBlend = {ST_PIPELINE_COLOR_BLEND, NULL, 0, 0, 0, 0, NULL, {0, 0, 0, 0}};
    const GraphicsPipelineCreateInfo graphicsInfo[7] = {
        {ST_GRAPHICS_PIPELINE, NULL, 0, 2, sceneStages, &vertexInput, &assembly, NULL, &viewport, &rasterBack, &multisample, &depthOn, &blendOff, &dynamic, v->pipelineLayout, v->renderPass, 0, 0, -1},
        {ST_GRAPHICS_PIPELINE, NULL, 0, 2, overlayStages, &noInput, &assembly, NULL, &viewport, &rasterNone, v->floats ? &oneSample : &multisample, &depthOff, v->floats ? &overlayBlend : &blendOn, &dynamic, v->pipelineLayout, v->floats ? v->presentPass : v->renderPass, 0, 0, -1},
        {ST_GRAPHICS_PIPELINE, NULL, 0, 2, backdropStages, &noInput, &assembly, NULL, &viewport, &rasterNone, &multisample, &depthOff, &blendBehind, &dynamic, v->pipelineLayout, v->renderPass, 0, 0, -1},
        {ST_GRAPHICS_PIPELINE, NULL, 0, 2, traceStages, &noInput, &assembly, NULL, &viewport, &rasterNone, &multisample, &depthOff, &blendOn, &dynamic, v->pipelineLayout, v->renderPass, 0, 0, -1},
        /* what casts shadows: both sides, as rays have it; with no pixel shader where it can do without */
        {ST_GRAPHICS_PIPELINE, NULL, 0, 1, shadowStages, &vertexInput, &assembly, NULL, &viewport, &rasterNone, &oneSample, &depthNear, &noBlend, &dynamic, v->pipelineLayout, v->shadowPass, 0, 0, -1},
        {ST_GRAPHICS_PIPELINE, NULL, 0, 2, shadowStages, &vertexInput, &assembly, NULL, &viewport, &rasterNone, &oneSample, &depthNear, &noBlend, &dynamic, v->pipelineLayout, v->shadowPass, 0, 0, -1},
        {ST_GRAPHICS_PIPELINE, NULL, 0, 2, environmentStages, &noInput, &assembly, NULL, &viewport, &rasterNone, &multisample, &depthOff, &blendBehind, &dynamic, v->pipelineLayout, v->renderPass, 0, 0, -1}};
    VkPipeline made[7] = {0, 0, 0, 0, 0, 0, 0};
    ok = ok && v->vkCreateGraphicsPipelines(v->device, v->pipelineCache, 7, graphicsInfo, NULL, made) == VK_OK;
    v->scenePipeline = made[0];
    v->overlayPipeline = made[1];
    v->backdropPipeline = made[2];
    v->traceShowPipeline = made[3];
    v->shadowPipeline = made[4];
    v->shadowHolesPipeline = made[5];
    v->environmentPipeline = made[6];
    if (ok && v->floats) {
        /* what comes after: postShaders.inc */
        uint32_t* (*const makers[POST_KINDS])(size_t*) = {NULL, artfShaderPostOcclusion, artfShaderPostOcclusionBlur, artfShaderPostReflection, artfShaderPostFroxels,
                                                          artfShaderPostTemporal, artfShaderPostFocus, artfShaderPostBloomDown, artfShaderPostBloomUp, artfShaderPostReflectionApply,
                                                          artfShaderPostMeter, artfShaderPostMotionBlur, artfShaderPostVelocity, artfShaderPostTileMost, artfShaderPostUpscale,
                                                          artfShaderPostSharpen, artfShaderPostSampled, artfShaderPostSampledSpace, artfShaderPostCloudColumns, artfShaderPostCloudApply};
        for (int i = 0; i < POST_KINDS && ok; ++i) {
            uint32_t* postCode = i == 0 ? artfShaderPostGather(&size, v->samples) : makers[i](&size);
            postAccessOf(postCode, size, &v->postReads[i], &v->postWrites[i]);
            VkShaderModule module = shaderModule(v, postCode, size);
            const ComputePipelineCreateInfo c = {ST_COMPUTE_PIPELINE, NULL, 0, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_COMPUTE, module, "main", NULL}, v->pipelineLayout, 0, -1};
            ok = module && v->vkCreateComputePipelines(v->device, v->pipelineCache, 1, &c, NULL, &v->postPipelines[i]) == VK_OK;
            if (module) v->vkDestroyShaderModule(v->device, module, NULL);
        }
        uint32_t* finalCode = artfShaderPostFinalFor(&size, v->preRotate, &display);
        VkShaderModule finalModule = shaderModule(v, finalCode, size);
        const PipelineShaderStage presentStages[2] = {overlayStages[0], {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_FRAGMENT, finalModule, "main", NULL}};
        const ColorBlendAttachment whole = {0, 0, 0, 0, 0, 0, 0, 15};
        const PipelineColorBlend replace = {ST_PIPELINE_COLOR_BLEND, NULL, 0, 0, 0, 1, &whole, {0, 0, 0, 0}};
        const GraphicsPipelineCreateInfo present = {ST_GRAPHICS_PIPELINE, NULL, 0, 2, presentStages, &noInput, &assembly, NULL, &viewport, &rasterNone, &oneSample, &depthOff, &replace, &dynamic, v->pipelineLayout, v->presentPass, 0, 0, -1};
        ok = ok && finalModule && v->vkCreateGraphicsPipelines(v->device, v->pipelineCache, 1, &present, NULL, &v->presentPipeline) == VK_OK;
        if (finalModule) v->vkDestroyShaderModule(v->device, finalModule, NULL);
        /* fog over what is behind: light * left + added * what shows of it (destination alpha), alpha kept */
        uint32_t* fogCode = artfShaderFogBackdrop(&size);
        VkShaderModule fogModule = shaderModule(v, fogCode, size);
        const PipelineShaderStage fogStages[2] = {overlayStages[0], {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_FRAGMENT, fogModule, "main", NULL}};
        const ColorBlendAttachment overBehind = {1, 8 /* destination alpha */, 6 /* source alpha */, 0, 0 /* zero */, 1 /* one */, 0, 15};
        ColorBlendAttachment pairFog[2];
        const PipelineColorBlend fogBlend = blendOf(v, pairFog, overBehind, false);
        const GraphicsPipelineCreateInfo fog = {ST_GRAPHICS_PIPELINE, NULL, 0, 2, fogStages, &noInput, &assembly, NULL, &viewport, &rasterNone, &multisample, &depthOff, &fogBlend, &dynamic, v->pipelineLayout, v->renderPass, 0, 0, -1};
        ok = ok && fogModule && v->vkCreateGraphicsPipelines(v->device, v->pipelineCache, 1, &fog, NULL, &v->fogPipeline) == VK_OK;
        if (fogModule) v->vkDestroyShaderModule(v->device, fogModule, NULL);
    }
    if (ok) {
        uint32_t* cutoutCode = sceneFragmentCode(v, true, NULL, 0, shaderVariant(v), &size);
        VkShaderModule cutoutModule = shaderModule(v, cutoutCode, size);
        VkPipeline cutouts[4] = {0, 0, 0, 0};
        ok = cutoutModule && cutoutPipelinesCreate(v, cutoutModule, v->renderPass, cutouts);
        v->layerDepthPipeline = cutouts[0];
        v->layerPipeline = cutouts[1];
        v->cutoutPipeline = cutouts[2];
        v->particlePipeline = cutouts[3];
        if (cutoutModule) v->vkDestroyShaderModule(v->device, cutoutModule, NULL);
    }

    for (int i = 0; i < COMPUTE_SHADERS; ++i) v->vkDestroyShaderModule(v->device, compute[i], NULL);
    v->vkDestroyShaderModule(v->device, sceneFragment, NULL);
    v->vkDestroyShaderModule(v->device, overlayVertex, NULL);
    v->vkDestroyShaderModule(v->device, overlayFragment, NULL);
    v->vkDestroyShaderModule(v->device, backdropFragment, NULL);
    v->vkDestroyShaderModule(v->device, traceFragment, NULL);
    v->vkDestroyShaderModule(v->device, environmentFragment, NULL);
    v->vkDestroyShaderModule(v->device, shadowVertex, NULL);
    v->vkDestroyShaderModule(v->device, shadowFragment, NULL);
    return ok;
}

/* ---- the scene, on the GPU ---- */

void writeBufferDescriptors(Vulkan* v, Slot* s) {
    const DescriptorBufferInfo infos[6] = {{s->frame.buffer, 0, ~(VkDeviceSize)0}, {s->instances.buffer, 0, ~(VkDeviceSize)0}, {s->visible.buffer, 0, ~(VkDeviceSize)0},
                                           {v->meshTable.buffer, 0, ~(VkDeviceSize)0}, {s->materials.buffer, 0, ~(VkDeviceSize)0}, {s->indirect.buffer, 0, ~(VkDeviceSize)0}};
    WriteDescriptorSet writes[6];
    for (uint32_t i = 0; i < 6; ++i) {
        WriteDescriptorSet w = {ST_WRITE_DESCRIPTOR_SET, NULL, s->set, i, 0, 1, i == 0 ? DESCRIPTOR_UNIFORM : DESCRIPTOR_STORAGE, NULL, &infos[i], NULL};
        writes[i] = w;
    }
    v->vkUpdateDescriptorSets(v->device, 6, writes, 0, NULL);
    const DescriptorBufferInfo flags = {v->flags.buffer, 0, ~(VkDeviceSize)0};
    const WriteDescriptorSet flagWrite = {ST_WRITE_DESCRIPTOR_SET, NULL, s->set, 10, 0, 1, DESCRIPTOR_STORAGE, NULL, &flags, NULL};
    v->vkUpdateDescriptorSets(v->device, 1, &flagWrite, 0, NULL);
    const DescriptorBufferInfo background = {s->background.buffer, 0, ~(VkDeviceSize)0};
    const WriteDescriptorSet backgroundWrite = {ST_WRITE_DESCRIPTOR_SET, NULL, s->set, 11, 0, 1, DESCRIPTOR_UNIFORM, NULL, &background, NULL};
    v->vkUpdateDescriptorSets(v->device, 1, &backgroundWrite, 0, NULL);
    const DescriptorBufferInfo stream = {s->stream.buffer, 0, ~(VkDeviceSize)0};
    const WriteDescriptorSet streamWrite = {ST_WRITE_DESCRIPTOR_SET, NULL, s->set, 15, 0, 1, DESCRIPTOR_STORAGE, NULL, &stream, NULL};
    v->vkUpdateDescriptorSets(v->device, 1, &streamWrite, 0, NULL);
    const DescriptorBufferInfo colors = {v->colors.buffer, 0, ~(VkDeviceSize)0};
    const WriteDescriptorSet colorWrite = {ST_WRITE_DESCRIPTOR_SET, NULL, s->set, 16, 0, 1, DESCRIPTOR_STORAGE, NULL, &colors, NULL};
    v->vkUpdateDescriptorSets(v->device, 1, &colorWrite, 0, NULL);
    const DescriptorBufferInfo trace = {s->trace.buffer, 0, ~(VkDeviceSize)0};
    const WriteDescriptorSet traceWrite = {ST_WRITE_DESCRIPTOR_SET, NULL, s->set, 23, 0, 1, DESCRIPTOR_UNIFORM, NULL, &trace, NULL};
    v->vkUpdateDescriptorSets(v->device, 1, &traceWrite, 0, NULL);
    const DescriptorBufferInfo shadow = {s->shadow.buffer, 0, ~(VkDeviceSize)0};
    const WriteDescriptorSet shadowWrite = {ST_WRITE_DESCRIPTOR_SET, NULL, s->set, 24, 0, 1, DESCRIPTOR_UNIFORM, NULL, &shadow, NULL};
    v->vkUpdateDescriptorSets(v->device, 1, &shadowWrite, 0, NULL);
}

/* Room for each instance's flag, none set. */
bool flagsMake(Vulkan* v) {
    bufferFree(v, &v->flags);
    if (!bufferNew(v, &v->flags, (VkDeviceSize)v->instanceCapacity * 4, BUFFER_STORAGE | BUFFER_TRANSFER_DST, false)) return false;
    VkCommandBuffer c = oneShotBegin(v);
    v->vkCmdFillBuffer(c, v->flags.buffer, 0, ~(VkDeviceSize)0, 0);
    return oneShotEnd(v, c);
}

/* Copies data to a buffer of the GPU's. */
bool upload(Vulkan* v, GpuBuffer* to, VkDeviceSize offset, const void* data, VkDeviceSize size) {
    if (!size) return true;
    if (to->mapped) { memcpy((char*)to->mapped + offset, data, size); return true; }
    GpuBuffer staging;
    if (!bufferNew(v, &staging, size, BUFFER_TRANSFER_SRC, true)) return false;
    memcpy(staging.mapped, data, size);
    VkCommandBuffer c = oneShotBegin(v);
    const BufferCopy region = {0, offset, size};
    v->vkCmdCopyBuffer(c, staging.buffer, to->buffer, 1, &region);
    bool ok = oneShotEnd(v, c);
    bufferFree(v, &staging);
    return ok;
}

bool takesFormat(const Vulkan* v, uint32_t format) { return format >= 157 ? v->takesAstc : format >= 147 ? v->takesEtc : v->takesBc; }

long long artfVulkanTextureMemoryLimit = -1;
long long artfVulkanMemoryBudget = -1;

/* What a texture that cannot be made at all shows: white, which every entry of the array must have anyway. */
void textureWhite(Vulkan* v, uint32_t index) {
    const DescriptorImageInfo white = {v->sampler, v->textures.items[0].view, LAYOUT_SHADER_READ};
    for (int k = 0; k < SLOTS; ++k) {
        const WriteDescriptorSet w = {ST_WRITE_DESCRIPTOR_SET, NULL, v->slots[k].set, 6, index, 1, DESCRIPTOR_COMBINED, &white, NULL, NULL};
        v->vkUpdateDescriptorSets(v->device, 1, &w, 0, NULL);
    }
}

/* (a texture's mark: what went up was less than was asked) */
#define TEXTURE_SHORT ((uint64_t)1 << 63)
#define TEXTURE_NONE 255

/* Blocks go up as they are, where the device takes them; plain pixels are made into blocks if that was asked. When the
 * GPU's memory has run out, it goes up without its largest mipmaps, as many as it takes; when not even the smallest
 * fits, it is white, and said so (ARTF_EVENT_REFUSED). Only the white texture itself not fitting fails. */
bool uploadTexture(Vulkan* v, ArtfTextureData* t, uint32_t index, bool compress, ArtfTexture handle) {
    GpuImage image;
    /* (what its image holds now: it is not let go for less) */
    const int held = index < v->textureHeld.len ? v->textureHeld.items[index] : TEXTURE_NONE;
    uint32_t format = t->format, offsets[16];
    const uint8_t* source = t->blocks;
    uint8_t* encoded = NULL;
    size_t bytes = t->blockBytes;
    if (format && !takesFormat(v, format)) {
        artfTextureDecode(t);
        format = 0;
    } else if (format) {
        memcpy(offsets, t->blockOffsets, sizeof offsets);
    } else if (compress && !t->read && (v->takesBc || v->takesEtc)) {
        encoded = artfTextureEncode(t, !v->takesBc, &format, &bytes, offsets);
        source = encoded;
    }
    if (!format) {
        source = (const uint8_t*)t->pixels;
        bytes = 0;
        for (int level = 0, w = t->width, h = t->height; level < t->mipCount; ++level) {
            offsets[level] = t->offsets[level] * 4;
            if (!t->read || level >= t->residentMip) bytes += (size_t)w * h * 4;
            w = artfMaxi(w / 2, 1);
            h = artfMaxi(h / 2, 1);
        }
    }
    /* what is resident: the mipmaps from that one down, as an image of their own; fewer, if that does not fit */
    const int resident = artfMini(t->residentMip, t->mipCount - 1);
    int first = resident, width = 0, height = 0;
    for (;; ++first) {
        width = t->width;
        height = t->height;
        for (int level = 0; level < first; ++level) {
            width = artfMaxi(width / 2, 1);
            height = artfMaxi(height / 2, 1);
        }
        if (imageNew(v, &image, format ? (int)format : FORMAT_BGRA8, width, height, (uint32_t)(t->mipCount - first), 1, IMAGE_SAMPLED | IMAGE_TRANSFER_DST, ASPECT_COLOR)) break;
        imageFree(v, &image);
        const bool holds = index && index < v->textures.len && v->textures.items[index].image && held != TEXTURE_NONE;
        if (holds && held < first) {
            /* what it holds is larger than this: it goes, the device being idle, and this is tried again in its room */
            textureWhite(v, index);
            imageFree(v, &v->textures.items[index]);
            v->textureHeld.items[index] = TEXTURE_NONE;
            --first;
            continue;
        }
        if (holds && held == first) {
            /* as large as what it holds: that stays, short of what was asked, and is tried again later */
            artfFree(encoded);
            v->textureMarks.items[index] = (uint64_t)t->generation << 8 | (uint64_t)resident | TEXTURE_SHORT;
            v->texturesShort = true;
            return true;
        }
        if (first + 1 < t->mipCount) continue;
        artfFree(encoded);
        if (!index) return false; /* (white itself) */
        if (index < v->textures.len) {
            imageFree(v, &v->textures.items[index]);
            v->textures.items[index] = image;
        } else {
            artfVecPush(&v->textures, image);
            artfVecPush(&v->textureMarks, 0);
            artfVecPush(&v->textureHeld, TEXTURE_NONE);
        }
        v->textureHeld.items[index] = TEXTURE_NONE;
        /* (marked short: tried again when textures change, as residency makes them smaller) */
        v->textureMarks.items[index] = (uint64_t)t->generation << 8 | (uint64_t)resident | TEXTURE_SHORT;
        v->texturesShort = true;
        textureWhite(v, index);
        artfEventSay(ARTF_EVENT_REFUSED, v, handle, 0, "artful: texture %u does not fit in the GPU's memory, even at its smallest: it is white", handle);
        return true;
    }
    if (first > resident) {
        artfLog("artful: texture %u is taken from mipmap %d, not %d, as the GPU's memory has run out", handle, first, resident);
        v->texturesShort = true;
    }
    const uint32_t levels = (uint32_t)(t->mipCount - first);
    /* (what goes up: the levels from first, from where they start) */
    const size_t skip = offsets[first] <= bytes ? offsets[first] : 0;
    BufferImageCopy regions[16];
    for (uint32_t level = 0, w = (uint32_t)width, h = (uint32_t)height; level < levels; ++level) {
        BufferImageCopy r = {(VkDeviceSize)(offsets[(uint32_t)first + level] - skip), 0, 0, {ASPECT_COLOR, level, 0, 1}, {0, 0, 0}, {w, h, 1}};
        regions[level] = r;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    GpuBuffer staging;
    if (!bufferNew(v, &staging, bytes - skip, BUFFER_TRANSFER_SRC, true)) {
        /* (no room to bring it up: what it held stays, short of what was asked, tried again later) */
        imageFree(v, &image);
        artfFree(encoded);
        if (!index) return false; /* (white itself) */
        if (index >= v->textures.len) {
            /* (new: white, until it fits) */
            GpuImage none;
            memset(&none, 0, sizeof none);
            artfVecPush(&v->textures, none);
            artfVecPush(&v->textureMarks, 0);
            artfVecPush(&v->textureHeld, TEXTURE_NONE);
            textureWhite(v, index);
        }
        v->textureMarks.items[index] = (uint64_t)t->generation << 8 | (uint64_t)resident | TEXTURE_SHORT;
        v->texturesShort = true;
        return true;
    }
    memcpy(staging.mapped, source + skip, bytes - skip);
    artfFree(encoded);
    VkCommandBuffer c = oneShotBegin(v);
    imageBarrier(v, c, image.image, levels, LAYOUT_UNDEFINED, LAYOUT_TRANSFER_DST, STAGE_TOP, STAGE_TRANSFER, 0, ACCESS_TRANSFER_WRITE);
    v->vkCmdCopyBufferToImage(c, staging.buffer, image.image, LAYOUT_TRANSFER_DST, levels, regions);
    imageBarrier(v, c, image.image, levels, LAYOUT_TRANSFER_DST, LAYOUT_SHADER_READ, STAGE_TRANSFER, STAGE_FRAGMENT, ACCESS_TRANSFER_WRITE, ACCESS_SHADER_READ);
    bool ok = oneShotEnd(v, c);
    bufferFree(v, &staging);
    /* (marked as what was asked, so that it is not tried again each frame; short of it, tried again when textures change) */
    const uint64_t mark = (uint64_t)t->generation << 8 | (uint64_t)resident | (first > resident ? TEXTURE_SHORT : 0);
    if (index < v->textures.len) {
        imageFree(v, &v->textures.items[index]);
        v->textures.items[index] = image;
        v->textureMarks.items[index] = mark;
        v->textureHeld.items[index] = (uint8_t)first;
    } else {
        artfVecPush(&v->textures, image);
        artfVecPush(&v->textureMarks, mark);
        artfVecPush(&v->textureHeld, (uint8_t)first);
    }
    /* The first, white, goes everywhere: every entry of the array is then a real one, and the device
     * need not offer partly bound arrays. */
    DescriptorImageInfo* infos = artfMalloc((size_t)v->maxTextures * sizeof *infos);
    const uint32_t count = index ? 1 : v->maxTextures;
    for (uint32_t i = 0; i < count; ++i) {
        const DescriptorImageInfo info = {v->sampler, image.view, LAYOUT_SHADER_READ};
        infos[i] = info;
    }
    for (int i = 0; i < SLOTS; ++i) {
        WriteDescriptorSet w = {ST_WRITE_DESCRIPTOR_SET, NULL, v->slots[i].set, 6, index, count, DESCRIPTOR_COMBINED, infos, NULL, NULL};
        v->vkUpdateDescriptorSets(v->device, 1, &w, 0, NULL);
    }
    artfFree(infos);
    return ok;
}

/* Colors start with a white one, which is what the vertices of a mesh without colors read. */
bool colorsReset(Vulkan* v) {
    const uint32_t white = 0xFFFFFFFFu;
    v->colorCount = 1;
    return upload(v, &v->colors, 0, &white, 4);
}

/* The list of copies: two words each, for four parts (early and late, plain and with holes or backs), with
 * room for every instance in each of its levels, as any of them may be the one chosen; then the layers,
 * which the CPU writes. */
uint32_t layersAt(const Vulkan* v) { return v->instanceCapacity * (ARTF_MAX_LODS + 1) * 4; }
/* (and after all that, a copy for each particle of emitters of many: their instances follow the instances and their
 * motion, at twice the room for instances) */
uint32_t swarmCopiesAt(const Vulkan* v) { return layersAt(v) + v->instanceCapacity * 3; }
VkDeviceSize visibleSize(const Vulkan* v) { return ((VkDeviceSize)swarmCopiesAt(v) + v->swarmRoom) * 2 * 4; }
VkDeviceSize instanceBytes(const Vulkan* v) { return (VkDeviceSize)v->instanceCapacity * 128 + (VkDeviceSize)v->swarmRoom * 64; }


/* What the shaders need of meshlets, for meshes that have many, do not bend, and are on the GPU (not removed, sent
 * away by residency, or refused for want of memory: their ranges hold nothing to read). */
bool meshletsUsed(const Vulkan* v, const ArtfMeshData* m, size_t i) { return (m->meshletCount >= MESHLET_LEAST || m->dagBounds) && !m->skin && !m->removed && i < v->meshes.len && v->meshes.items[i].indexCount; }
bool meshletsUp(Vulkan* v, const ArtfScene* scene) {
    size_t rows = 1;
    for (size_t i = 0; i < scene->meshes.len; ++i) {
        const ArtfMeshData* m = &scene->meshes.items[i];
        if (meshletsUsed(v, m, i)) rows += (size_t)m->meshletCount * MESHLET_ROWS;
    }
    const VkDeviceSize tableBytes = (VkDeviceSize)rows * 16, meshBytes = ((VkDeviceSize)scene->meshes.len + 1) * 16;
    if (tableBytes > v->meshletTable.size) {
        bufferFree(v, &v->meshletTable);
        if (!bufferNew(v, &v->meshletTable, tableBytes * 2, BUFFER_STORAGE, true)) return false;
    }
    if (meshBytes > v->meshletMeshes.size) {
        bufferFree(v, &v->meshletMeshes);
        if (!bufferNew(v, &v->meshletMeshes, meshBytes * 2, BUFFER_STORAGE, true)) return false;
        for (int i = 0; i < SLOTS; ++i) v->slots[i].meshletBound = false;
    }
    float* table = v->meshletTable.mapped;
    uint32_t* meshes = v->meshletMeshes.mapped;
    uint32_t first = 0;
    v->mostMeshlets = 0;
    for (size_t i = 0; i < scene->meshes.len; ++i) {
        const ArtfMeshData* m = &scene->meshes.items[i];
        const MeshRange* range = &v->meshes.items[i];
        const bool used = meshletsUsed(v, m, i);
        const uint32_t info[4] = {first, used ? m->meshletCount : 0, range->firstIndex, (uint32_t)range->vertexOffset};
        memcpy(meshes + i * 4, info, sizeof info);
        if (!used) continue;
        v->mostMeshlets = artfMaxi((int)v->mostMeshlets, (int)m->meshletCount);
        for (uint32_t k = 0; k < m->meshletCount; ++k, table += MESHLET_ROWS * 4) {
            const ArtfMeshlet* l = &m->meshlets[k];
            const uint32_t indices[2] = {l->triangleOffset * 3, l->triangleCount * 3};
            memcpy(table, l->center, 16);
            memcpy(table + 4, l->cone, 16);
            memcpy(table + 8, indices, 8);
            /* a graph's cluster: its bounds and its parent's (artfDagDrawn); else always drawn, as on the first level */
            const float* b = m->dagBounds ? m->dagBounds + (size_t)k * 10 : NULL;
            const float none[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 3e38f};
            if (!b) b = none;
            memcpy(table + 12, b, 16);
            memcpy(table + 16, b + 5, 16);
            table[20] = b[4];
            table[21] = b[9] < 1e30f ? b[9] : 3e38f;
            table[22] = !m->dagBounds || k < m->dagWhole ? 1.f : 0.f;
            table[23] = 0;
        }
        first += m->meshletCount;
    }
    return true;
}

/* What the scene gained since it was last drawn. That is rare, so it may wait for the GPU. */
bool syncScene(Vulkan* v, ArtfScene* scene) {
    bool rebind = false;
    if (v->sceneId != scene->id) {
        v->vkDeviceWaitIdle(v->device);
        v->sceneId = scene->id;
        for (size_t i = 0; i < v->textures.len; ++i) imageFree(v, &v->textures.items[i]);
        v->textures.len = 0;
        v->textureMarks.len = 0;
        v->textureHeld.len = 0;
        v->meshes.len = 0;
        v->vertexCount = 0;
        v->indexCount = 0;
        v->colorCount = 1;
        v->vertexSpans.len = v->indexSpans.len = v->colorSpans.len = 0;
        v->meshRemovals = scene->meshRemovals;
        v->textureVersion = ~scene->textureVersion;
        v->meshTableVersion = ~(uint64_t)0; /* (another scene may have as many meshes) */
        for (int i = 0; i < SLOTS; ++i) v->slots[i].fresh = true;
    }

    /* what went gives its room back; a new mesh in its place needs room of its own */
    bool refill = false;
    if (v->meshRemovals != scene->meshRemovals) {
        v->vkDeviceWaitIdle(v->device);
        v->meshRemovals = scene->meshRemovals;
        for (size_t i = 0; i < v->meshes.len; ++i) {
            MeshRange* range = &v->meshes.items[i];
            const ArtfMeshData* m = &scene->meshes.items[i];
            if (range->refused) {
                /* (memory went: one that did not fit is tried again) */
                refill = refill || !m->removed;
                memset(range, 0, sizeof *range);
                range->generation = m->removed ? m->generation : ~m->generation;
                continue;
            }
            if (!m->removed && range->generation == m->generation) continue;
            refill = refill || !m->removed;
            if (!range->indexCount) { range->generation = m->removed ? m->generation : ~m->generation; continue; }
            spanGive(&v->vertexSpans, (uint32_t)range->vertexOffset, range->vertexLength);
            spanGive(&v->indexSpans, range->firstIndex, range->indexLength);
            spanGive(&v->colorSpans, range->colorBase, range->colorLength);
            memset(range, 0, sizeof *range);
            range->generation = m->removed ? m->generation : ~m->generation;
        }
        v->meshTableVersion = ~(uint64_t)0;
        for (int i = 0; i < SLOTS; ++i) {
            v->slots[i].fresh = true;
            v->slots[i].structureVersion = ~(uint64_t)0;
        }
    }

    if (scene->meshes.len > v->meshes.len || refill) {
        v->vkDeviceWaitIdle(v->device);
        /* in what was given back, or after what is there; outgrown: larger buffers, and all goes up again. Larger by
         * twice what is there, or by what is there when that does not fit in the GPU's memory; when that does not
         * either, by half as much again and again, and what does not fit then is refused: not drawn, and said
         * (ARTF_EVENT_REFUSED), and the rest goes on */
        bool refusing = false;
        for (;;) {
            bool fits = true;
            for (size_t i = 0; i < scene->meshes.len && fits; ++i) {
                const ArtfMeshData* m = &scene->meshes.items[i];
                if (i < v->meshes.len && (m->removed || v->meshes.items[i].generation == m->generation)) continue;
                MeshRange range = {0, 0, 0, 0, m->skin ? m->pose.applied : m->deformVersion, 0, 0, m->generation, 0};
                if (m->vertexCount && !m->evicted) {
                    const uint32_t atVertex = spanTake(&v->vertexSpans, m->vertexCount, &v->vertexCount, (uint32_t)(v->vertices.size / sizeof(ArtfPackedVertex)));
                    const uint32_t atIndex = atVertex == UINT32_MAX ? UINT32_MAX : spanTake(&v->indexSpans, ARTF_MESH_ALL_INDICES(m), &v->indexCount, (uint32_t)(v->indices.size / 4));
                    const uint32_t atColor = atIndex == UINT32_MAX || !m->colors ? 0 : spanTake(&v->colorSpans, m->vertexCount, &v->colorCount, (uint32_t)(v->colors.size / 4));
                    const bool room = atVertex != UINT32_MAX && atIndex != UINT32_MAX && atColor != UINT32_MAX;
                    if (!room && !refusing) {
                        fits = false;
                        break;
                    }
                    /* (what goes up through a staging buffer may find no memory for that either) */
                    const bool up = room && upload(v, &v->vertices, (VkDeviceSize)atVertex * sizeof(ArtfPackedVertex), m->vertices, (VkDeviceSize)m->vertexCount * sizeof(ArtfPackedVertex)) &&
                                    upload(v, &v->indices, (VkDeviceSize)atIndex * 4, m->indices, (VkDeviceSize)ARTF_MESH_ALL_INDICES(m) * 4) &&
                                    (!m->colors || upload(v, &v->colors, (VkDeviceSize)atColor * 4, m->colors, (VkDeviceSize)m->vertexCount * 4));
                    if (!up) {
                        if (atVertex != UINT32_MAX) spanGive(&v->vertexSpans, atVertex, m->vertexCount);
                        if (atIndex != UINT32_MAX) spanGive(&v->indexSpans, atIndex, ARTF_MESH_ALL_INDICES(m));
                        if (atColor != UINT32_MAX && m->colors) spanGive(&v->colorSpans, atColor, m->vertexCount);
                        const ArtfMesh handle = artfMeshHandleOf(scene, (uint32_t)i);
                        artfEventSay(ARTF_EVENT_REFUSED, v, handle, (uint64_t)m->vertexCount * sizeof(ArtfPackedVertex) + (uint64_t)m->indexCount * 4,
                                     "artful: mesh %u does not fit in the GPU's memory: it is not drawn until a mesh goes", handle);
                        range.refused = 1;
                    } else {
                        range.firstIndex = atIndex;
                        range.vertexOffset = (int32_t)atVertex;
                        range.indexCount = m->indexCount;
                        range.indexLength = ARTF_MESH_ALL_INDICES(m);
                        range.colorBase = atColor;
                        range.vertexLength = m->vertexCount;
                        range.colorLength = m->colors ? m->vertexCount : 0;
                    }
                }
                if (i < v->meshes.len) v->meshes.items[i] = range;
                else artfVecPush(&v->meshes, range);
            }
            if (fits) break;
            uint64_t vertices = 0, indices = 0, colors = 1;
            for (size_t k = 0; k < scene->meshes.len; ++k) {
                vertices += scene->meshes.items[k].vertexCount;
                indices += ARTF_MESH_ALL_INDICES(&scene->meshes.items[k]);
                if (scene->meshes.items[k].colors) colors += scene->meshes.items[k].vertexCount;
            }
            bool made = false;
            for (int attempt = 0; attempt < 16 && !made; ++attempt) {
                bufferFree(v, &v->vertices);
                bufferFree(v, &v->indices);
                bufferFree(v, &v->colors);
                /* (twice, as much, then half and half again: the last of them refusing) */
                const uint64_t times = attempt == 0 ? 4 : attempt == 1 ? 2 : 1, shift = attempt <= 1 ? 1 : (uint64_t)(attempt - 1);
                const uint64_t vs = (vertices * times >> shift) + 1, is = (indices * times >> shift) + 3, cs = (colors * times >> shift) + 1;
                made = bufferNew(v, &v->vertices, vs * sizeof(ArtfPackedVertex), BUFFER_VERTEX | BUFFER_STORAGE | BUFFER_TRANSFER_DST | BUFFER_TRANSFER_SRC, false) &&
                       bufferNew(v, &v->indices, is * 4, BUFFER_INDEX | BUFFER_TRANSFER_DST, false) && bufferNew(v, &v->colors, cs * 4, BUFFER_STORAGE | BUFFER_TRANSFER_DST, false) &&
                       colorsReset(v);
                refusing = attempt >= 2;
            }
            if (!made) return false; /* (not even a sixteen-thousandth of it: what is drawn fails, and says so) */
            rebind = true;
            v->meshes.len = 0;
            v->vertexCount = 0;
            v->indexCount = 0;
            v->vertexSpans.len = v->indexSpans.len = v->colorSpans.len = 0;
            v->meshTableVersion = ~(uint64_t)0;
        }
        /* the table the shaders read; and the commands: one a mesh, for each of the four parts */
        if (v->meshes.len > v->meshCapacity) {
            v->meshCapacity = (uint32_t)v->meshes.len * 2;
            bufferFree(v, &v->meshTable);
            if (!bufferNew(v, &v->meshTable, (VkDeviceSize)v->meshCapacity * 80, BUFFER_STORAGE, true)) return false;
            for (int i = 0; i < SLOTS; ++i) {
                Slot* s = &v->slots[i];
                bufferFree(v, &s->indirect);
                bufferFree(v, &s->template_);
                if (!bufferNew(v, &s->indirect, (VkDeviceSize)v->meshCapacity * 80, BUFFER_INDIRECT | BUFFER_STORAGE | BUFFER_TRANSFER_DST, false)) return false;
                if (!bufferNew(v, &s->template_, (VkDeviceSize)v->meshCapacity * 80, BUFFER_TRANSFER_SRC, true)) return false;
            }
            rebind = true;
        }
    }
    /* (a mesh already up may have just got its lighter versions: so this is written every time) */
    if (v->meshTableVersion != scene->meshes.len + scene->lodVersion) {
        v->meshTableVersion = scene->meshes.len + scene->lodVersion;
        v->vkDeviceWaitIdle(v->device);
        if (!meshletsUp(v, scene)) {
            v->meshTableVersion = ~(uint64_t)0; /* (no room: written again the next frame) */
            return false;
        }
        float* out = v->meshTable.mapped;
        for (size_t i = 0; i < scene->meshes.len; ++i, out += 20) {
            const ArtfMeshData* m = &scene->meshes.items[i];
            float row[20] = {m->center[0], m->center[1], m->center[2], m->radius, m->scale[0], m->scale[1], m->scale[2], 0, m->offset[0], m->offset[1], m->offset[2], 0};
            /* (where its colors are, and the first vertex they go with, as bits) */
            memcpy(&row[7], &v->meshes.items[i].colorBase, 4);
            memcpy(&row[11], &v->meshes.items[i].vertexOffset, 4);
            for (uint32_t k = 0; k < ARTF_MAX_LODS; ++k) {
                const uint32_t id = k < m->lodCount ? m->lods[k] : 0;
                memcpy(&row[12 + k], &id, 4);
                row[16 + k] = k < m->lodCount ? artfMeshLevelError(m, k) : -1.f;
            }
            row[19] = m->dagBounds ? 1.f : 0.f; /* (a graph of clusters: shaders.c's drawnErrorSpv) */
            memcpy(out, row, sizeof row);
        }
        for (int i = 0; i < SLOTS; ++i) v->slots[i].fresh = true;
    }

    const size_t textures = scene->textures.len < v->maxTextures ? scene->textures.len : v->maxTextures;
    if (v->textureVersion != scene->textureVersion || textures > v->textures.len) {
        v->textureVersion = scene->textureVersion;
        for (size_t i = 0; i < textures; ++i) {
            ArtfTextureData* t = &scene->textures.items[i];
            const uint64_t mark = (uint64_t)t->generation << 8 | (uint64_t)artfMini(t->residentMip, artfMaxi(t->mipCount - 1, 0));
            if (i < v->textures.len && v->textureMarks.items[i] == mark) continue;
            v->vkDeviceWaitIdle(v->device);
            if (t->removed) {
                /* (white in its place, which every entry of the array must have; its image goes now, not when another
                 * texture comes to its place: the device is idle) */
                if (i >= v->textures.len) continue;
                textureWhite(v, (uint32_t)i);
                if (i) imageFree(v, &v->textures.items[i]);
                v->textureMarks.items[i] = mark;
                continue;
            }
            bool compress = v->compressTextures && i;
            for (size_t m = 0; m < scene->materials.len && compress; ++m) compress = scene->materials.items[m].normalMap != i;
            if (!uploadTexture(v, t, (uint32_t)i, compress, artfTextureHandleOf(scene, (uint32_t)i))) return false;
        }
        /* the GPU's memory ran out for textures: residency makes room, and those short of what was asked are tried
         * again as the others come back smaller */
        if (v->texturesShort) {
            v->texturesShort = false;
            artfSceneResidencyPress(scene);
        }
    }

    if (scene->instances.len > v->instanceCapacity || v->swarmPlaces > v->swarmRoom) {
        v->vkDeviceWaitIdle(v->device);
        /* (room for twice as many, or as many when that does not fit in the GPU's memory; when that does not either,
         * the room there was, and this frame is not drawn: said, and tried again the next) */
        const uint32_t was = v->instanceCapacity, wasSwarms = v->swarmRoom;
        const uint32_t wanted = scene->instances.len > was ? (uint32_t)scene->instances.len : was;
        if (v->swarmPlaces > v->swarmRoom) v->swarmRoom = v->swarmPlaces;
        bool made = false;
        for (int attempt = 0; attempt < 3 && !made; ++attempt) {
            v->instanceCapacity = attempt == 0 && wanted > was ? wanted * 2 : attempt <= 1 ? wanted : was;
            if (attempt == 2) v->swarmRoom = wasSwarms;
            made = true;
            for (int i = 0; i < SLOTS; ++i) {
                Slot* s = &v->slots[i];
                bufferFree(v, &s->instances);
                bufferFree(v, &s->visible);
                made = made && bufferNew(v, &s->instances, instanceBytes(v), BUFFER_STORAGE, true) && bufferNew(v, &s->visible, visibleSize(v), BUFFER_STORAGE | BUFFER_TRANSFER_DST, false);
                s->fresh = true;
            }
            made = made && flagsMake(v);
        }
        rebind = true;
        if (!made || v->instanceCapacity < scene->instances.len || v->swarmRoom < v->swarmPlaces) {
            for (int i = 0; i < SLOTS; ++i) writeBufferDescriptors(v, &v->slots[i]);
            return false;
        }
    }
    if (scene->materials.len > v->materialCapacity) {
        v->vkDeviceWaitIdle(v->device);
        /* (as the instances') */
        const uint32_t was = v->materialCapacity, wanted = (uint32_t)scene->materials.len;
        bool made = false;
        for (int attempt = 0; attempt < 3 && !made; ++attempt) {
            v->materialCapacity = attempt == 0 ? wanted * 2 : attempt == 1 ? wanted : was;
            made = true;
            for (int i = 0; i < SLOTS; ++i) {
                Slot* s = &v->slots[i];
                bufferFree(v, &s->materials);
                made = made && bufferNew(v, &s->materials, (VkDeviceSize)(v->materialCapacity ? v->materialCapacity : 1) * (sizeof(ArtfMaterialData) + sizeof(ArtfMaterialMore)), BUFFER_STORAGE, true);
                s->fresh = true;
            }
        }
        rebind = true;
        if (!made || v->materialCapacity < scene->materials.len) {
            for (int i = 0; i < SLOTS; ++i) writeBufferDescriptors(v, &v->slots[i]);
            return false;
        }
    }
    if (rebind)
        for (int i = 0; i < SLOTS; ++i) {
        writeBufferDescriptors(v, &v->slots[i]);
        lightDescriptors(v, &v->slots[i]);
    }
    return true;
}

/* Brings this slot's copy of what changes up to date. */
void syncSlot(Vulkan* v, Slot* s, const ArtfScene* scene) {
    if (s->fresh || s->materialVersion != scene->materialVersion) {
        memcpy(s->materials.mapped, scene->materials.items, scene->materials.len * sizeof(ArtfMaterialData));
        /* (a coat and sheen after them all: ArtfFrameData.materials says where) */
        memcpy((char*)s->materials.mapped + scene->materials.len * sizeof(ArtfMaterialData), scene->materialsMore.items, scene->materials.len * sizeof(ArtfMaterialMore));
        s->materialVersion = scene->materialVersion;
    }
    if (s->fresh || s->instanceVersion != scene->instanceVersion) {
        if (scene->instances.len) memcpy(s->instances.mapped, scene->instances.items, scene->instances.len * sizeof(ArtfInstanceData));
        s->instanceVersion = scene->instanceVersion;
    }
    if (s->fresh || s->structureVersion != scene->structureVersion || s->lodVersion != scene->lodVersion) {
        s->lodVersion = scene->lodVersion;
        /* each mesh's copies get a stretch of the visible list, as large as they could fill */
        artfVecReserve(&v->copyStarts, v->meshes.len + 1);
        uint32_t* starts = v->copyStarts.items;
        memset(starts, 0, (v->meshes.len + 1) * 4);
        for (size_t i = 0; i < scene->instances.len; ++i) {
            const uint32_t mesh = scene->instances.items[i].mesh;
            const ArtfMeshData* m = &scene->meshes.items[mesh];
            ++starts[mesh];
            for (uint32_t k = 0; k < m->lodCount; ++k) ++starts[m->lods[k]];
        }
        uint32_t* out = s->template_.mapped;
        uint32_t at = 0;
        for (size_t i = 0; i < v->meshes.len; ++i, out += 5) {
            const MeshRange* m = &v->meshes.items[i];
            const uint32_t command[5] = {m->indexCount, 0, m->firstIndex, (uint32_t)m->vertexOffset, at};
            memcpy(out, command, sizeof command);
            at += starts[i];
        }
        /* the other parts' commands: the same, each with its own stretch of the list */
        for (uint32_t part = 1; part < 4; ++part, out += v->meshes.len * 5) {
            memcpy(out, s->template_.mapped, v->meshes.len * 20);
            for (size_t i = 0; i < v->meshes.len; ++i) out[i * 5 + 4] += at * part;
        }
        s->structureVersion = scene->structureVersion;
    }
    s->fresh = false;
}

/* ---- a background pass ---- */

/* The program's shader, as a pipeline, when the scene's is another. It goes out in bands, each from a base:
 * VK_PIPELINE_CREATE_DISPATCH_BASE_BIT. No shader, or one the driver will not take: no pipeline, and no light. */
void backgroundPipelineMake(Vulkan* v, const ArtfScene* scene) {
    const ArtfBackground* b = &scene->backgroundPass;
    if (v->backgroundModuleStamp == b->moduleStamp && v->backgroundModuleScene == scene->id) return;
    v->backgroundModuleStamp = b->moduleStamp;
    v->backgroundModuleScene = scene->id;
    if (v->backgroundPipeline) {
        v->vkDeviceWaitIdle(v->device);
        v->vkDestroyPipeline(v->device, v->backgroundPipeline, NULL);
        v->backgroundPipeline = 0;
    }
    if (!b->spirv) return;
    uint32_t* code = artfMalloc(b->pass.spirvBytes);
    memcpy(code, b->spirv, b->pass.spirvBytes);
    VkShaderModule module = shaderModule(v, code, b->pass.spirvBytes);
    if (module) {
        const ComputePipelineCreateInfo c = {ST_COMPUTE_PIPELINE, NULL, 0x10, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_COMPUTE, module, "main", NULL}, v->pipelineLayout, 0, -1};
        if (v->vkCreateComputePipelines(v->device, v->pipelineCache, 1, &c, NULL, &v->backgroundPipeline) != VK_OK) v->backgroundPipeline = 0;
        v->vkDestroyShaderModule(v->device, module, NULL);
    }
    if (!v->backgroundPipeline) artfLog("artful: the background pass's shader could not be made");
}

/* Room for its light at this size, and its data, where they changed. That is rare, so it may wait. */
bool syncBackground(Vulkan* v, const ArtfScene* scene, uint32_t bloomPixels) {
    const VkDeviceSize hdrSize = (VkDeviceSize)v->sceneWidth * v->sceneHeight * 16, bloomSize = (VkDeviceSize)bloomPixels * 16;
    bool rebind = false;
    if (v->hdr.size != hdrSize || v->bloom.size != bloomSize) {
        v->vkDeviceWaitIdle(v->device);
        bufferFree(v, &v->hdr);
        bufferFree(v, &v->bloom);
        if (!bufferNew(v, &v->hdr, hdrSize, BUFFER_STORAGE | BUFFER_TRANSFER_DST, false)) return false;
        if (!bufferNew(v, &v->bloom, bloomSize, BUFFER_STORAGE, false)) return false;
        rebind = true;
    }
    const ArtfBackgroundPass* p = &scene->backgroundPass.pass;
    if (v->backgroundDataStamp != scene->backgroundPass.dataStamp || v->backgroundDataScene != scene->id || !v->backgroundData.buffer) {
        v->vkDeviceWaitIdle(v->device);
        const VkDeviceSize bytes = p->data ? (VkDeviceSize)((p->dataBytes + 3) & ~(size_t)3) : 0;
        if (!v->backgroundData.buffer || v->backgroundData.size < bytes) {
            bufferFree(v, &v->backgroundData);
            if (!bufferNew(v, &v->backgroundData, bytes, BUFFER_STORAGE | BUFFER_TRANSFER_DST, false)) return false;
            rebind = true;
        }
        if (p->data && !upload(v, &v->backgroundData, 0, p->data, p->dataBytes)) return false;
        v->backgroundDataStamp = scene->backgroundPass.dataStamp;
        v->backgroundDataScene = scene->id;
    }
    backgroundPipelineMake(v, scene);
    if (rebind) {
        const DescriptorBufferInfo infos[3] = {{v->hdr.buffer, 0, ~(VkDeviceSize)0}, {v->bloom.buffer, 0, ~(VkDeviceSize)0}, {v->backgroundData.buffer, 0, ~(VkDeviceSize)0}};
        for (int i = 0; i < SLOTS; ++i) {
            WriteDescriptorSet writes[3];
            for (uint32_t k = 0; k < 3; ++k) {
                const WriteDescriptorSet w = {ST_WRITE_DESCRIPTOR_SET, NULL, v->slots[i].set, 12 + k, 0, 1, DESCRIPTOR_STORAGE, NULL, &infos[k], NULL};
                writes[k] = w;
            }
            v->vkUpdateDescriptorSets(v->device, 3, writes, 0, NULL);
        }
    }
    return true;
}

/* Rays enough to go out on their own: a 4K frame of 64 rays a pixel is seconds of work, and Windows takes
 * a device that keeps the GPU two seconds as lost. Nor does a driver have to give it back between the
 * dispatches of one submission: so a frame this large goes out in bands of rows, each a submission of
 * its own, waited for. Below this a frame is one dispatch, as fast as it can go. */
#define BACKGROUND_RAYS_APART 64000000ull
#define BACKGROUND_RAYS_A_BAND 8000000u

uint32_t backgroundRaysPerPixel(const ArtfScene* scene) { return (uint32_t)artfMaxi(scene->backgroundPass.pass.raysPerPixel, 1); }

uint64_t backgroundRays(const Vulkan* v, const ArtfScene* scene) {
    return (uint64_t)v->sceneWidth * (uint64_t)v->sceneHeight * backgroundRaysPerPixel(scene);
}

bool traceBackgroundApart(Vulkan* v, Slot* s, const ArtfScene* scene) {
    const VkFlags readWrite = ACCESS_SHADER_READ | ACCESS_SHADER_WRITE;
    const uint32_t groupsX = ((uint32_t)v->sceneWidth + 7) / 8, groupsY = ((uint32_t)v->sceneHeight + 7) / 8;
    const uint32_t raysPerGroupRow = groupsX * 64u * backgroundRaysPerPixel(scene);
    const uint32_t band = (uint32_t)artfMaxi(1, (int)(BACKGROUND_RAYS_A_BAND / raysPerGroupRow));
    for (uint32_t y = 0; y < groupsY; y += band) {
        VkCommandBuffer c = oneShotBegin(v);
        /* (the frame before reads what this one is about to write) */
        memoryBarrier(v, c, STAGE_COMPUTE | STAGE_FRAGMENT, STAGE_COMPUTE, readWrite, readWrite);
        v->vkCmdBindPipeline(c, 1, v->backgroundPipeline);
        v->vkCmdBindDescriptorSets(c, 1, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
        v->vkCmdDispatchBase(c, 0, y, 0, groupsX, (uint32_t)artfMini((int)band, (int)(groupsY - y)), 1);
        if (!oneShotEnd(v, c)) return false;
    }
    return true;
}

/* Its rays, unless they went out apart, and its bloom: all that comes before the render pass. */
void traceBackground(Vulkan* v, VkCommandBuffer c, Slot* s, const ArtfBackgroundBlock* h, bool raysDone) {
    const VkFlags readWrite = ACCESS_SHADER_READ | ACCESS_SHADER_WRITE;
    /* (the frame before reads what this one is about to write; or, the bands wrote it) */
    memoryBarrier(v, c, STAGE_COMPUTE | STAGE_FRAGMENT, STAGE_COMPUTE, readWrite, readWrite);
    v->vkCmdBindDescriptorSets(c, 1, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
    if (!raysDone && v->backgroundPipeline) {
        v->vkCmdBindPipeline(c, 1, v->backgroundPipeline);
        v->vkCmdDispatch(c, ((uint32_t)v->sceneWidth + 7) / 8, ((uint32_t)v->sceneHeight + 7) / 8, 1);
        memoryBarrier(v, c, STAGE_COMPUTE, STAGE_COMPUTE, ACCESS_SHADER_WRITE, readWrite);
    } else if (!raysDone) {
        /* (no shader: no light) */
        memoryBarrier(v, c, STAGE_COMPUTE, STAGE_TRANSFER, readWrite, ACCESS_TRANSFER_WRITE);
        v->vkCmdFillBuffer(c, v->hdr.buffer, 0, ~(VkDeviceSize)0, 0);
        memoryBarrier(v, c, STAGE_TRANSFER, STAGE_COMPUTE, ACCESS_TRANSFER_WRITE, readWrite);
    }
    const uint32_t levels = h->size[2];
    v->vkCmdBindPipeline(c, 1, v->bloomFirstPipeline);
    for (uint32_t i = 0; i < levels; ++i) {
        if (i == 1) v->vkCmdBindPipeline(c, 1, v->bloomNextPipeline);
        v->vkCmdPushConstants(c, v->pipelineLayout, SHADER_COMPUTE, 0, 4, &i);
        v->vkCmdDispatch(c, (h->bloom[i][1] * h->bloom[i][2] + 63) / 64, 1, 1);
        memoryBarrier(v, c, STAGE_COMPUTE, STAGE_COMPUTE, ACCESS_SHADER_WRITE, readWrite);
    }
    v->vkCmdBindPipeline(c, 1, v->bloomUpPipeline);
    for (uint32_t i = levels - 1; i-- > 0;) {
        v->vkCmdPushConstants(c, v->pipelineLayout, SHADER_COMPUTE, 0, 4, &i);
        v->vkCmdDispatch(c, (h->bloom[i][1] * h->bloom[i][2] + 63) / 64, 1, 1);
        memoryBarrier(v, c, STAGE_COMPUTE, STAGE_COMPUTE, ACCESS_SHADER_WRITE, readWrite);
    }
    memoryBarrier(v, c, STAGE_COMPUTE, STAGE_FRAGMENT, ACCESS_SHADER_WRITE, ACCESS_SHADER_READ);
}

/* ---- the overlay ---- */

void vkSetOverlay(ArtfRenderer* r, const ArtfOverlay* o) {
    Vulkan* v = (Vulkan*)r;
    if (!o || !o->pixels) {
        v->overlayPixels = NULL;
        v->overlayPending = false;
        v->overlayUsable = false;
        v->overlayDamage.len = 0;
        return;
    }
    v->overlayPixels = o->pixels;
    v->overlayWidth = o->width;
    v->overlayHeight = o->height;
    v->overlayStride = o->stride ? o->stride : o->width;
    if (o->partial) {
        for (size_t i = 0; i < o->damageCount; ++i) artfVecPush(&v->overlayDamage, o->damage[i]);
    } else {
        v->overlayFull = true;
    }
    if (!o->partial || o->damageCount) v->overlayPending = true;
}

/* Sends what changed of the overlay up; only that, so that a page that barely changes costs nothing. */
bool uploadOverlay(Vulkan* v, Slot* s, VkCommandBuffer c) {
    if (!v->overlayPending) return true;
    const size_t bytes = (size_t)v->width * v->height * 4;
    if (!s->overlayStaging.buffer && !bufferNew(v, &s->overlayStaging, bytes, BUFFER_TRANSFER_SRC, true)) return false;
    uint32_t* staging = s->overlayStaging.mapped;
    const int ow = artfMini(v->overlayWidth, v->width), oh = artfMini(v->overlayHeight, v->height);

    BufferImageCopy regions[32];
    uint32_t regionCount = 0;
    size_t used = 0;
    bool full = v->overlayFull || !v->overlayUsable || v->overlayDamage.len > ARTF_COUNT(regions);
    if (!full) {
        for (size_t i = 0; i < v->overlayDamage.len; ++i) {
            const ArtfRect d = v->overlayDamage.items[i];
            const int x0 = artfMaxi(d.x, 0), y0 = artfMaxi(d.y, 0), x1 = artfMini(d.x + d.w, ow), y1 = artfMini(d.y + d.h, oh);
            if (x1 <= x0 || y1 <= y0) continue;
            const size_t w = (size_t)(x1 - x0), h = (size_t)(y1 - y0);
            if (used + w * h > (size_t)v->width * v->height) { full = true; break; }
            BufferImageCopy region = {(VkDeviceSize)used * 4, 0, 0, {ASPECT_COLOR, 0, 0, 1}, {x0, y0, 0}, {(uint32_t)w, (uint32_t)h, 1}};
            regions[regionCount++] = region;
            for (size_t y = 0; y < h; ++y) memcpy(staging + used + y * w, v->overlayPixels + (size_t)(y0 + (int)y) * v->overlayStride + x0, w * 4);
            used += w * h;
        }
    }
    if (full) {
        if (ow < v->width || oh < v->height) memset(staging, 0, bytes);
        for (int y = 0; y < oh; ++y) memcpy(staging + (size_t)y * v->width, v->overlayPixels + (size_t)y * v->overlayStride, (size_t)ow * 4);
        BufferImageCopy region = {0, 0, 0, {ASPECT_COLOR, 0, 0, 1}, {0, 0, 0}, {(uint32_t)v->width, (uint32_t)v->height, 1}};
        regions[0] = region;
        regionCount = 1;
    }
    if (regionCount) {
        const int from = v->overlayUsable ? LAYOUT_SHADER_READ : LAYOUT_UNDEFINED;
        imageBarrier(v, c, v->overlay.image, 1, from, LAYOUT_TRANSFER_DST, STAGE_FRAGMENT, STAGE_TRANSFER, 0, ACCESS_TRANSFER_WRITE);
        v->vkCmdCopyBufferToImage(c, s->overlayStaging.buffer, v->overlay.image, LAYOUT_TRANSFER_DST, regionCount, regions);
        imageBarrier(v, c, v->overlay.image, 1, LAYOUT_TRANSFER_DST, LAYOUT_SHADER_READ, STAGE_TRANSFER, STAGE_FRAGMENT, ACCESS_TRANSFER_WRITE, ACCESS_SHADER_READ);
        v->overlayUsable = true;
    }
    v->overlayPending = false;
    v->overlayFull = false;
    v->overlayDamage.len = 0;
    return true;
}

/* ---- a frame ---- */

/* The scene's commands from first on: in one call where the device can, or one by one. */
void drawCommands(Vulkan* v, VkCommandBuffer c, Slot* s, VkPipeline pipeline, uint32_t first, uint32_t count) {
    const VkDeviceSize zero = 0;
    v->vkCmdBindPipeline(c, 0, pipeline);
    v->vkCmdBindDescriptorSets(c, 0, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
    v->vkCmdBindVertexBuffers(c, 0, 1, &v->vertices.buffer, &zero);
    v->vkCmdBindIndexBuffer(c, v->indices.buffer, 0, 1 /* 32 bits */);
    if (v->multiDraw) {
        v->vkCmdDrawIndexedIndirect(c, s->indirect.buffer, (VkDeviceSize)first * 20, count, 20);
    } else {
        for (uint32_t i = 0; i < count; ++i) v->vkCmdDrawIndexedIndirect(c, s->indirect.buffer, (VkDeviceSize)(first + i) * 20, 1, 20);
    }
}

/* Vertices a pose moved go up with the frame, in its own commands, so that nothing has to wait. */
/* What only bones move goes on the GPU. */
bool bonesAlone(const ArtfMeshData* m) {
    if (!m->skin->joints || !m->pose.jointCount || m->pose.jointCount > 65535) return false;
    for (uint32_t t = 0; m->pose.weights && t < m->skin->morphCount; ++t)
        if (m->pose.weights[t] != 0) return false;
    return true;
}

/* All skins in use go up together, when one is new: rare. */
bool syncSkinData(Vulkan* v, const ArtfScene* scene) {
    if (v->skinScene != scene->id || v->skinRemovals != scene->meshRemovals) {
        v->skinPlaces.len = 0;
        v->skinScene = scene->id;
        v->skinRemovals = scene->meshRemovals;
    }
    bool missing = false;
    size_t rows = 0;
    ARTF_VEC(SkinPlace) places = {0};
    for (size_t i = 0; i < scene->deforming.len; ++i) {
        const ArtfMeshData* m = &scene->meshes.items[scene->deforming.items[i]];
        if (!m->skin->joints) continue;
        bool known = false;
        for (size_t k = 0; k < places.len && !known; ++k) known = places.items[k].skin == m->skin;
        if (known) continue;
        const SkinPlace place = {m->skin, (uint32_t)rows};
        artfVecPush(&places, place);
        rows += (size_t)m->vertexCount * 5;
        known = false;
        for (size_t k = 0; k < v->skinPlaces.len && !known; ++k) known = v->skinPlaces.items[k].skin == m->skin;
        missing = missing || !known;
    }
    if (!missing) {
        artfVecFree(&places);
        return true;
    }
    v->vkDeviceWaitIdle(v->device);
    bufferFree(v, &v->skinData);
    bool ok = bufferNew(v, &v->skinData, (VkDeviceSize)(rows + 1) * 16, BUFFER_STORAGE | BUFFER_TRANSFER_DST, false);
    float* all = artfMalloc((rows + 1) * 16);
    for (size_t k = 0; k < places.len && ok; ++k) {
        const ArtfSkinData* skin = places.items[k].skin;
        uint32_t count = 0;
        for (size_t i = 0; i < scene->deforming.len && !count; ++i)
            if (scene->meshes.items[scene->deforming.items[i]].skin == skin) count = scene->meshes.items[scene->deforming.items[i]].vertexCount;
        float* to = all + (size_t)places.items[k].row * 4;
        for (uint32_t i = 0; i < count; ++i, to += 20) {
            memcpy(to, skin->rest + (size_t)i * 6, 24);
            memcpy(to + 6, skin->joints + (size_t)i * 8, 16);
            to[10] = to[11] = 0;
            memcpy(to + 12, skin->weights + (size_t)i * 8, 32);
        }
    }
    if (ok) ok = upload(v, &v->skinData, 0, all, (VkDeviceSize)rows * 16);
    artfFree(all);
    v->skinPlaces.len = 0;
    for (size_t k = 0; k < places.len; ++k) artfVecPush(&v->skinPlaces, places.items[k]);
    artfVecFree(&places);
    return ok;
}

bool shadowCullMake(Vulkan* v) {
    if (v->shadowCullPipeline) return true;
    size_t size;
    uint32_t* code = artfShaderShadowCull(&size);
    VkShaderModule module = shaderModule(v, code, size);
    if (!module) return false;
    const ComputePipelineCreateInfo info = {ST_COMPUTE_PIPELINE, NULL, 0, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_COMPUTE, module, "main", NULL}, v->pipelineLayout, 0, -1};
    const bool ok = v->vkCreateComputePipelines(v->device, v->pipelineCache, 1, &info, NULL, &v->shadowCullPipeline) == VK_OK;
    v->vkDestroyShaderModule(v->device, module, NULL);
    return ok;
}


bool meshletPipelineMake(Vulkan* v) {
    for (int phase = 0; phase < 3; ++phase) {
        if (v->meshletPipelines[phase]) continue;
        size_t size;
        uint32_t* code = artfShaderMeshletCull(&size, (ArtfCullPhase)phase);
        VkShaderModule module = shaderModule(v, code, size);
        if (!module) return false;
        const ComputePipelineCreateInfo info = {ST_COMPUTE_PIPELINE, NULL, 0, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_COMPUTE, module, "main", NULL}, v->pipelineLayout, 0, -1};
        const bool ok = v->vkCreateComputePipelines(v->device, v->pipelineCache, 1, &info, NULL, &v->meshletPipelines[phase]) == VK_OK;
        v->vkDestroyShaderModule(v->device, module, NULL);
        if (!ok) return false;
    }
    return true;
}

/* A place for each meshlet of each instance to remember whether it showed: instances go by their mesh, the largest of
 * its levels. What no room is left for goes whole. */
bool meshletFlagsUp(Vulkan* v, const ArtfScene* scene) {
    enum { MOST = 1 << 22 };
    const uint64_t version = scene->structureVersion ^ scene->id << 32;
    if (v->meshletFlags.buffer && v->meshletFlagVersion == version) return true;
    v->vkDeviceWaitIdle(v->device);
    const VkDeviceSize startBytes = ((VkDeviceSize)scene->instances.len + 1) * 4;
    if (startBytes > v->meshletFlagStarts.size) {
        bufferFree(v, &v->meshletFlagStarts);
        if (!bufferNew(v, &v->meshletFlagStarts, startBytes * 2, BUFFER_STORAGE, true)) return false;
        for (int i = 0; i < SLOTS; ++i) bindStorage(v, &v->slots[i], 47, &v->meshletFlagStarts);
    }
    uint32_t* starts = v->meshletFlagStarts.mapped;
    uint32_t at = 0;
    for (size_t i = 0; i < scene->instances.len; ++i) {
        const uint32_t count = scene->meshes.items[scene->instances.items[i].mesh].meshletCount;
        /* (a lighter level has fewer) */
        starts[i] = at + count <= MOST ? at : ~0u;
        if (at + count <= MOST) at += count;
    }
    const VkDeviceSize bytes = ((VkDeviceSize)at + 1) * 4;
    if (bytes > v->meshletFlags.size) {
        bufferFree(v, &v->meshletFlags);
        if (!bufferNew(v, &v->meshletFlags, bytes * 2, BUFFER_STORAGE | BUFFER_TRANSFER_DST, false)) return false;
        for (int i = 0; i < SLOTS; ++i) bindStorage(v, &v->slots[i], 46, &v->meshletFlags);
    }
    VkCommandBuffer c = oneShotBegin(v);
    v->vkCmdFillBuffer(c, v->meshletFlags.buffer, 0, ~(VkDeviceSize)0, 0);
    if (!oneShotEnd(v, c)) return false;
    v->meshletFlagVersion = version;
    return true;
}

/* Where occlusion's second pass has its half of a slot's jobs. */
uint32_t meshletHalf(const Vulkan* v) { return v->instanceCapacity * 4 + MESHLET_HEADER; }

/* This slot's room for jobs, and its header for this frame: 0 commands fit when meshlets are not for this frame. */
bool stageMeshlets(Vulkan* v, Slot* s, bool on) {
    const VkDeviceSize stateBytes = (VkDeviceSize)meshletHalf(v) * 8;
    if (stateBytes > s->meshletState.size) {
        bufferFree(v, &s->meshletState);
        if (!bufferNew(v, &s->meshletState, stateBytes, BUFFER_STORAGE | BUFFER_INDIRECT, true)) return false;
        bindStorage(v, s, 43, &s->meshletState);
    }
    if (!s->meshletCommands.buffer) {
        if (!bufferNew(v, &s->meshletCommands, (VkDeviceSize)MESHLET_COMMANDS * 40, BUFFER_STORAGE | BUFFER_INDIRECT, false)) return false;
        bindStorage(v, s, 42, &s->meshletCommands);
    }
    if (s->meshletBoundTable != v->meshletTable.serial || !s->meshletBound) {
        bindStorage(v, s, 40, &v->meshletTable);
        bindStorage(v, s, 41, &v->meshletMeshes);
        s->meshletBoundTable = v->meshletTable.serial;
        s->meshletBound = true;
    }
    uint32_t* header = s->meshletState.mapped;
    const uint32_t groups = (v->mostMeshlets + 63) / 64, room = on ? MESHLET_COMMANDS : 0, half = meshletHalf(v);
    const uint32_t first[MESHLET_HEADER] = {groups, 0, 1, 0, layersAt(v) + v->instanceCapacity, room, half, 0, 0};
    const uint32_t second[MESHLET_HEADER] = {groups, 0, 1, 0, layersAt(v) + v->instanceCapacity * 2, room, half, MESHLET_COMMANDS, 0};
    memcpy(header, first, sizeof first);
    memcpy(header + half, second, sizeof second);
    return true;
}

bool skinPipelineMake(Vulkan* v) {
    if (v->skinPipeline) return true;
    size_t size;
    uint32_t* code = artfShaderSkin(&size);
    VkShaderModule module = shaderModule(v, code, size);
    if (!module) return false;
    const ComputePipelineCreateInfo info = {ST_COMPUTE_PIPELINE, NULL, 0, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_COMPUTE, module, "main", NULL}, v->pipelineLayout, 0, -1};
    const bool ok = v->vkCreateComputePipelines(v->device, v->pipelineCache, 1, &info, NULL, &v->skinPipeline) == VK_OK;
    v->vkDestroyShaderModule(v->device, module, NULL);
    return ok;
}

/* Jobs for the GPU, in this slot's table: three rows each, then their bones. */
bool stageBones(Vulkan* v, Slot* s, const ArtfScene* scene) {
    v->skinJobs.len = 0;
    size_t bones = 0;
    for (size_t i = 0; i < scene->deforming.len; ++i) {
        const uint32_t index = scene->deforming.items[i];
        const ArtfMeshData* m = &scene->meshes.items[index];
        if (v->meshes.items[index].deformVersion == m->deformVersion || !bonesAlone(m)) continue;
        artfVecPush(&v->skinJobs, index);
        bones += m->pose.jointCount;
    }
    if (!v->skinJobs.len) return true;
    if (!skinPipelineMake(v) || !syncSkinData(v, scene)) return false;
    const VkDeviceSize bytes = ((VkDeviceSize)v->skinJobs.len + bones) * 48;
    bool rebind = false;
    if (bytes > s->skinTable.size) {
        bufferFree(v, &s->skinTable);
        if (!bufferNew(v, &s->skinTable, bytes * 2, BUFFER_STORAGE, true)) return false;
        rebind = true;
    }
    if (rebind || s->skinBoundData != v->skinData.serial || s->skinBoundVertices != v->vertices.serial) {
        const DescriptorBufferInfo infos[3] = {{v->skinData.buffer, 0, ~(VkDeviceSize)0}, {s->skinTable.buffer, 0, ~(VkDeviceSize)0}, {v->vertices.buffer, 0, ~(VkDeviceSize)0}};
        for (uint32_t i = 0; i < 3; ++i) {
            const WriteDescriptorSet w = {ST_WRITE_DESCRIPTOR_SET, NULL, s->set, 34 + i, 0, 1, DESCRIPTOR_STORAGE, NULL, &infos[i], NULL};
            v->vkUpdateDescriptorSets(v->device, 1, &w, 0, NULL);
        }
        s->skinBoundData = v->skinData.serial;
        s->skinBoundVertices = v->vertices.serial;
    }
    uint32_t* rows = s->skinTable.mapped;
    uint32_t boneRow = (uint32_t)v->skinJobs.len * 3;
    for (size_t j = 0; j < v->skinJobs.len; ++j) {
        const uint32_t index = v->skinJobs.items[j];
        const ArtfMeshData* m = &scene->meshes.items[index];
        MeshRange* range = &v->meshes.items[index];
        uint32_t rest = 0;
        for (size_t k = 0; k < v->skinPlaces.len; ++k)
            if (v->skinPlaces.items[k].skin == m->skin) rest = v->skinPlaces.items[k].row;
        uint32_t* row = rows + j * 12;
        row[0] = (uint32_t)range->vertexOffset;
        row[1] = m->vertexCount;
        row[2] = rest;
        row[3] = boneRow;
        memcpy(row + 4, m->scale, 12);
        row[7] = m->pose.jointCount;
        memcpy(row + 8, m->offset, 12);
        row[11] = 0;
        memcpy(rows + (size_t)boneRow * 4, m->pose.joints, (size_t)m->pose.jointCount * 48);
        boneRow += m->pose.jointCount * 3;
        range->deformVersion = m->deformVersion;
    }
    return true;
}

void runBones(Vulkan* v, VkCommandBuffer c, Slot* s) {
    if (!v->skinJobs.len) return;
    memoryBarrier(v, c, STAGE_VERTEX_INPUT, STAGE_COMPUTE, ACCESS_VERTEX_READ, ACCESS_SHADER_WRITE);
    v->vkCmdBindPipeline(c, 1, v->skinPipeline);
    v->vkCmdBindDescriptorSets(c, 1, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
    for (uint32_t i = 0; i < (uint32_t)v->skinJobs.len; ++i) {
        /* (one after another: two jobs may pose the same vertices, a level and the whole it was made from) */
        if (i) memoryBarrier(v, c, STAGE_COMPUTE, STAGE_COMPUTE, ACCESS_SHADER_WRITE, ACCESS_SHADER_WRITE);
        v->vkCmdPushConstants(c, v->pipelineLayout, SHADER_COMPUTE, 0, 4, &i);
        const uint32_t count = v->scratchSkinCounts.items[i];
        v->vkCmdDispatch(c, (count + 63) / 64, 1, 1);
    }
    memoryBarrier(v, c, STAGE_COMPUTE, STAGE_VERTEX_INPUT, ACCESS_SHADER_WRITE, ACCESS_VERTEX_READ);
}

bool stagePoses(Vulkan* v, Slot* s, ArtfScene* scene) {
    if (!stageBones(v, s, scene)) return false;
    v->scratchSkinCounts.len = 0;
    for (size_t j = 0; j < v->skinJobs.len; ++j) artfVecPush(&v->scratchSkinCounts, scene->meshes.items[v->skinJobs.items[j]].vertexCount);
    v->posedCopies.len = 0;
    VkDeviceSize bytes = 0;
    for (size_t i = 0; i < scene->deforming.len; ++i) {
        const uint32_t index = scene->deforming.items[i];
        ArtfMeshData* m = &scene->meshes.items[index];
        if (v->meshes.items[index].deformVersion == m->deformVersion) continue;
        if (m->pose.applied != m->deformVersion) artfMeshDeform(m);
        bytes += (VkDeviceSize)m->vertexCount * sizeof(ArtfPackedVertex);
    }
    if (!bytes) return true;
    if (bytes > s->posed.size) {
        bufferFree(v, &s->posed);
        if (!bufferNew(v, &s->posed, bytes * 2, BUFFER_TRANSFER_SRC, true)) return false;
    }
    VkDeviceSize at = 0;
    for (size_t i = 0; i < scene->deforming.len; ++i) {
        const uint32_t index = scene->deforming.items[i];
        const ArtfMeshData* m = &scene->meshes.items[index];
        MeshRange* range = &v->meshes.items[index];
        if (range->deformVersion == m->deformVersion) continue;
        const VkDeviceSize size = (VkDeviceSize)m->vertexCount * sizeof(ArtfPackedVertex);
        memcpy((char*)s->posed.mapped + at, m->vertices, size);
        const BufferCopy copy = {at, (VkDeviceSize)range->vertexOffset * sizeof(ArtfPackedVertex), size};
        artfVecPush(&v->posedCopies, copy);
        at += size;
        range->deformVersion = m->deformVersion;
    }
    return true;
}

/* What casts shadows, by mesh: plain ones and then those with holes, each with its draw command. It changes
 * only with the scene. What the driver reads of it is this slot's. */
void bindStorage(Vulkan* v, Slot* s, uint32_t binding, const GpuBuffer* b) {
    const DescriptorBufferInfo info = {b->buffer, 0, ~(VkDeviceSize)0};
    const WriteDescriptorSet w = {ST_WRITE_DESCRIPTOR_SET, NULL, s->set, binding, 0, 1, DESCRIPTOR_STORAGE, NULL, &info, NULL};
    v->vkUpdateDescriptorSets(v->device, 1, &w, 0, NULL);
}

bool syncShadowList(Vulkan* v, Slot* s, const ArtfScene* scene, uint32_t views) {
    const uint32_t meshes = (uint32_t)v->meshes.len;
    if (s->shadowInstances == scene->tracedVersion && s->shadowStructure == scene->structureVersion && s->shadowMaterials == scene->materialVersion && s->shadowScene == scene->id && s->shadowMeshes == meshes && s->shadowViews == views && s->shadowList.buffer) return true;
    /* room for all that casts in every view; what is really there, the shader says */
    const VkDeviceSize listBytes = ((VkDeviceSize)scene->instances.len + 1) * 8 * views, commandBytes = ((VkDeviceSize)meshes * 40 + 40) * views;
    const VkDeviceSize casterBytes = ((VkDeviceSize)scene->instances.len + 2) * 16;
    if (listBytes > s->shadowList.size) {
        bufferFree(v, &s->shadowList);
        if (!bufferNew(v, &s->shadowList, listBytes * 2, BUFFER_STORAGE, false)) return false;
        bindStorage(v, s, 26, &s->shadowList);
    }
    if (casterBytes > s->shadowCasters.size) {
        bufferFree(v, &s->shadowCasters);
        if (!bufferNew(v, &s->shadowCasters, casterBytes * 2, BUFFER_STORAGE, true)) return false;
        bindStorage(v, s, 38, &s->shadowCasters);
    }
    if (commandBytes > s->shadowCommands.size) {
        bufferFree(v, &s->shadowCommands);
        bufferFree(v, &s->shadowViewCommands);
        if (!bufferNew(v, &s->shadowCommands, commandBytes * 2, BUFFER_TRANSFER_SRC, true)) return false;
        if (!bufferNew(v, &s->shadowViewCommands, commandBytes * 2, BUFFER_INDIRECT | BUFFER_STORAGE | BUFFER_TRANSFER_DST, false)) return false;
        bindStorage(v, s, 39, &s->shadowViewCommands);
    }
    /* how many of each, then where each starts */
    artfVecReserve(&v->copyStarts, (size_t)meshes * 2 + 1);
    uint32_t* counts = v->copyStarts.items;
    memset(counts, 0, ((size_t)meshes * 2 + 1) * 4);
#define KIND(d) (scene->materials.items[(d)->material].flags & ARTF_MATERIAL_MASK ? meshes : 0)
    for (size_t i = 0; i < scene->instances.len; ++i) {
        const ArtfInstanceData* d = &scene->instances.items[i];
        if (!d->translucent) ++counts[KIND(d) + artfMeshHeld(scene, d->mesh)];
    }
    uint32_t* commands = s->shadowCommands.mapped;
    uint32_t at = 0;
    for (uint32_t i = 0; i < meshes * 2; ++i) {
        const uint32_t count = counts[i];
        counts[i] = at;
        at += count;
    }
    /* each view with its own stretch of the list, and nothing in its commands yet */
    for (uint32_t view = 0; view < views; ++view)
        for (uint32_t i = 0; i < meshes * 2; ++i) {
            const MeshRange* m = &v->meshes.items[i % meshes];
            const uint32_t command[5] = {m->indexCount, 0, m->firstIndex, (uint32_t)m->vertexOffset, view * at + counts[i]};
            memcpy(commands + ((size_t)view * meshes * 2 + i) * 5, command, sizeof command);
        }
    uint32_t* casters = s->shadowCasters.mapped;
    uint32_t n = 0;
    for (size_t i = 0; i < scene->instances.len; ++i) {
        const ArtfInstanceData* d = &scene->instances.items[i];
        if (d->translucent) continue;
        uint32_t* entry = casters + ((size_t)n + 1) * 4;
        const uint32_t held = artfMeshHeld(scene, d->mesh);
        entry[0] = (uint32_t)i;
        entry[1] = held;
        entry[2] = KIND(d) + held;
        ++n;
    }
#undef KIND
    casters[0] = n;
    casters[1] = meshes * 2;
    s->shadowCasterCount = n;
    s->shadowViews = views;
    s->shadowInstances = scene->tracedVersion;
    s->shadowStructure = scene->structureVersion;
    s->shadowMaterials = scene->materialVersion;
    s->shadowScene = scene->id;
    s->shadowMeshes = meshes;
    return true;
}

void shadowDraw(Vulkan* v, VkCommandBuffer c, Slot* s, VkPipeline pipeline, uint32_t first, uint32_t count) {
    v->vkCmdBindPipeline(c, 0, pipeline);
    if (v->multiDraw) {
        v->vkCmdDrawIndexedIndirect(c, s->shadowViewCommands.buffer, (VkDeviceSize)first * 20, count, 20);
    } else {
        for (uint32_t i = 0; i < count; ++i) v->vkCmdDrawIndexedIndirect(c, s->shadowViewCommands.buffer, (VkDeviceSize)(first + i) * 20, 1, 20);
    }
}

/* The maps that changed, each in its square of the atlas, with only what can reach it. */
void drawShadows(Vulkan* v, VkCommandBuffer c, Slot* s, const ArtfShadowFrame* shadows, uint32_t views, uint64_t changed) {
    if (!changed) return;
    /* each view's commands start empty, and the shader fills them */
    const VkFlags reading = STAGE_DRAW_INDIRECT | STAGE_VERTEX_SHADER;
    const BufferCopy all = {0, 0, (VkDeviceSize)views * v->meshes.len * 40};
    memoryBarrier(v, c, reading, STAGE_TRANSFER, ACCESS_INDIRECT | ACCESS_SHADER_READ, ACCESS_TRANSFER_WRITE);
    v->vkCmdCopyBuffer(c, s->shadowCommands.buffer, s->shadowViewCommands.buffer, 1, &all);
    memoryBarrier(v, c, STAGE_TRANSFER | reading, STAGE_COMPUTE, ACCESS_TRANSFER_WRITE | ACCESS_SHADER_READ, ACCESS_SHADER_READ | ACCESS_SHADER_WRITE);
    v->vkCmdBindPipeline(c, 1, v->shadowCullPipeline);
    v->vkCmdBindDescriptorSets(c, 1, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
    v->vkCmdDispatch(c, (s->shadowCasterCount + 63) / 64, views, 1);
    memoryBarrier(v, c, STAGE_COMPUTE, reading, ACCESS_SHADER_WRITE, ACCESS_INDIRECT | ACCESS_SHADER_READ);

    const Rect2D area = {{0, 0}, {(uint32_t)v->shadowWidth, (uint32_t)v->shadowHeight}};
    RenderPassBeginInfo pass = {ST_RENDER_PASS_BEGIN, NULL, v->shadowPass, v->shadowFramebuffer, area, 0, NULL};
    v->vkCmdBeginRenderPass(c, &pass, 0);
    const VkDeviceSize zero = 0;
    v->vkCmdBindVertexBuffers(c, 0, 1, &v->vertices.buffer, &zero);
    v->vkCmdBindIndexBuffer(c, v->indices.buffer, 0, 1 /* 32 bits */);
    v->vkCmdBindPipeline(c, 0, v->shadowPipeline);
    v->vkCmdBindDescriptorSets(c, 0, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
    const uint32_t meshes = (uint32_t)v->meshes.len;
    for (uint32_t i = 0; i < views; ++i) {
        if (!(changed >> i & 1)) continue;
        const float* info = shadows->views[i].info;
        const float side = shadows->views[i].size[0];
        const Viewport viewport = {info[0], info[1], side, side, 0, 1};
        const Rect2D square = {{(int32_t)info[0], (int32_t)info[1]}, {(uint32_t)side, (uint32_t)side}};
        v->vkCmdSetViewport(c, 0, 1, &viewport);
        v->vkCmdSetScissor(c, 0, 1, &square);
        ClearAttachment clear;
        memset(&clear, 0, sizeof clear);
        clear.aspect = ASPECT_DEPTH;
        clear.value.depthStencil.depth = 1.f;
        const ClearRect rect = {square, 0, 1};
        v->vkCmdClearAttachments(c, 1, &clear, 1, &rect);
        v->vkCmdPushConstants(c, v->pipelineLayout, SHADER_VERTEX, 4, 4, &i);
        shadowDraw(v, c, s, v->shadowPipeline, i * meshes * 2, meshes);
        shadowDraw(v, c, s, v->shadowHolesPipeline, i * meshes * 2 + meshes, meshes);
    }
    v->vkCmdEndRenderPass(c);
}

/* ---- emitters of many ---- */

/* Which there are, and how many particles in all. */
uint32_t swarmList(Vulkan* v, const ArtfScene* scene) {
    v->swarms.len = 0;
    const uint32_t count = artfSceneSwarms(scene, NULL, 0);
    if (!count) return 0;
    artfVecReserve(&v->swarms, count);
    v->swarms.len = artfSceneSwarms(scene, v->swarms.items, count);
    uint32_t places = 0;
    for (size_t k = 0; k < v->swarms.len; ++k) places += artfSwarmPlaces(v->swarms.items[k]);
    return places;
}

bool swarmPipelineMake(Vulkan* v) {
    if (v->swarmPipeline) return true;
    size_t size;
    uint32_t* code = artfShaderSwarm(&size);
    VkShaderModule module = shaderModule(v, code, size);
    if (!module) return false;
    const ComputePipelineCreateInfo info = {ST_COMPUTE_PIPELINE, NULL, 0, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_COMPUTE, module, "main", NULL}, v->pipelineLayout, 0, -1};
    const bool ok = v->vkCreateComputePipelines(v->device, v->pipelineCache, 1, &info, NULL, &v->swarmPipeline) == VK_OK;
    v->vkDestroyShaderModule(v->device, module, NULL);
    return ok;
}

/* Where each emitter's particles are: when they change, each that was there keeps what it had, in its new place; and
 * this slot's rows: the steps each has not seen, and how to face the eye. */
bool syncSwarms(Vulkan* v, Slot* s, const ArtfScene* scene) {
    const size_t count = v->swarms.len;
    if (!count) return true;
    if (!swarmPipelineMake(v)) return false;
    const bool same = v->swarmScene == scene->id && v->swarmsBefore.len == count && !memcmp(v->swarmsBefore.items, v->swarms.items, count * sizeof *v->swarms.items);
    if (!same) {
        v->vkDeviceWaitIdle(v->device);
        GpuBuffer state;
        if (!bufferNew(v, &state, (VkDeviceSize)v->swarmPlaces * ARTF_SWARM_FLOATS * 4, BUFFER_STORAGE | BUFFER_TRANSFER_DST | BUFFER_TRANSFER_SRC, false)) return false;
        ARTF_VEC(uint64_t) seen = {0};
        ARTF_VEC(BufferCopy) copies = {0};
        uint32_t at = 0;
        for (size_t k = 0; k < count; ++k) {
            uint64_t had = UINT64_MAX;
            for (size_t j = 0; j < v->swarmsBefore.len && v->swarmScene == scene->id; ++j)
                if (v->swarmsBefore.items[j] == v->swarms.items[k] && v->swarmSeen.items[j] != UINT64_MAX) {
                    had = v->swarmSeen.items[j];
                    const BufferCopy copy = {(VkDeviceSize)v->swarmAt.items[j] * ARTF_SWARM_FLOATS * 4, (VkDeviceSize)at * ARTF_SWARM_FLOATS * 4, (VkDeviceSize)artfSwarmPlaces(v->swarms.items[k]) * ARTF_SWARM_FLOATS * 4};
                    artfVecPush(&copies, copy);
                }
            artfVecPush(&seen, had);
            at += artfSwarmPlaces(v->swarms.items[k]);
        }
        if (copies.len) {
            VkCommandBuffer c = oneShotBegin(v);
            v->vkCmdCopyBuffer(c, v->swarmState.buffer, state.buffer, (uint32_t)copies.len, copies.items);
            if (!oneShotEnd(v, c)) return false;
        }
        artfVecFree(&copies);
        bufferFree(v, &v->swarmState);
        v->swarmState = state;
        for (int i = 0; i < SLOTS; ++i) bindStorage(v, &v->slots[i], 66, &v->swarmState);
        v->swarmSeen.len = 0;
        v->swarmAt.len = 0;
        v->swarmsBefore.len = 0;
        at = 0;
        for (size_t k = 0; k < count; ++k) {
            artfVecPush(&v->swarmSeen, seen.items[k]);
            artfVecPush(&v->swarmAt, at);
            artfVecPush(&v->swarmsBefore, v->swarms.items[k]);
            at += artfSwarmPlaces(v->swarms.items[k]);
        }
        artfVecFree(&seen);
        v->swarmScene = scene->id;
    }
    /* the rows: where each emitter's start, then each one's */
    size_t rows = count;
    for (size_t k = 0; k < count; ++k) {
        const ArtfSwarm* w = v->swarms.items[k];
        const uint64_t seen = v->swarmSeen.items[k];
        const uint64_t from = seen == UINT64_MAX || seen < artfSwarmOldest(w) ? artfSwarmOldest(w) : seen;
        rows += 6 + (size_t)(artfSwarmStepCount(w) - from) * ARTF_SWARM_ROWS;
    }
    if (rows * 16 > s->swarmRows.size) {
        bufferFree(v, &s->swarmRows);
        if (!bufferNew(v, &s->swarmRows, rows * 32, BUFFER_STORAGE, true)) return false;
        bindStorage(v, s, 67, &s->swarmRows);
    }
    float* out = s->swarmRows.mapped;
    uint32_t row = (uint32_t)count;
    for (size_t k = 0; k < count; ++k) {
        ArtfSwarm* w = v->swarms.items[k];
        const uint64_t seen = v->swarmSeen.items[k];
        const bool fresh = seen == UINT64_MAX || seen < artfSwarmOldest(w);
        const uint64_t from = fresh ? artfSwarmOldest(w) : seen, steps = artfSwarmStepCount(w) - from;
        memcpy(out + k * 4, &row, 4);
        float* at = out + (size_t)row * 4;
        const uint32_t head[8] = {(uint32_t)steps, artfSwarmPlaces(w), v->swarmAt.items[k], fresh ? 1u : 0u,
                                  v->instanceCapacity * 2 + v->swarmAt.items[k], swarmCopiesAt(v) + v->swarmAt.items[k], artfMeshIndexOf(scene, scene->particleQuad), 0};
        memcpy(at, head, sizeof head);
        float face[ARTF_SWARM_ROWS * 4];
        artfSwarmFaceRows(scene, w, face);
        memcpy(at + 8, face, 16 * sizeof *face);
        for (uint64_t i = 0; i < steps; ++i) memcpy(at + 24 + i * ARTF_SWARM_ROWS * 4, artfSwarmStepRows(w, from + i), ARTF_SWARM_ROWS * 16);
        row += 6 + (uint32_t)steps * ARTF_SWARM_ROWS;
        v->swarmSeen.items[k] = artfSwarmStepCount(w);
    }
    return true;
}

/* Each emitter's particles, moved and turned to the eye, into this slot's instances and copies. */
void runSwarms(Vulkan* v, VkCommandBuffer c, Slot* s) {
    if (!v->swarms.len) return;
    memoryBarrier(v, c, STAGE_COMPUTE | STAGE_VERTEX_SHADER, STAGE_COMPUTE, ACCESS_SHADER_READ | ACCESS_SHADER_WRITE, ACCESS_SHADER_READ | ACCESS_SHADER_WRITE);
    v->vkCmdBindPipeline(c, 1, v->swarmPipeline);
    v->vkCmdBindDescriptorSets(c, 1, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
    for (uint32_t k = 0; k < (uint32_t)v->swarms.len; ++k) {
        v->vkCmdPushConstants(c, v->pipelineLayout, SHADER_COMPUTE, 0, 4, &k);
        v->vkCmdDispatch(c, (artfSwarmPlaces(v->swarms.items[k]) + 63) / 64, 1, 1);
    }
    memoryBarrier(v, c, STAGE_COMPUTE, STAGE_VERTEX_SHADER, ACCESS_SHADER_WRITE, ACCESS_SHADER_READ);
}

/* All of them, after the other layers, in one draw of the quad: each over what is there, in their order. */
void drawSwarms(Vulkan* v, VkCommandBuffer c, Slot* s, const ArtfScene* scene) {
    if (!v->swarms.len || !scene->particleQuad) return;
    const MeshRange* quad = &v->meshes.items[artfMeshIndexOf(scene, scene->particleQuad)];
    const VkDeviceSize zero = 0;
    v->vkCmdBindVertexBuffers(c, 0, 1, &v->vertices.buffer, &zero);
    v->vkCmdBindIndexBuffer(c, v->indices.buffer, 0, 1 /* 32 bits */);
    v->vkCmdBindPipeline(c, 0, v->particlePipeline);
    v->vkCmdBindDescriptorSets(c, 0, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
    v->vkCmdDrawIndexed(c, quad->indexCount, v->swarmPlaces, quad->firstIndex, quad->vertexOffset, swarmCopiesAt(v));
}

/* What shows what is behind it, the farthest first: its depth among what is there, and then its colors
 * where that depth is its own, so that only its nearest surface shows. */
void drawLayers(Vulkan* v, VkCommandBuffer c, const ArtfScene* scene, uint32_t count) {
#define PARTICLE(k) (scene->instances.items[v->layers.items[k].instance].flags & ARTF_INSTANCE_PARTICLE)
    if (v->base.view == ARTF_VIEW_OVERDRAW) {
        /* (each once, both sides: as they count on the CPU) */
        v->vkCmdBindPipeline(c, 0, v->overdrawPipelines[1]);
        for (uint32_t i = 0; i < count; ++i) {
            const MeshRange* m = &v->meshes.items[v->layers.items[i].mesh];
            v->vkCmdDrawIndexed(c, m->indexCount, 1, m->firstIndex, m->vertexOffset, layersAt(v) + i);
        }
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const MeshRange* m = &v->meshes.items[v->layers.items[i].mesh];
        if (PARTICLE(i)) {
            /* particles one after another, of one quad: one draw for them all, in their order */
            uint32_t end = i + 1;
            while (end < count && PARTICLE(end) && v->layers.items[end].mesh == v->layers.items[i].mesh) ++end;
            v->vkCmdBindPipeline(c, 0, v->particlePipeline);
            v->vkCmdDrawIndexed(c, m->indexCount, end - i, m->firstIndex, m->vertexOffset, layersAt(v) + i);
            i = end - 1;
            continue;
        }
        v->vkCmdBindPipeline(c, 0, v->layerDepthPipeline);
        v->vkCmdDrawIndexed(c, m->indexCount, 1, m->firstIndex, m->vertexOffset, layersAt(v) + i);
        v->vkCmdBindPipeline(c, 0, v->layerPipeline);
        v->vkCmdDrawIndexed(c, m->indexCount, 1, m->firstIndex, m->vertexOffset, layersAt(v) + i);
    }
#undef PARTICLE
}

/* ---- lights past the frame's own ---- */

void lightDescriptors(Vulkan* v, Slot* s) {
    if (!s->probesBound) s->probesBound = s->probes.buffer;
    const DescriptorBufferInfo lights = {s->extraLights.buffer, 0, ~(VkDeviceSize)0}, lists = {s->tileLights.buffer, 0, ~(VkDeviceSize)0}, probes = {s->probesBound, 0, ~(VkDeviceSize)0};
    const DescriptorBufferInfo decals = {s->decals.buffer, 0, ~(VkDeviceSize)0};
    const WriteDescriptorSet writes[4] = {{ST_WRITE_DESCRIPTOR_SET, NULL, s->set, 60, 0, 1, DESCRIPTOR_STORAGE, NULL, &lights, NULL},
                                          {ST_WRITE_DESCRIPTOR_SET, NULL, s->set, 61, 0, 1, DESCRIPTOR_STORAGE, NULL, &lists, NULL},
                                          {ST_WRITE_DESCRIPTOR_SET, NULL, s->set, 62, 0, 1, DESCRIPTOR_STORAGE, NULL, &probes, NULL},
                                          {ST_WRITE_DESCRIPTOR_SET, NULL, s->set, 63, 0, 1, DESCRIPTOR_STORAGE, NULL, &decals, NULL}};
    v->vkUpdateDescriptorSets(v->device, 4, writes, 0, NULL);
}

/* What this slot's set says the probes are: 62, what pixels and a step read; 70, what a step writes (0: left as it is). */
void probeDescriptors(Vulkan* v, Slot* s, VkBuffer now, VkBuffer next) {
    const DescriptorBufferInfo a = {now, 0, ~(VkDeviceSize)0}, b = {next, 0, ~(VkDeviceSize)0};
    const WriteDescriptorSet writes[2] = {{ST_WRITE_DESCRIPTOR_SET, NULL, s->set, 62, 0, 1, DESCRIPTOR_STORAGE, NULL, &a, NULL},
                                          {ST_WRITE_DESCRIPTOR_SET, NULL, s->set, 70, 0, 1, DESCRIPTOR_STORAGE, NULL, &b, NULL}};
    v->vkUpdateDescriptorSets(v->device, next ? 2 : 1, writes, 0, NULL);
    s->probesBound = now;
    if (next) s->probesNextBound = next;
}

bool artfVulkanProbesOnCpu = false;

/* How often the GPU's probes go back to the scene, in their steps: a renderer that comes after this one (when the device
 * is lost, or a window moves to another GPU) goes on from what it kept, half a second before at sixty frames. */
#define PROBES_KEPT_EVERY 30

/* Light that bounced. On the GPU: the step's pipeline (artfShaderProbes), made for what the probes are, and the buffers
 * all frames share; *gather says this frame takes a step (with the trees or the hardware's structures, which the caller
 * keeps up to date). Or, for tests, a step on this thread's workers, and what they hold into this slot's buffer. */
bool syncIndirect(Vulkan* v, Slot* s, ArtfScene* scene, const ArtfFrameData* frame, bool* gather) {
    *gather = false;
    if (frame->indirect[0][3] <= 0 || scene->trace.desc.mode != ARTF_TRACE_NONE) return true;
    const ArtfIndirect* g = &scene->indirect;
    const VkDeviceSize need = (VkDeviceSize)artfIndirectCount(g) * ARTF_PROBE_FLOATS * 4;
    if (artfVulkanProbesOnCpu) {
        if (!v->pool) v->pool = artfPoolNew(0);
        artfSceneIndirectStep(scene, frame, v->pool);
        if (s->probes.size < need) {
            bufferFree(v, &s->probes);
            if (!bufferNew(v, &s->probes, need, BUFFER_STORAGE, true)) return false;
            s->probesBound = 0;
        }
        if (s->probesBound != s->probes.buffer) probeDescriptors(v, s, s->probes.buffer, 0);
        memcpy(s->probes.mapped, scene->indirect.probes, (size_t)need);
        v->probeScene = 0; /* (the GPU's start over, if it takes them back) */
        return true;
    }
    if (v->probesNow.size < need) {
        /* (both slots' sets name them: neither may be in flight) */
        v->vkDeviceWaitIdle(v->device);
        bufferFree(v, &v->probesNow);
        bufferFree(v, &v->probesNext);
        if (!bufferNew(v, &v->probesNow, need, BUFFER_STORAGE | BUFFER_TRANSFER_SRC | BUFFER_TRANSFER_DST, false)) return false;
        if (!bufferNew(v, &v->probesNext, need, BUFFER_STORAGE | BUFFER_TRANSFER_SRC | BUFFER_TRANSFER_DST, false)) return false;
        for (int i = 0; i < SLOTS; ++i) probeDescriptors(v, &v->slots[i], v->probesNow.buffer, v->probesNext.buffer);
        v->probeScene = 0;
    }
    if (s->probesBound != v->probesNow.buffer || s->probesNextBound != v->probesNext.buffer) probeDescriptors(v, s, v->probesNow.buffer, v->probesNext.buffer);
    /* (what the shader is made for: how many rays, how much a probe keeps, walls, the hardware, materials past glTF's
     * core; the scene's materials in C retire it when they change) */
    const int rays = g->desc.rays > 0 ? g->desc.rays : 64;
    const float keep = g->desc.hysteresis > 0 ? artfClampf(g->desc.hysteresis, 0, .999f) : .9f;
    const bool walls = !g->desc.throughWalls, hardware = v->rayQuery, more = scene->moreMaterials > 0;
    uint32_t keepBits;
    memcpy(&keepBits, &keep, 4);
    const uint64_t key = (uint64_t)keepBits << 32 | (uint64_t)rays << 3 | (walls ? 1u : 0u) | (hardware ? 2u : 0u) | (more ? 4u : 0u);
    if (v->probePipeline && (v->probeKey != key || v->probeGraphs != scene->graphs.len || v->probeGraphScene != scene->id)) {
        pipelineRetire(v, v->probePipeline);
        v->probePipeline = 0;
    }
    if (!v->probePipeline) {
        size_t size;
        uint32_t* code = artfShaderProbes(&size, v->maxTextures, scene->graphs.items, (uint32_t)scene->graphs.len, hardware, more, walls, rays, keep);
        VkShaderModule module = shaderModule(v, code, size);
        if (!module) return false;
        const ComputePipelineCreateInfo info = {ST_COMPUTE_PIPELINE, NULL, 0, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_COMPUTE, module, "main", NULL}, v->pipelineLayout, 0, -1};
        const bool ok = v->vkCreateComputePipelines(v->device, v->pipelineCache, 1, &info, NULL, &v->probePipeline) == VK_OK;
        v->vkDestroyShaderModule(v->device, module, NULL);
        if (!ok) return false;
        v->probeKey = key;
        v->probeGraphs = (uint32_t)scene->graphs.len;
        v->probeGraphScene = scene->id;
    }
    if (v->probeScene != scene->id || v->probeSerial != g->serial) {
        /* (probes new to this renderer: from what the scene kept of them, a renderer's before this one or the CPU's;
         * from nothing, as the CPU's start, where it kept none) */
        v->probeScene = scene->id;
        v->probeSerial = g->serial;
        v->probeFrames = 0;
        v->probeClear = true;
        if (g->frames && g->probes) {
            GpuBuffer up;
            if (!bufferNew(v, &up, need, BUFFER_TRANSFER_SRC, true)) {
                bufferFree(v, &up);
                return false;
            }
            memcpy(up.mapped, g->probes, (size_t)need);
            VkCommandBuffer c = oneShotBegin(v);
            const BufferCopy all = {0, 0, need};
            v->vkCmdCopyBuffer(c, up.buffer, v->probesNow.buffer, 1, &all);
            memoryBarrier(v, c, STAGE_TRANSFER, STAGE_FRAGMENT | STAGE_COMPUTE, ACCESS_TRANSFER_WRITE, ACCESS_SHADER_READ);
            const bool ok = oneShotEnd(v, c);
            bufferFree(v, &up);
            if (!ok) return false;
            v->probeFrames = g->frames;
            v->probeClear = false;
        }
    }
    *gather = true;
    return true;
}

/* A step of the probes, in the frame's commands: from what they held, which the frames before read, into the next;
 * then that into what this frame's pixels read. */
void gatherProbes(Vulkan* v, VkCommandBuffer c, Slot* s, const ArtfScene* scene) {
    const ArtfIndirect* g = &scene->indirect;
    const VkFlags readWrite = ACCESS_SHADER_READ | ACCESS_SHADER_WRITE;
    if (v->probeClear) {
        memoryBarrier(v, c, STAGE_FRAGMENT | STAGE_COMPUTE | STAGE_TRANSFER, STAGE_TRANSFER, readWrite | ACCESS_TRANSFER_WRITE, ACCESS_TRANSFER_WRITE);
        v->vkCmdFillBuffer(c, v->probesNow.buffer, 0, ~(VkDeviceSize)0, 0);
        v->probeClear = false;
    }
    memoryBarrier(v, c, STAGE_TRANSFER | STAGE_FRAGMENT | STAGE_COMPUTE, STAGE_COMPUTE, ACCESS_TRANSFER_WRITE | readWrite, readWrite);
    v->vkCmdBindPipeline(c, 1, v->probePipeline);
    v->vkCmdBindDescriptorSets(c, 1, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
    v->vkCmdPushConstants(c, v->pipelineLayout, SHADER_COMPUTE, 0, 4, &v->probeFrames);
    v->vkCmdDispatch(c, (uint32_t)(g->dims[0] * g->dims[1]), (uint32_t)(g->dims[2] * (g->cascades > 0 ? g->cascades : 1)), 1); /* (each volume's cells: cascades) */
    memoryBarrier(v, c, STAGE_COMPUTE | STAGE_FRAGMENT, STAGE_TRANSFER, ACCESS_SHADER_WRITE, ACCESS_TRANSFER_READ | ACCESS_TRANSFER_WRITE);
    const BufferCopy all = {0, 0, (VkDeviceSize)artfIndirectCount(g) * ARTF_PROBE_FLOATS * 4};
    v->vkCmdCopyBuffer(c, v->probesNext.buffer, v->probesNow.buffer, 1, &all);
    memoryBarrier(v, c, STAGE_TRANSFER, STAGE_FRAGMENT | STAGE_COMPUTE, ACCESS_TRANSFER_WRITE, ACCESS_SHADER_READ);
    ++v->probeFrames;
    if (v->probeFrames % PROBES_KEPT_EVERY == 0 && g->probes) {
        /* (and, now and then, to the scene: see Slot's probesBack) */
        if (s->probesBack.size < all.size) {
            bufferFree(v, &s->probesBack);
            if (!bufferNewFor(v, &s->probesBack, all.size, BUFFER_TRANSFER_DST, BUFFER_READ_BY_CPU)) {
                bufferFree(v, &s->probesBack);
                return;
            }
        }
        /* (after the copy that made probesNow what it is) */
        memoryBarrier(v, c, STAGE_TRANSFER, STAGE_TRANSFER, ACCESS_TRANSFER_WRITE, ACCESS_TRANSFER_READ);
        v->vkCmdCopyBuffer(c, v->probesNow.buffer, s->probesBack.buffer, 1, &all);
        memoryBarrier(v, c, STAGE_TRANSFER, STAGE_HOST, ACCESS_TRANSFER_WRITE, ACCESS_HOST_READ);
        s->probesBackHeld = true;
        s->probesBackScene = scene->id;
        s->probesBackSerial = g->serial;
        s->probesBackFrames = v->probeFrames;
    }
}

bool artfVulkanProbesRead(ArtfRenderer* r, float* out, size_t floats) {
    Vulkan* v = (Vulkan*)r;
    if (v->lost || !v->probesNow.buffer || (VkDeviceSize)floats * 4 > v->probesNow.size) return false;
    if (v->vkDeviceWaitIdle(v->device) != VK_OK) return false;
    GpuBuffer read;
    if (!bufferNewFor(v, &read, (VkDeviceSize)floats * 4, BUFFER_TRANSFER_DST, BUFFER_READ_BY_CPU)) {
        bufferFree(v, &read);
        return false;
    }
    VkCommandBuffer c = oneShotBegin(v);
    const BufferCopy all = {0, 0, (VkDeviceSize)floats * 4};
    v->vkCmdCopyBuffer(c, v->probesNow.buffer, read.buffer, 1, &all);
    memoryBarrier(v, c, STAGE_TRANSFER, STAGE_HOST, ACCESS_TRANSFER_WRITE, ACCESS_HOST_READ);
    const bool ok = oneShotEnd(v, c);
    if (ok) memcpy(out, read.mapped, floats * 4);
    bufferFree(v, &read);
    return ok;
}

/* Into this slot's buffers: the lights, and each cluster's list (its fence was waited for); into the frame, its
 * slices of depth. */
bool syncLights(Vulkan* v, Slot* s, const ArtfScene* scene, ArtfFrameData* frame) {
    v->lightsOmitted = 0;
    if (frame->eye[2][3] <= 0) return true;
    /* (made in main memory, then copied: what the GPU reads is slow to read back, as the lists' counts are) */
    if (!v->lightScratch) v->lightScratch = artfMalloc(ARTF_MAX_LIGHTS * sizeof(ArtfLightData));
    const uint32_t count = artfSceneExtraLights(scene, frame, v->lightScratch);
    artfSceneLightSlices(frame, v->lightScratch, count);
    const VkDeviceSize need = (VkDeviceSize)artfSceneLightTiles(frame, v->sceneWidth, v->sceneHeight, v->lightScratch, count, NULL, &v->listScratch, &v->listScratchSize, &v->lightsOmitted) * 4;
    if (s->tileLights.size < need) {
        /* (a quarter more, so a list that grows a little does not make it again) */
        bufferFree(v, &s->tileLights);
        if (!bufferNew(v, &s->tileLights, need + need / 4, BUFFER_STORAGE, true)) return false;
        lightDescriptors(v, s);
    }
    memcpy(s->extraLights.mapped, v->lightScratch, (size_t)count * sizeof(ArtfLightData));
    memcpy(s->tileLights.mapped, v->listScratch, (size_t)need);
    return true;
}

/* ---- what moves: the surface's second word, and what the vertices were ---- */

/* Room for what the vertex buffer held a frame ago, as large as it is. */
bool pastVerticesRoom(Vulkan* v) {
    if (v->pastVertices.size >= v->vertices.size) return true;
    v->vkDeviceWaitIdle(v->device);
    bufferFree(v, &v->pastVertices);
    if (!bufferNew(v, &v->pastVertices, v->vertices.size, BUFFER_STORAGE | BUFFER_TRANSFER_DST, false)) return false;
    for (int i = 0; i < SLOTS; ++i) bindStorage(v, &v->slots[i], 68, &v->pastVertices);
    return true;
}

/* Before poses move them: the vertices of what bends, as the frame before drew them. */
void pastVerticesCopy(Vulkan* v, VkCommandBuffer c, const ArtfScene* scene) {
    v->pastCopies.len = 0;
    for (size_t i = 0; i < scene->deforming.len; ++i) {
        const uint32_t index = scene->deforming.items[i];
        const MeshRange* range = &v->meshes.items[index];
        const VkDeviceSize at = (VkDeviceSize)range->vertexOffset * sizeof(ArtfPackedVertex);
        const BufferCopy copy = {at, at, (VkDeviceSize)scene->meshes.items[index].vertexCount * sizeof(ArtfPackedVertex)};
        /* (the vertex buffer may have been made again smaller than this one: both bound it) */
        if (copy.size && at + copy.size <= v->pastVertices.size && at + copy.size <= v->vertices.size) artfVecPush(&v->pastCopies, copy);
    }
    if (!v->pastCopies.len) return;
    /* (the frame before read both; the bones write the vertices after this) */
    memoryBarrier(v, c, STAGE_VERTEX_INPUT | STAGE_VERTEX_SHADER, STAGE_TRANSFER, ACCESS_VERTEX_READ | ACCESS_SHADER_READ, ACCESS_TRANSFER_READ | ACCESS_TRANSFER_WRITE);
    v->vkCmdCopyBuffer(c, v->vertices.buffer, v->pastVertices.buffer, (uint32_t)v->pastCopies.len, v->pastCopies.items);
    memoryBarrier(v, c, STAGE_TRANSFER, STAGE_COMPUTE | STAGE_TRANSFER | STAGE_VERTEX_SHADER, ACCESS_TRANSFER_WRITE, ACCESS_SHADER_READ | ACCESS_SHADER_WRITE | ACCESS_TRANSFER_WRITE);
}

/* The meshlets view's table (binding 65): of each mesh, where its triangles start in it; then of each triangle, its
 * meshlet, numbered as the CPU numbers them. A triangle is known by PrimitiveId, which counts from its draw's first:
 * so the meshlets view draws whole meshes. Made when the view first asks, and again when meshes come or go. */
bool meshletIdsUp(Vulkan* v, const ArtfScene* scene) {
    const uint64_t key = (scene->id * 1000003u) ^ ((uint64_t)scene->meshes.len * 7919u) ^ (scene->meshRemovals * 31u) ^ (scene->lodVersion << 40);
    if (v->meshletIds.buffer && key == v->meshletIdsKey) return true;
    v->vkDeviceWaitIdle(v->device);
    size_t words = scene->meshes.len + 1;
    for (size_t i = 0; i < scene->meshes.len; ++i) words += scene->meshes.items[i].indexCount / 3;
    if ((VkDeviceSize)words * 4 > v->meshletIds.size) {
        bufferFree(v, &v->meshletIds);
        if (!bufferNew(v, &v->meshletIds, (VkDeviceSize)words * 8, BUFFER_STORAGE, true)) return false;
        for (int i = 0; i < SLOTS; ++i) bindStorage(v, &v->slots[i], 65, &v->meshletIds);
    }
    uint32_t* out = v->meshletIds.mapped;
    uint32_t at = (uint32_t)scene->meshes.len + 1;
    for (size_t i = 0; i < scene->meshes.len; ++i) {
        const ArtfMeshData* m = &scene->meshes.items[i];
        out[i] = at;
        for (uint32_t k = 0; k < ARTF_MESH_WHOLE_MESHLETS(m); ++k)
            for (uint32_t t = 0; t < m->meshlets[k].triangleCount; ++t) out[at + m->meshlets[k].triangleOffset + t] = k;
        at += m->indexCount / 3;
    }
    v->meshletIdsKey = key;
    return true;
}

/* What was being made off the frame, where it is done: taken (see shaderJobEnd). */
bool pipelinesTake(Vulkan* v, const ArtfScene* scene) {
    bool taken = false;
    if (v->shaderJob && (!v->shaderJob->task || artfTaskDone(v->shaderJob->task))) {
        if (!shaderJobEnd(v, scene, true)) return false;
        taken = true;
    }
    for (int k = 0; k < MAKE_COMPUTE; ++k)
        if (v->computeJobs[k] && (!v->computeJobs[k]->task || artfTaskDone(v->computeJobs[k]->task))) {
            if (!computeJobEnd(v, scene, k, true)) return false;
            taken = true;
        }
    if (taken && !v->shaderJob) {
        bool pending = false;
        for (int k = 0; k < MAKE_COMPUTE; ++k) pending = pending || v->computeJobs[k];
        if (!pending) artfEventSay(ARTF_EVENT_SHADERS_READY, v, 0, 0, "artful: what the scene asked for is made, and drawn from this frame");
    }
    return true;
}

/* The scene's pixel shaders and passes for want, wide (the surface's second word) and the scene's materials of its
 * own, against those there: made on a thread of their own if they differ, or now if wait or if those there were made
 * for another scene's materials. Until then, frames are drawn with those there. */
bool shadersFollow(Vulkan* v, const ArtfScene* scene, const Features* want, bool wide, bool wait) {
    const Features have = featuresOf(v);
    const bool graphs = graphsPrefix(v->graphCount, v->graphScene, scene);
    if (!memcmp(want, &have, sizeof have) && graphs && v->graphCount == scene->graphs.len && wide == v->surfaceWide) return true;
    if (wait || !graphs) {
        shaderJobEnd(v, scene, false);
        shaderJobStart(v, scene, want, wide, true);
        return shaderJobEnd(v, scene, true);
    }
    if (!v->shaderJob) shaderJobStart(v, scene, want, wide, false);
    return true;
}


/* The scene, as the pipelines there can draw it (vkDrawAs): what that changes of it, for the frame, is given back. */
/* A frame given up after its fence was reset (the GPU's memory ran out for what it needed): nothing submitted but the
 * fence's signal, and with a window, the image it took given back as it was; so the next frame of this slot does not wait
 * for ever, and the renderer goes on. */
void frameOwed(Vulkan* v) {
    Slot* s = v->owed;
    v->owed = NULL;
    const VkFlags waitStage = STAGE_COLOR;
    SubmitInfo submit = {ST_SUBMIT_INFO, NULL, 0, NULL, NULL, 0, NULL, 0, NULL};
    if (v->swapchain) {
        submit.waitCount = 1;
        submit.waits = &s->acquired;
        submit.waitStages = &waitStage;
        submit.signalCount = 1;
        submit.signals = &v->rendered[v->owedImage];
    }
    if (v->vkQueueSubmit(v->queue, 1, &submit, s->fence) != VK_OK) {
        v->lost = true;
        return;
    }
    ++v->frameNumber;
    if (v->swapchain) {
        const PresentInfo present = {ST_PRESENT_INFO, NULL, 1, &v->rendered[v->owedImage], 1, &v->swapchain, &v->owedImage, NULL};
        const VkResult result = v->vkQueuePresentKHR(v->queue, &present);
        if (result == VK_SUBOPTIMAL || result == VK_ERR_OUT_OF_DATE) v->targetsStale = true;
    }
}

bool vkDraw(ArtfRenderer* r, ArtfScene* scene) {
    const ArtfTraceMode mode = scene->trace.desc.mode;
    const bool traced = scene->shadows.traced, temporal = scene->post.screen.temporal;
    const float blur = scene->post.lens.motionBlur;
    Vulkan* v = (Vulkan*)r;
    v->memoryShort = false;
    bool drawn = vkDrawAs(v, scene);
    if (v->owed && !v->lost) frameOwed(v);
    v->owed = NULL;
    /* (a frame given up as the GPU's memory ran out: the picture is the one before, it was said, and the renderer goes
     * on, to try again the next frame; false is for a device lost) */
    if (!drawn && v->memoryShort && !v->lost) drawn = true;
    scene->trace.desc.mode = mode;
    scene->shadows.traced = traced;
    scene->post.screen.temporal = temporal;
    scene->post.lens.motionBlur = blur;
    return drawn;
}

bool vkDrawAs(Vulkan* v, ArtfScene* scene) {
    const double start = artfNowMs();
    if (v->lost) return false;
    if (v->base.renderScale != v->renderScale) {
        /* (pacing: the scene's targets at another size, the frame's as they were) */
        v->renderScale = v->base.renderScale;
        v->targetsStale = true;
    }
    /* what pipelines the scene asks for: where they are being made, this frame is drawn with those there */
    const bool wait = artfVulkanShadersWait || !v->frameNumber || v->warmed;
    v->warmed = false;
    if (!pipelinesTake(v, scene)) return false;
    if (scene->trace.desc.mode != ARTF_TRACE_NONE && !v->base.view) {
        /* (until the tracing shader is made, the frame is rasterized) */
        bool usable;
        if (!computeFollow(v, scene, MAKE_TRACE, wait, &usable)) return false;
        if (!usable) scene->trace.desc.mode = ARTF_TRACE_NONE;
    }
    /* (by the scene's size, as the CPU: drawn smaller, what a pixel is is larger) */
    artfSceneScaleStep(scene, v->sceneWidth > 0 ? v->sceneWidth : v->width, v->sceneHeight > 0 ? v->sceneHeight : v->height);
    artfSceneFaceParticles(scene);
    if (v->targetsStale && !targetsRemake(v)) return false;
    if (!v->framebuffers[0]) return true; /* a window with no size */
    const bool tracingNow = scene->trace.desc.mode != ARTF_TRACE_NONE;
    v->swarmPlaces = tracingNow ? 0 : swarmList(v, scene);
    if (tracingNow) v->swarms.len = 0;
    if (!syncScene(v, scene)) return false;
    if (!syncEnvironment(v, scene)) return false;
    if (v->floats && !syncLut(v, scene)) return false;
    /* probes that come or go: the scene's pixel shaders, with or without what reads them */
    const bool probes = scene->indirect.on && scene->indirect.probes, walls = probes && !scene->indirect.desc.throughWalls;
    const bool fogGrid = v->floats && scene->post.screen.volumetric > 0 && scene->fogDensity > 0;
    const bool coats = scene->layeredMaterials > 0, softShadows = scene->shadows.soft && scene->shadows.resolution > 0;
    /* a view: shaders that have them, for as long as one is asked */
    const ArtfView view = v->base.view;
    const bool viewShaders = view != ARTF_VIEW_LIT, meshletShaders = view == ARTF_VIEW_MESHLETS && v->primitiveIds;
    const bool tracing = scene->trace.desc.mode != ARTF_TRACE_NONE && !view; /* (a view is of triangles) */
    /* shadows and reflections from rays while drawing: the trees, or the hardware */
    const bool tracedAsked = !tracing && scene->shadows.traced;
    const bool tracedReflections = !tracing && v->floats && scene->post.screen.tracedReflections && scene->post.screen.reflections > 0;
    const bool shapes = artfSceneShapedLights(scene);
    const Features want = {probes, walls, fogGrid, coats, softShadows, shapes, viewShaders, meshletShaders, tracedAsked, tracedAsked && v->rayQuery};
    /* the surface's second word only where motion is followed: other passes, and what is drawn in them */
    const bool wide = v->floats && (scene->post.screen.temporal || scene->post.lens.motionBlur > 0);
    if (!shadersFollow(v, scene, &want, wide, wait)) return false;
    if (v->targetsStale && !targetsRemake(v)) return false;
    if (!v->framebuffers[0]) return true;
    /* (until they are made, reflections from rays are those from the screen: postRecord) */
    bool reflectionsUsable;
    if (tracedReflections && !computeFollow(v, scene, MAKE_REFLECTIONS, wait, &reflectionsUsable)) return false;
    /* What the pipelines there draw: shadows from rays as their pixel shaders have them (with the shadow maps until
     * they are made, or from rays until those without are); temporal antialiasing and motion blur only in passes that
     * follow motion. */
    const bool tracedShadows = !tracing && v->tracedShadows;
    if (!tracing) scene->shadows.traced = tracedShadows;
    if (v->floats && !v->surfaceWide) {
        scene->post.screen.temporal = false;
        scene->post.lens.motionBlur = 0;
    }
    if (v->surfaceWide && scene->deforming.len && !pastVerticesRoom(v)) return false;
    if (tracing && !syncTrace(v, scene)) return false;
    /* light that bounced, gathered here: its rays go through the same trees, or the hardware */
    const bool probeRays = probes && scene->trace.desc.mode == ARTF_TRACE_NONE && !artfVulkanProbesOnCpu;
    if ((tracedShadows || tracedReflections || probeRays) && !syncTraced(v, scene)) return false;
    /* (what rays read of the instances: the slot's, once its fence is waited for) */
    const bool topRays = tracing || tracedShadows || tracedReflections || probeRays;
    const bool topHardware = v->rayQuery && (tracing ? scene->trace.desc.hardware : true);

    {
        ArtfFrameData look;
        artfSceneFrame(scene, v->sceneWidth, v->sceneHeight, &look);
        artfSceneResidencyStep(scene, &look, v->sceneHeight);
    }
    Slot* s = &v->slots[v->frameNumber % SLOTS];
    VkResult result = v->vkWaitForFences(v->device, 1, &s->fence, 1, ~(uint64_t)0);
    if (result != VK_OK) {
        artfLog("artful: waiting for the GPU failed: %d", (int)result);
        return false;
    }
    const uint32_t slotIndex = (uint32_t)(v->frameNumber % SLOTS);
    if (v->retired.len) retiredFree(v, false);
    if (s->probesBackHeld) {
        /* (the probes this slot's frame copied: the scene keeps them, so that a renderer after this one goes on from
         * them, or the CPU's) */
        ArtfIndirect* g = &scene->indirect;
        s->probesBackHeld = false;
        if (g->probes && s->probesBackScene == scene->id && s->probesBackSerial == g->serial && s->probesBack.mapped) {
            memcpy(g->probes, s->probesBack.mapped, (size_t)artfIndirectCount(g) * ARTF_PROBE_FLOATS * 4);
            g->frames = s->probesBackFrames;
            ++g->version;
        }
    }
    if (s->timed && s->passCount) {
        uint64_t ticks[QUERIES];
        /* (1: 64 bits; the fence says it is done) */
        if (v->vkGetQueryPoolResults(v->device, v->timestamps, slotIndex * QUERIES, s->passCount + 1, sizeof ticks, ticks, 8, 1) == VK_OK) {
            v->gpuMs = (double)(ticks[s->passCount] - ticks[0]) * v->timestampPeriod * 1e-6;
            for (uint32_t k = 0; k < s->passCount; ++k) {
                v->passTimes[k].name = s->passNames[k];
                v->passTimes[k].ms = (double)(ticks[k + 1] - ticks[k]) * v->timestampPeriod * 1e-6;
            }
            v->passTimeCount = s->passCount;
        }
    }

    uint32_t image = 0;
    if (v->swapchain) {
        result = v->vkAcquireNextImageKHR(v->device, v->swapchain, ~(uint64_t)0, s->acquired, 0, &image);
        if (result == VK_ERR_OUT_OF_DATE) { v->targetsStale = true; return true; }
        if (result != VK_OK && result != VK_SUBOPTIMAL) {
            artfLog("artful: acquiring an image failed: %d", (int)result);
            lose(v, result);
            return false;
        }
    }
    v->vkResetFences(v->device, 1, &s->fence);
    /* (from here, a frame given up owes its fence, and the image it took: frameOwed) */
    v->owed = s;
    v->owedImage = image;

    ArtfFrameData frame;
    artfSceneFrame(scene, v->sceneWidth, v->sceneHeight, &frame);
    artfFrameForView(&frame, (int)view);
    frame.output[0] = (float)v->width;
    frame.output[1] = (float)v->height;
    frame.output[3] = (float)v->postFrames;
    if (v->floats) {
        /* (an effect came or went: the post buffers as they now need to be, with the device idle) */
        const uint32_t needs = postNeedsOf(v, &frame, scene->clouds && !artfOrtho(&frame));
        if (needs != v->postNeeds) {
            v->vkDeviceWaitIdle(v->device);
            v->postNeeds = needs;
            if (!postBuffersMake(v)) {
                v->postNeeds = ~0u; /* (none fit: made again the next frame) */
                return false;
            }
        }
    }
    if (v->floats || view == ARTF_VIEW_MOTION) {
        /* the frame before, and with temporal antialiasing, a place within the pixel: as softDraw */
        float now[16];
        memcpy(now, frame.viewProj, sizeof now);
        if (v->hasPrevious) memcpy(frame.previous, v->previous, sizeof v->previous);
        memcpy(v->previous, now, sizeof now);
        v->hasPrevious = true;
        if (scene->post.screen.temporal && !view) artfFrameJitter(&frame, v->postFrames++, v->sceneWidth, v->sceneHeight);
        frame.grade2[2][3] = (float)(v->grainFrames++ & 0xFFFFFu);
        if (tracing) frame.post[1][1] = 0; /* (fog that light goes through is drawn, not traced) */
        /* how each instance moved, after the instances in this slot's buffer: for temporal antialiasing and motion blur, and the view of it */
        if (scene->post.screen.temporal || scene->post.lens.motionBlur > 0 || view == ARTF_VIEW_MOTION) {
            artfMotionStep(&v->motion, scene, false);
            frame.moving = v->motion.moving ? 1.f : 0.f;
            if (s->motionStep != v->motion.version || s->motionCount != v->motion.count || s->instanceVersion != scene->instanceVersion) {
                /* (four rows each: the fourth says a pose bent it, and the vertex shader takes its vertices back) */
                float* rows = (float*)((char*)s->instances.mapped + scene->instances.len * sizeof(ArtfInstanceData));
                for (size_t i = 0; i < v->motion.count; ++i, rows += 16) {
                    memcpy(rows, v->motion.motion + i * 12, 48);
                    rows[12] = v->surfaceWide && v->motion.bent[i] ? 1.f : 0.f; /* (only where the vertices were kept) */
                    rows[13] = rows[14] = rows[15] = 0;
                }
                s->motionStep = v->motion.version;
                s->motionCount = v->motion.count;
            }
        }
    } else {
        memset(frame.post, 0, sizeof frame.post);
    }
    /* (rays while drawing: not when all is traced; reflections only in floats) */
    if (tracing || !v->floats) frame.traced &= ~(uint32_t)ARTF_TRACED_REFLECTIONS;
    if (tracing) frame.traced = frame.tracedCasts = 0;
    frame.hizLevels = v->hizLevels;
    memcpy(frame.hiz, v->hizTable, sizeof frame.hiz);
    /* the decals that may show, straight into this slot's buffer (its fence was waited for) */
    frame.indirect[1][3] = (float)artfSceneDecals(scene, &frame, v->sceneHeight, s->decals.mapped);
    bool probeStep = false;
    if (!syncLights(v, s, scene, &frame)) return false;
    memcpy(s->frame.mapped, &frame, sizeof frame);
    /* the frame's lights' profiles, past those past them in their buffer (shaders.c's profileAt) */
    for (uint32_t k = 0; k < frame.lightCount; ++k)
        if (frame.lightShapes[k][2][3] > 0)
            memcpy((char*)s->extraLights.mapped + ARTF_MAX_LIGHTS * sizeof(ArtfLightData) + (size_t)k * ARTF_PROFILE_FLOATS * sizeof(float), scene->lightProfile[(int)frame.lightShapes[k][2][3] - 1], ARTF_PROFILE_FLOATS * sizeof(float));
    if (!syncIndirect(v, s, scene, &frame, &probeStep)) return false;
    syncSlot(v, s, scene);
    if (!syncSwarms(v, s, scene)) return false;
    uint32_t layerCount = 0, traceAdding = 0;
    bool traceClear = false;
    if (!stagePoses(v, s, scene)) return false;
    ArtfShadowFrame shadows;
    const uint32_t shadowViews = tracing ? 0 : artfSceneShadowFrame(scene, &frame, &shadows);
    uint64_t shadowChanged = 0;
    v->shadowMapsDrawn = 0;
    if (tracing) memset(&shadows, 0, sizeof shadows);
    memcpy(s->shadow.mapped, &shadows, sizeof shadows);
    if (shadowViews) {
        const int width = (int)shadows.sun[2] * ARTF_SHADOW_ATLAS_COLUMNS, height = (int)shadows.sun[3];
        if (width != v->shadowWidth || height > v->shadowHeight) {
            v->vkDeviceWaitIdle(v->device);
            if (!shadowAtlasMake(v, width, height)) return false;
        }
        if (!syncShadowList(v, s, scene, shadowViews)) return false;
        if (!shadowCullMake(v)) return false;
        for (uint32_t i = 0; i < shadowViews; ++i) {
            const uint32_t side = (uint32_t)shadows.views[i].size[0];
            const uint64_t key = artfShadowViewKey(scene, &shadows.views[i], side);
            if (key == v->shadowKeys[i] && !artfShadowViewTouched(scene, &shadows.views[i], side, v->shadowSeen)) continue;
            v->shadowKeys[i] = key;
            shadowChanged |= 1ull << i;
            ++v->shadowMapsDrawn;
        }
        v->shadowSeen = scene->shadowChangeCount;
    }
    if (tracing) {
        const uint64_t key = artfSceneTraceKey(scene, &frame);
        const ArtfTraceDesc* d = &scene->trace.desc;
        if (key != v->traceKey || !v->traceDone) {
            /* (with a filter, what changed does not start over: it finds what still holds) */
            traceClear = !d->denoise;
            v->traceDone = 0;
            v->traceKey = key;
        }
        traceAdding = (uint32_t)d->samples;
        if (d->maxSamples > 0) traceAdding = v->traceDone >= (uint32_t)d->maxSamples ? 0 : (uint32_t)artfMini(d->samples, d->maxSamples - (int)v->traceDone);
        if (d->denoise && traceAdding) traceClear = true;
        ArtfTraceFrame traced;
        artfSceneTraceFrame(scene, &frame, v->sceneWidth, v->sceneHeight, d->denoise ? v->traceFrames : v->traceDone, &traced);
        traced.pad[0] = d->denoise ? 1 : v->traceDone + traceAdding;
        traced.pad[1] = v->denoiseFirst;
        memcpy(traced.previous, v->tracePrevious, sizeof traced.previous);
        if (d->denoise) {
            /* how objects moved, as this slot knows it */
            const ArtfTrace* t = &scene->trace;
            const VkDeviceSize bytes = ((VkDeviceSize)t->motion.len + 12) * 4;
            if (bytes > s->motion.size) {
                bufferFree(v, &s->motion);
                if (!bufferNew(v, &s->motion, bytes * 2, BUFFER_STORAGE, true)) return false;
                const DescriptorBufferInfo info = {s->motion.buffer, 0, ~(VkDeviceSize)0};
                const WriteDescriptorSet w = {ST_WRITE_DESCRIPTOR_SET, NULL, s->set, 37, 0, 1, DESCRIPTOR_STORAGE, NULL, &info, NULL};
                v->vkUpdateDescriptorSets(v->device, 1, &w, 0, NULL);
                s->motionVersion = ~t->motionVersion;
            }
            if (s->motionVersion != t->motionVersion) {
                memcpy(s->motion.mapped, t->motion.items, t->motion.len * 4);
                s->motionVersion = t->motionVersion;
            }
        }
        memcpy(s->trace.mapped, &traced, sizeof traced);
        v->traceDone += traceAdding;
        if (d->denoise && traceAdding) {
            v->traceFrames += traceAdding;
            v->denoiseFirst = false;
            memcpy(v->tracePrevious, frame.viewProj, sizeof v->tracePrevious);
        }
    } else if (scene->translucentCount) {
        artfVecReserve(&v->layers, scene->translucentCount);
        layerCount = artfSceneLayers(scene, &frame, v->layers.items);
        /* (where the grid of layers by transmittance is: the frame's row, written again) */
        artfLayerRange(&frame, v->layers.items, layerCount);
        memcpy((char*)s->frame.mapped + offsetof(ArtfFrameData, layers), frame.layers, sizeof frame.layers);
        const VkDeviceSize bytes = (VkDeviceSize)layerCount * 8;
        if (bytes > s->layerList.size) {
            bufferFree(v, &s->layerList);
            if (!bufferNew(v, &s->layerList, bytes * 2, BUFFER_TRANSFER_SRC, true)) return false;
        }
        uint32_t* out = s->layerList.mapped;
        for (uint32_t i = 0; i < layerCount; ++i) {
            out[i * 2] = v->layers.items[i].instance;
            out[i * 2 + 1] = v->layers.items[i].mesh;
        }
    }
    const bool background = scene->backgroundPass.on && !view;
    ArtfBackgroundBlock backgroundFrame;
    if (background) {
        uint32_t bloomPixels = 0;
        artfBackgroundFrame(scene, v->sceneWidth, v->sceneHeight, &backgroundFrame, &bloomPixels);
        if (!syncBackground(v, scene, bloomPixels)) return false;
        memcpy(s->background.mapped, &backgroundFrame, sizeof backgroundFrame);
        /* (each frame in flight has its own copy of the stream: this one's is free to write) */
        const ArtfBackgroundPass* p = &scene->backgroundPass.pass;
        if (s->streamStamp != scene->backgroundPass.streamStamp || s->streamScene != scene->id) {
            const VkDeviceSize bytes = p->stream ? p->streamBytes : 0;
            if (bytes > s->stream.size) {
                bufferFree(v, &s->stream);
                if (!bufferNew(v, &s->stream, bytes, BUFFER_STORAGE, true)) return false;
                const DescriptorBufferInfo info = {s->stream.buffer, 0, ~(VkDeviceSize)0};
                const WriteDescriptorSet write = {ST_WRITE_DESCRIPTOR_SET, NULL, s->set, 15, 0, 1, DESCRIPTOR_STORAGE, NULL, &info, NULL};
                v->vkUpdateDescriptorSets(v->device, 1, &write, 0, NULL);
            }
            if (bytes) memcpy(s->stream.mapped, p->stream, (size_t)bytes);
            s->streamStamp = scene->backgroundPass.streamStamp;
            s->streamScene = scene->id;
        }
    }
    const bool backgroundApart = background && v->backgroundPipeline && backgroundRays(v, scene) > BACKGROUND_RAYS_APART;
    if (backgroundApart && !traceBackgroundApart(v, s, scene)) {
        artfLog("artful: the background pass's rays could not be traced");
        return false;
    }

    VkCommandBuffer c = s->commands;
    /* (its descriptors change here: not once the set is bound) */
    const bool overdraw = view == ARTF_VIEW_OVERDRAW;
    const bool sorted = layerCount && v->fragmentStores && !v->floats && (v->samples == 1 || v->sampleShading || v->depthCoverage) && scene->features[ARTF_FEATURE_LAYERS_SORTED] && !overdraw;
    if (sorted && !layerListsMake(v, scene)) return false;
    if (v->floats && !cloudsSync(v, scene, &frame)) return false;
    if (sorted) {
        /* (one call for all of them: what they add to the lists has no order) */
        if ((VkDeviceSize)layerCount * 20 > s->layerCommands.size) {
            bufferFree(v, &s->layerCommands);
            if (!bufferNew(v, &s->layerCommands, (VkDeviceSize)layerCount * 40, BUFFER_INDIRECT, true)) return false;
        }
        uint32_t* commands = s->layerCommands.mapped;
        for (uint32_t i = 0; i < layerCount; ++i) {
            const MeshRange* m = &v->meshes.items[v->layers.items[i].mesh];
            const uint32_t command[5] = {m->indexCount, 1, m->firstIndex, (uint32_t)m->vertexOffset, layersAt(v) + i};
            memcpy(commands + (size_t)i * 5, command, sizeof command);
        }
    }
    const bool drawsInstances = !tracing && scene->instances.len;
    /* (the meshlets view needs whole meshes: see meshletIdsUp) */
    const bool meshlets = drawsInstances && v->drawCounted && v->mostMeshlets && scene->features[ARTF_FEATURE_CONES] && view != ARTF_VIEW_MESHLETS;
    if (drawsInstances && !stageMeshlets(v, s, meshlets)) return false;
    if (meshlets && !meshletPipelineMake(v)) return false;
    if (meshlets && scene->features[ARTF_FEATURE_OCCLUSION] && !meshletFlagsUp(v, scene)) return false;
    if (overdraw && !overdrawPipelinesMake(v)) return false;
    if (meshletShaders && !meshletIdsUp(v, scene)) return false;
    const CommandBufferBeginInfo begin = {ST_COMMAND_BUFFER_BEGIN, NULL, 1, NULL};
    v->vkResetCommandBuffer(c, 0);
    v->vkBeginCommandBuffer(c, &begin);
    trackFrameStart(v);
    s->timed = v->timestamps != 0;
    s->passCount = 0;
    if (s->timed) {
        v->vkCmdResetQueryPool(c, v->timestamps, slotIndex * QUERIES, QUERIES);
        v->vkCmdWriteTimestamp(c, STAGE_TOP, v->timestamps, slotIndex * QUERIES);
    }
    if (!uploadOverlay(v, s, c)) return false;
    if (topRays && !slotMeshSync(v, s, c, scene, topHardware)) return false;
    if (topRays && !slotTopSync(v, s, c, scene, topHardware)) return false;
    if (v->surfaceWide && scene->deforming.len) pastVerticesCopy(v, c, scene);
    runBones(v, c, s);
    if (v->posedCopies.len) {
        memoryBarrier(v, c, STAGE_VERTEX_INPUT, STAGE_TRANSFER, ACCESS_VERTEX_READ, ACCESS_TRANSFER_WRITE);
        v->vkCmdCopyBuffer(c, s->posed.buffer, v->vertices.buffer, (uint32_t)v->posedCopies.len, v->posedCopies.items);
        memoryBarrier(v, c, STAGE_TRANSFER, STAGE_VERTEX_INPUT, ACCESS_TRANSFER_WRITE, ACCESS_VERTEX_READ);
    }

    const uint32_t meshCount = (uint32_t)v->meshes.len, instanceCount = tracing ? 0 : (uint32_t)scene->instances.len;
    /* (overdraw writes no depth, so there is nothing to test against: it goes without) */
    const bool occlusion = instanceCount && scene->features[ARTF_FEATURE_OCCLUSION] && !overdraw;
    const VkPipeline solidPipeline = overdraw ? v->overdrawPipelines[0] : v->scenePipeline, cutoutPipeline = overdraw ? v->overdrawPipelines[1] : v->cutoutPipeline;
    const bool cutouts = !tracing && (scene->cutoutCount != 0 || scene->features[ARTF_FEATURE_LOD_FADE]);
    if (instanceCount) {
        /* the commands start with no copies; the shader adds those in view. (The barrier also takes
         * in the frame before, whose flags and pyramid this one touches.) */
        const BufferCopy copy = {0, 0, (VkDeviceSize)meshCount * (cutouts ? 80 : occlusion ? 40 : 20)};
        memoryBarrier(v, c, STAGE_COMPUTE | STAGE_DRAW_INDIRECT, STAGE_TRANSFER | STAGE_COMPUTE, ACCESS_SHADER_WRITE | ACCESS_SHADER_READ | ACCESS_INDIRECT, ACCESS_TRANSFER_WRITE | ACCESS_SHADER_READ | ACCESS_SHADER_WRITE);
        v->vkCmdCopyBuffer(c, s->template_.buffer, s->indirect.buffer, 1, &copy);
        memoryBarrier(v, c, STAGE_TRANSFER, STAGE_COMPUTE, ACCESS_TRANSFER_WRITE, ACCESS_SHADER_READ | ACCESS_SHADER_WRITE);
        v->vkCmdBindPipeline(c, 1, v->cullPipelines[occlusion ? ARTF_CULL_EARLY : ARTF_CULL_ALL]);
        v->vkCmdBindDescriptorSets(c, 1, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
        v->vkCmdDispatch(c, (instanceCount + 63) / 64, 1, 1);
        if (meshlets) {
            memoryBarrier(v, c, STAGE_COMPUTE, STAGE_DRAW_INDIRECT | STAGE_COMPUTE, ACCESS_SHADER_WRITE, ACCESS_INDIRECT | ACCESS_SHADER_READ | ACCESS_SHADER_WRITE);
            v->vkCmdBindPipeline(c, 1, v->meshletPipelines[occlusion ? ARTF_CULL_EARLY : ARTF_CULL_ALL]);
            v->vkCmdDispatchIndirect(c, s->meshletState.buffer, 0);
        }
        memoryBarrier(v, c, STAGE_COMPUTE, STAGE_DRAW_INDIRECT | STAGE_VERTEX_SHADER, ACCESS_SHADER_WRITE, ACCESS_INDIRECT | ACCESS_SHADER_READ);
    }
    if (layerCount) {
        const BufferCopy copy = {0, (VkDeviceSize)layersAt(v) * 8, (VkDeviceSize)layerCount * 8};
        memoryBarrier(v, c, STAGE_COMPUTE, STAGE_TRANSFER, ACCESS_SHADER_WRITE, ACCESS_TRANSFER_WRITE);
        v->vkCmdCopyBuffer(c, s->layerList.buffer, s->visible.buffer, 1, &copy);
        memoryBarrier(v, c, STAGE_TRANSFER, STAGE_VERTEX_SHADER, ACCESS_TRANSFER_WRITE, ACCESS_SHADER_READ);
    }
    passMark(v, c, s, "cull");
    if (probeStep) {
        gatherProbes(v, c, s, scene);
        passMark(v, c, s, "probes");
    }

    runSwarms(v, c, s);
    if (background) {
        traceBackground(v, c, s, &backgroundFrame, backgroundApart);
        passMark(v, c, s, "background");
    }
    if (shadowViews) {
        drawShadows(v, c, s, &shadows, shadowViews, shadowChanged);
        if (shadowChanged) passMark(v, c, s, "shadows");
    }
    const bool froxels = v->floats && frame.post[1][1] > 0 && frame.look[1] > 0;
    if (froxels) {
        /* fog that light goes through: its grid, from the sun's maps, for what is drawn to read (after the frame before read it) */
        trackUse(v, c, 0, TRACK(TRACK_FOG), STAGE_COMPUTE, 0, ACCESS_SHADER_WRITE);
        v->vkCmdBindDescriptorSets(c, 1, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
        const uint32_t cells = (uint32_t)((v->sceneWidth + ARTF_FROXEL_TILE - 1) / ARTF_FROXEL_TILE) * (uint32_t)((v->sceneHeight + ARTF_FROXEL_TILE - 1) / ARTF_FROXEL_TILE);
        postStep(v, c, POST_FROXELS, 0, cells);
        passMark(v, c, s, "fog grid");
    }
    if (tracing) {
        /* (the frame before read and wrote what this one is about to) */
        if (traceClear) {
            trackUse(v, c, 0, TRACK(TRACK_TRACE), STAGE_TRANSFER, 0, ACCESS_TRANSFER_WRITE);
            v->vkCmdFillBuffer(c, v->traceSum.buffer, 0, ~(VkDeviceSize)0, 0);
        }
        if (traceAdding) {
            trackUse(v, c, TRACK(TRACK_TRACE), TRACK(TRACK_TRACE), STAGE_COMPUTE, ACCESS_SHADER_READ, ACCESS_SHADER_WRITE);
            v->vkCmdBindPipeline(c, 1, v->tracePipeline);
            v->vkCmdBindDescriptorSets(c, 1, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
            v->vkCmdDispatch(c, ((uint32_t)v->sceneWidth + 7) / 8, ((uint32_t)v->sceneHeight + 7) / 8, 1);
            if (scene->trace.desc.denoise) denoise(v, c, s);
            passMark(v, c, s, "trace");
        }
    }

    /* (one for each attachment the scene's pass has: with samples and floats, four; the surface's is 0) */
    ClearValue clears[4];
    memset(clears, 0, sizeof clears);
    memcpy(clears[0].color, frame.background, sizeof clears[0].color);
    if (v->floats) {
        /* (the background as light: what shows, as light, as on the CPU) */
        const float alpha = frame.background[3], over = alpha > 0 ? 1.f / alpha : 0.f;
        for (int k = 0; k < 3; ++k) clears[0].color[k] = artfSrgbDecode(frame.background[k] * over) * alpha;
    }
    const Rect2D area = {{0, 0}, {(uint32_t)v->sceneWidth, (uint32_t)v->sceneHeight}};
    /* (the height negative: y goes up in clip space, as on the CPU) */
    const Viewport viewport = {0, (float)v->sceneHeight, (float)v->sceneWidth, -(float)v->sceneHeight, 0, 1};
    if (sorted) {
        /* (the frame before read them) */
        trackUse(v, c, 0, TRACK(TRACK_LAYERS), STAGE_TRANSFER, 0, ACCESS_TRANSFER_WRITE);
        v->vkCmdFillBuffer(c, v->layerCounts.buffer, 0, ~(VkDeviceSize)0, 0);
        if (v->layerSumsFresh) v->vkCmdFillBuffer(c, v->layerFragments.buffer, 0, ~(VkDeviceSize)0, 0);
        v->layerSumsFresh = false;
    }
    /* what the scene's passes' pixel shaders read (fog's grid, what was traced) and the layers' lists they write: one
     * barrier for all, from what wrote each (trackUse); depth, by the passes' own dependencies */
    {
        const uint32_t reads = (froxels ? TRACK(TRACK_FOG) : 0u) | (tracing ? TRACK(TRACK_TRACE) : 0u) | (sorted ? TRACK(TRACK_LAYERS) : 0u);
        if (reads) trackUse(v, c, reads, sorted ? TRACK(TRACK_LAYERS) : 0u, STAGE_FRAGMENT, ACCESS_SHADER_READ, sorted ? ACCESS_SHADER_WRITE : 0u);
        trackCovered(v, 0, TRACK(TRACK_DEPTH), STAGE_EARLY_TESTS | STAGE_LATE_TESTS, 0, ACCESS_DEPTH_WRITE);
    }
    RenderPassBeginInfo pass = {ST_RENDER_PASS_BEGIN, NULL, occlusion || sorted || v->floats ? v->earlyPass : v->renderPass, v->framebuffers[image], area, v->floats ? (v->samples > 1 ? 4u : 3u) : 2u, clears};
    v->vkCmdBeginRenderPass(c, &pass, 0);
    v->vkCmdSetViewport(c, 0, 1, &viewport);
    v->vkCmdSetScissor(c, 0, 1, &area);
    if (background || (frame.sky[3] > 0 && frame.ground[3] > 0)) {
        v->vkCmdBindPipeline(c, 0, background ? v->backdropPipeline : v->environmentPipeline);
        v->vkCmdBindDescriptorSets(c, 0, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
        v->vkCmdDraw(c, 3, 1, 0, 0);
    }
    if (froxels) {
        v->vkCmdBindPipeline(c, 0, v->fogPipeline);
        v->vkCmdBindDescriptorSets(c, 0, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
        v->vkCmdDraw(c, 3, 1, 0, 0);
    }
    if (instanceCount) drawCommands(v, c, s, solidPipeline, 0, meshCount);
    if (meshlets) v->vkCmdDrawIndexedIndirectCount(c, s->meshletCommands.buffer, 0, s->meshletState.buffer, MESHLET_DRAWN * 4, MESHLET_COMMANDS, 20);
    if (cutouts) drawCommands(v, c, s, cutoutPipeline, meshCount * 2, meshCount);
    if (occlusion) {
        v->vkCmdEndRenderPass(c);
        passMark(v, c, s, "scene, early");
        /* the pyramid, from what was just drawn */
        v->vkCmdBindPipeline(c, 1, v->hizFirstPipeline);
        v->vkCmdBindDescriptorSets(c, 1, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
        /* (what read it this frame, the early culling, is before the scene's pass, which orders it before this) */
        trackCovered(v, TRACK(TRACK_HIZ), 0, STAGE_COMPUTE, ACCESS_SHADER_READ, 0);
        v->trackWriteStage[TRACK_HIZ] = 0;
        v->trackReadStage[TRACK_HIZ] = 0;
        for (uint32_t level = 0; level < v->hizLevels; ++level) {
            if (level == 1) v->vkCmdBindPipeline(c, 1, v->hizNextPipeline);
            v->vkCmdPushConstants(c, v->pipelineLayout, SHADER_COMPUTE, 0, 4, &level);
            if (level) trackUse(v, c, TRACK(TRACK_HIZ), TRACK(TRACK_HIZ), STAGE_COMPUTE, ACCESS_SHADER_READ, ACCESS_SHADER_WRITE);
            else trackCovered(v, 0, TRACK(TRACK_HIZ), STAGE_COMPUTE, 0, ACCESS_SHADER_WRITE);
            v->vkCmdDispatch(c, (v->hizTable[level][1] * v->hizTable[level][2] + 63) / 64, 1, 1);
        }
        trackUse(v, c, TRACK(TRACK_HIZ), 0, STAGE_COMPUTE, ACCESS_SHADER_READ, 0);
        v->vkCmdBindPipeline(c, 1, v->cullPipelines[ARTF_CULL_LATE]);
        v->vkCmdDispatch(c, (instanceCount + 63) / 64, 1, 1);
        if (meshlets) {
            memoryBarrier(v, c, STAGE_COMPUTE, STAGE_DRAW_INDIRECT | STAGE_COMPUTE, ACCESS_SHADER_WRITE, ACCESS_INDIRECT | ACCESS_SHADER_READ | ACCESS_SHADER_WRITE);
            v->vkCmdBindPipeline(c, 1, v->meshletPipelines[ARTF_CULL_LATE]);
            v->vkCmdDispatchIndirect(c, s->meshletState.buffer, (VkDeviceSize)meshletHalf(v) * 4);
        }
        memoryBarrier(v, c, STAGE_COMPUTE, STAGE_DRAW_INDIRECT | STAGE_VERTEX_SHADER, ACCESS_SHADER_WRITE, ACCESS_INDIRECT | ACCESS_SHADER_READ);
        passMark(v, c, s, "occlusion");
        pass.renderPass = sorted || v->floats ? v->midPass : v->latePass;
        v->vkCmdBeginRenderPass(c, &pass, 0);
        v->vkCmdSetViewport(c, 0, 1, &viewport);
        v->vkCmdSetScissor(c, 0, 1, &area);
        drawCommands(v, c, s, solidPipeline, meshCount, meshCount);
        if (meshlets) v->vkCmdDrawIndexedIndirectCount(c, s->meshletCommands.buffer, (VkDeviceSize)MESHLET_COMMANDS * 20, s->meshletState.buffer, ((VkDeviceSize)meshletHalf(v) + MESHLET_DRAWN) * 4, MESHLET_COMMANDS, 20);
        if (cutouts) drawCommands(v, c, s, cutoutPipeline, meshCount * 3, meshCount);
    }
    if (!v->floats) passMark(v, c, s, occlusion ? "scene, late" : "scene");
    if (v->floats) {
        /* what comes after reads the depth of what is solid: it is taken now, before layers write theirs */
        v->vkCmdEndRenderPass(c);
        passMark(v, c, s, occlusion ? "scene, late" : "scene");
        v->vkCmdBindDescriptorSets(c, 1, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
        /* (the gather's shader names set 1 too, the post buffers' parts: bound, though depth alone is written; and the
         * frame before's last pass read what the post passes write, in another submission: after it) */
        rolesBind(v, c, 1);
        trackUse(v, c, 0, TRACK(TRACK_POST), STAGE_COMPUTE, 0, ACCESS_SHADER_WRITE);
        postStep(v, c, POST_GATHER, 1, (uint32_t)v->sceneWidth * (uint32_t)v->sceneHeight);
        passMark(v, c, s, "depth for what follows");
        pass.renderPass = v->latePass;
        v->vkCmdBeginRenderPass(c, &pass, 0);
        v->vkCmdSetViewport(c, 0, 1, &viewport);
        v->vkCmdSetScissor(c, 0, 1, &area);
    }
    if (sorted) {
        /* by transmittance: what each surface takes into the grid; the grid's columns, by what is in front; each
         * surface into its sample's sums, by its weight; the sums over the frame. (Sums of integers: no order.) */
        v->vkCmdBindPipeline(c, 0, v->layerGridPipeline);
        v->vkCmdBindDescriptorSets(c, 0, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
        layerDraws(v, c, s, layerCount);
        v->vkCmdEndRenderPass(c);
        memoryBarrier(v, c, STAGE_FRAGMENT, STAGE_COMPUTE, ACCESS_SHADER_WRITE, ACCESS_SHADER_READ | ACCESS_SHADER_WRITE);
        v->vkCmdBindPipeline(c, 1, v->layerPrefixPipeline);
        v->vkCmdBindDescriptorSets(c, 1, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
        v->vkCmdDispatch(c, (layerColumns(v) + 63) / 64, 1, 1);
        memoryBarrier(v, c, STAGE_COMPUTE, STAGE_FRAGMENT, ACCESS_SHADER_WRITE, ACCESS_SHADER_READ);
        pass.renderPass = v->latePass;
        v->vkCmdBeginRenderPass(c, &pass, 0);
        v->vkCmdSetViewport(c, 0, 1, &viewport);
        v->vkCmdSetScissor(c, 0, 1, &area);
        v->vkCmdBindPipeline(c, 0, v->layerListPipeline);
        layerDraws(v, c, s, layerCount);
        /* (a sample's sums are its own: the pass's own dependency, by region, is enough) */
        const GlobalBarrier after = {ST_MEMORY_BARRIER, NULL, ACCESS_SHADER_WRITE, ACCESS_SHADER_READ};
        v->vkCmdPipelineBarrier(c, STAGE_FRAGMENT, STAGE_FRAGMENT, 1 /* by region */, 1, &after, 0, NULL, 0, NULL);
        v->vkCmdBindPipeline(c, 0, v->layerResolvePipeline);
        v->vkCmdDraw(c, 3, 1, 0, 0);
    } else if (layerCount) {
        drawLayers(v, c, scene, layerCount);
    }
    drawSwarms(v, c, s, scene);
    if (layerCount) passMark(v, c, s, "layers");
    if (tracing && (v->traceDone || scene->trace.desc.denoise)) {
        v->vkCmdBindPipeline(c, 0, v->traceShowPipeline);
        v->vkCmdBindDescriptorSets(c, 0, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
        v->vkCmdDraw(c, 3, 1, 0, 0);
    }
    if (v->overlayPixels && v->overlayUsable && !v->floats) {
        v->vkCmdBindPipeline(c, 0, v->overlayPipeline);
        v->vkCmdBindDescriptorSets(c, 0, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
        v->vkCmdDraw(c, 3, 1, 0, 0);
    }
    v->vkCmdEndRenderPass(c);
    if (v->floats) postRecord(v, c, s, &frame, image);
    else passMark(v, c, s, "overlay and out");
    v->vkEndCommandBuffer(c);

    const VkFlags waitStage = STAGE_COLOR;
    SubmitInfo submit = {ST_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &c, 0, NULL};
    if (v->swapchain) {
        submit.waitCount = 1;
        submit.waits = &s->acquired;
        submit.waitStages = &waitStage;
        submit.signalCount = 1;
        submit.signals = &v->rendered[image];
    }
    const bool injected = artfVulkanSubmitsLeft == 0;
    result = injected ? -4 /* VK_ERROR_DEVICE_LOST */ : v->vkQueueSubmit(v->queue, 1, &submit, s->fence);
    v->owed = NULL;
    if (artfVulkanSubmitsLeft > 0) --artfVulkanSubmitsLeft;
    if (result != VK_OK) {
        artfLog("artful: submitting a frame failed: %d", (int)result);
        /* (a loss a test made up: the device is not lost, and the driver may be asked why only when it is) */
        if (injected) v->lost = true;
        else lose(v, result);
        return false;
    }
    ++v->frameNumber;
    if (v->swapchain) {
        const PresentInfo present = {ST_PRESENT_INFO, NULL, 1, &v->rendered[image], 1, &v->swapchain, &image, NULL};
        result = v->vkQueuePresentKHR(v->queue, &present);
        if (result == VK_SUBOPTIMAL) { if (surfaceChanged(v)) v->targetsStale = true; }
        else if (result == VK_ERR_OUT_OF_DATE) v->targetsStale = true;
        else if (result != VK_OK) {
            artfLog("artful: presenting a frame failed: %d", (int)result);
            lose(v, result);
            return false;
        }
    }
    v->cpuMs = artfNowMs() - start;
    return true;
}

bool vkReadPixels(ArtfRenderer* r, uint32_t* dst) {
    Vulkan* v = (Vulkan*)r;
    if (v->lost || v->swapchain || !v->frameNumber) return false; /* (what was presented is the window system's) */
    if (v->vkDeviceWaitIdle(v->device) != VK_OK) return false;
    const size_t bytes = (size_t)v->imageWidth * v->imageHeight * 4;
    if (v->readback.size < bytes) {
        bufferFree(v, &v->readback);
        if (!bufferNewFor(v, &v->readback, bytes, BUFFER_TRANSFER_DST, BUFFER_READ_BY_CPU)) {
            bufferFree(v, &v->readback);
            return false;
        }
    }
    VkCommandBuffer c = oneShotBegin(v);
    const BufferImageCopy region = {0, 0, 0, {ASPECT_COLOR, 0, 0, 1}, {0, 0, 0}, {(uint32_t)v->imageWidth, (uint32_t)v->imageHeight, 1}};
    v->vkCmdCopyImageToBuffer(c, v->offscreen.image, LAYOUT_TRANSFER_SRC, v->readback.buffer, 1, &region);
    memoryBarrier(v, c, STAGE_TRANSFER, STAGE_HOST, ACCESS_TRANSFER_WRITE, ACCESS_HOST_READ);
    bool ok = oneShotEnd(v, c);
    if (ok) memcpy(dst, v->readback.mapped, bytes);
    if (ok && v->colorFormat == FORMAT_A2B10G10R10)
        /* (red and blue change places: A2R10G10B10, as the CPU's) */
        for (size_t i = 0; i < bytes / 4; ++i) dst[i] = (dst[i] & 0xC00FFC00u) | (dst[i] & 0x3FFu) << 20 | (dst[i] >> 20 & 0x3FFu);
    return ok;
}

void vkResize(ArtfRenderer* r, int width, int height) {
    Vulkan* v = (Vulkan*)r;
    if (width == v->width && height == v->height) return;
    v->width = width;
    v->height = height;
    v->targetsStale = true;
}

uint32_t* vkPixels(ArtfRenderer* r) { (void)r; return NULL; }
const char* vkDescription(const ArtfRenderer* r) { return ((const Vulkan*)r)->description; }
void vkStats(const ArtfRenderer* r, ArtfFrameStats* out) {
    const Vulkan* v = (const Vulkan*)r;
    out->cpuMs = v->cpuMs;
    out->traceSamples = v->traceDone;
    out->gpuMs = v->gpuMs;
    out->gpuBytes = v->deviceBytes;
    out->lightsOmitted = v->lightsOmitted;
    out->shadowMapsDrawn = v->shadowMapsDrawn;
    out->passCount = v->passTimeCount;
    memcpy(out->passes, v->passTimes, v->passTimeCount * sizeof *out->passes);
}

uint32_t artfVulkanTemporalFrames(const ArtfRenderer* r) { return ((const Vulkan*)r)->postFrames; }

/* ---- start and end ---- */

void artfVulkanWarm(ArtfRenderer* r) {
    Vulkan* v = (Vulkan*)r;
    /* (the next draw takes what was made, and makes before it draws what is still not there: pipelinesTake, wait) */
    if (v->shaderJob) {
        artfTaskJoin(v->shaderJob->task);
        v->shaderJob->task = NULL;
    }
    for (int k = 0; k < MAKE_COMPUTE; ++k) {
        if (!v->computeJobs[k]) continue;
        artfTaskJoin(v->computeJobs[k]->task);
        v->computeJobs[k]->task = NULL;
    }
    v->warmed = true;
}

void vkFree(ArtfRenderer* r) {
    Vulkan* v = (Vulkan*)r;
    if (v->device) {
        shaderJobEnd(v, NULL, false);
        for (int k = 0; k < MAKE_COMPUTE; ++k) computeJobEnd(v, NULL, k, false);
        v->vkDeviceWaitIdle(v->device);
        retiredFree(v, true);
        artfVecFree(&v->retired);
        targetsFree(v);
        for (size_t i = 0; i < v->textures.len; ++i) imageFree(v, &v->textures.items[i]);
        artfVecFree(&v->textures);
        bufferFree(v, &v->vertices);
        bufferFree(v, &v->indices);
        bufferFree(v, &v->meshTable);
        bufferFree(v, &v->colors);
        bufferFree(v, &v->traceNodes);
        bufferFree(v, &v->traceTriangles);
        bufferFree(v, &v->traceVertices);
        artfFree(v->traceUp);
        artfVecFree(&v->traceCopies[0]);
        artfVecFree(&v->traceCopies[1]);
        for (int k = 0; k < SLOTS; ++k) {
            bufferFree(v, &v->slots[k].traceStaging);
            bufferFree(v, &v->slots[k].traceTop);
            bufferFree(v, &v->slots[k].traceInstances);
        }
        rtFree(v);
        if (v->skinPipeline) v->vkDestroyPipeline(v->device, v->skinPipeline, NULL);
        if (v->swarmPipeline) v->vkDestroyPipeline(v->device, v->swarmPipeline, NULL);
        bufferFree(v, &v->swarmState);
        artfVecFree(&v->swarms);
        artfVecFree(&v->swarmsBefore);
        artfVecFree(&v->swarmSeen);
        artfVecFree(&v->swarmAt);
        for (int i = 0; i < 3; ++i)
            if (v->meshletPipelines[i]) v->vkDestroyPipeline(v->device, v->meshletPipelines[i], NULL);
        bufferFree(v, &v->meshletFlags);
        bufferFree(v, &v->meshletFlagStarts);
        bufferFree(v, &v->meshletTable);
        bufferFree(v, &v->meshletMeshes);
        if (v->shadowCullPipeline) v->vkDestroyPipeline(v->device, v->shadowCullPipeline, NULL);
        bufferFree(v, &v->skinData);
        artfVecFree(&v->skinPlaces);
        artfVecFree(&v->skinJobs);
        artfVecFree(&v->scratchSkinCounts);
        if (v->denoiseLight) v->vkDestroyPipeline(v->device, v->denoiseLight, NULL);
        if (v->denoiseTime) v->vkDestroyPipeline(v->device, v->denoiseTime, NULL);
        if (v->denoiseVariance) v->vkDestroyPipeline(v->device, v->denoiseVariance, NULL);
        for (int i = 0; i < ARTF_DENOISE_PASSES; ++i)
            if (v->denoiseSpace[i]) v->vkDestroyPipeline(v->device, v->denoiseSpace[i], NULL);
        if (v->denoiseEnd) v->vkDestroyPipeline(v->device, v->denoiseEnd, NULL);
        if (v->shadowFramebuffer) v->vkDestroyFramebuffer(v->device, v->shadowFramebuffer, NULL);
        imageFree(v, &v->shadowAtlas);
        bufferFree(v, &v->hdr);
        bufferFree(v, &v->bloom);
        bufferFree(v, &v->backgroundData);
        bufferFree(v, &v->environment);
        bufferFree(v, &v->lutStaging);
        if (v->environmentPipeline) v->vkDestroyPipeline(v->device, v->environmentPipeline, NULL);
        for (int i = 0; i < POST_KINDS; ++i)
            if (v->postPipelines[i]) v->vkDestroyPipeline(v->device, v->postPipelines[i], NULL);
        bufferFree(v, &v->pastVertices);
        artfMotionFree(&v->motion);
        if (v->fogPipeline) v->vkDestroyPipeline(v->device, v->fogPipeline, NULL);
        if (v->presentPipeline) v->vkDestroyPipeline(v->device, v->presentPipeline, NULL);
        if (v->presentPass) v->vkDestroyRenderPass(v->device, v->presentPass, NULL);
        artfFree(v->lightScratch);
        artfPoolFree(v->pool);
        artfFree(v->listScratch);
        for (int i = 0; i < SLOTS; ++i) {
            Slot* s = &v->slots[i];
            bufferFree(v, &s->frame);
            bufferFree(v, &s->extraLights);
            bufferFree(v, &s->decals);
            bufferFree(v, &s->tileLights);
            bufferFree(v, &s->probes);
            bufferFree(v, &s->probesBack);
            bufferFree(v, &s->background);
            bufferFree(v, &s->stream);
            bufferFree(v, &s->instances);
            bufferFree(v, &s->visible);
            bufferFree(v, &s->indirect);
            bufferFree(v, &s->template_);
            bufferFree(v, &s->materials);
            bufferFree(v, &s->layerList);
            bufferFree(v, &s->trace);
            bufferFree(v, &s->shadow);
            bufferFree(v, &s->shadowList);
            bufferFree(v, &s->shadowCommands);
            bufferFree(v, &s->posed);
            bufferFree(v, &s->skinTable);
            bufferFree(v, &s->swarmRows);
            bufferFree(v, &s->meshletCommands);
            bufferFree(v, &s->layerCommands);
            bufferFree(v, &s->meshletState);
            bufferFree(v, &s->motion);
            bufferFree(v, &s->shadowViewCommands);
            bufferFree(v, &s->shadowCasters);
            if (s->fence) v->vkDestroyFence(v->device, s->fence, NULL);
            if (s->acquired) v->vkDestroySemaphore(v->device, s->acquired, NULL);
        }
        for (int i = 0; i < MAX_SWAPCHAIN_IMAGES; ++i)
            if (v->rendered[i]) v->vkDestroySemaphore(v->device, v->rendered[i], NULL);
        if (v->swapchain) v->vkDestroySwapchainKHR(v->device, v->swapchain, NULL);
        for (int i = 0; i < 3; ++i)
            if (v->cullPipelines[i]) v->vkDestroyPipeline(v->device, v->cullPipelines[i], NULL);
        if (v->hizFirstPipeline) v->vkDestroyPipeline(v->device, v->hizFirstPipeline, NULL);
        if (v->hizNextPipeline) v->vkDestroyPipeline(v->device, v->hizNextPipeline, NULL);
        bufferFree(v, &v->flags);
        if (v->scenePipeline) v->vkDestroyPipeline(v->device, v->scenePipeline, NULL);
        if (v->overlayPipeline) v->vkDestroyPipeline(v->device, v->overlayPipeline, NULL);
        if (v->sceneVertexModule) v->vkDestroyShaderModule(v->device, v->sceneVertexModule, NULL);
        if (v->shadowPipeline) v->vkDestroyPipeline(v->device, v->shadowPipeline, NULL);
        if (v->shadowHolesPipeline) v->vkDestroyPipeline(v->device, v->shadowHolesPipeline, NULL);
        if (v->shadowPass) v->vkDestroyRenderPass(v->device, v->shadowPass, NULL);
        if (v->tracePipeline) v->vkDestroyPipeline(v->device, v->tracePipeline, NULL);
        if (v->reflectionTraced) v->vkDestroyPipeline(v->device, v->reflectionTraced, NULL);
        if (v->probePipeline) v->vkDestroyPipeline(v->device, v->probePipeline, NULL);
        bufferFree(v, &v->probesNow);
        bufferFree(v, &v->probesNext);
        if (v->traceShowPipeline) v->vkDestroyPipeline(v->device, v->traceShowPipeline, NULL);
        if (v->cutoutPipeline) v->vkDestroyPipeline(v->device, v->cutoutPipeline, NULL);
        if (v->layerListPipeline) v->vkDestroyPipeline(v->device, v->layerListPipeline, NULL);
        if (v->layerResolvePipeline) v->vkDestroyPipeline(v->device, v->layerResolvePipeline, NULL);
        if (v->layerGridPipeline) v->vkDestroyPipeline(v->device, v->layerGridPipeline, NULL);
        if (v->layerPrefixPipeline) v->vkDestroyPipeline(v->device, v->layerPrefixPipeline, NULL);
        bufferFree(v, &v->layerCounts);
        bufferFree(v, &v->layerFragments);
        bufferFree(v, &v->cloudField);
        bufferFree(v, &v->cloudGrid);
        if (v->midPass) v->vkDestroyRenderPass(v->device, v->midPass, NULL);
        if (v->resolvePass) v->vkDestroyRenderPass(v->device, v->resolvePass, NULL);
        if (v->layerDepthPipeline) v->vkDestroyPipeline(v->device, v->layerDepthPipeline, NULL);
        if (v->layerPipeline) v->vkDestroyPipeline(v->device, v->layerPipeline, NULL);
        if (v->particlePipeline) v->vkDestroyPipeline(v->device, v->particlePipeline, NULL);
        for (int i = 0; i < 2; ++i)
            if (v->overdrawPipelines[i]) v->vkDestroyPipeline(v->device, v->overdrawPipelines[i], NULL);
        bufferFree(v, &v->meshletIds);
        const VkPipeline backgroundPipelines[5] = {v->backgroundPipeline, v->bloomFirstPipeline, v->bloomNextPipeline, v->bloomUpPipeline, v->backdropPipeline};
        for (int i = 0; i < 5; ++i)
            if (backgroundPipelines[i]) v->vkDestroyPipeline(v->device, backgroundPipelines[i], NULL);
        if (v->sampler) v->vkDestroySampler(v->device, v->sampler, NULL);
        if (v->depthSampler) v->vkDestroySampler(v->device, v->depthSampler, NULL);
        if (v->descriptorPool) v->vkDestroyDescriptorPool(v->device, v->descriptorPool, NULL);
        if (v->pipelineLayout) v->vkDestroyPipelineLayout(v->device, v->pipelineLayout, NULL);
        if (v->setLayout) v->vkDestroyDescriptorSetLayout(v->device, v->setLayout, NULL);
        if (v->rolePool) v->vkDestroyDescriptorPool(v->device, v->rolePool, NULL);
        if (v->roleLayout) v->vkDestroyDescriptorSetLayout(v->device, v->roleLayout, NULL);
        if (v->renderPass) v->vkDestroyRenderPass(v->device, v->renderPass, NULL);
        if (v->earlyPass) v->vkDestroyRenderPass(v->device, v->earlyPass, NULL);
        if (v->latePass) v->vkDestroyRenderPass(v->device, v->latePass, NULL);
        if (v->commandPool) v->vkDestroyCommandPool(v->device, v->commandPool, NULL);
        pipelineCacheSave(v);
        if (v->pipelineCache) v->vkDestroyPipelineCache(v->device, v->pipelineCache, NULL);
        if (v->timestamps) v->vkDestroyQueryPool(v->device, v->timestamps, NULL);
        v->vkDestroyDevice(v->device, NULL);
    }
    if (v->surface) v->vkDestroySurfaceKHR(v->instance, v->surface, NULL);
    if (v->instance) v->vkDestroyInstance(v->instance, NULL);
#ifdef _WIN32
    if (v->library) FreeLibrary(v->library);
#else
    if (v->library) dlclose(v->library);
#endif
    artfFree(v->pipelineCachePath);
    artfVecFree(&v->meshes);
    artfVecFree(&v->vertexSpans);
    artfVecFree(&v->indexSpans);
    artfVecFree(&v->colorSpans);
    artfVecFree(&v->textureMarks);
    artfVecFree(&v->textureHeld);
    artfVecFree(&v->copyStarts);
    artfVecFree(&v->layers);
    artfVecFree(&v->posedCopies);
    artfVecFree(&v->pastCopies);
    artfVecFree(&v->overlayDamage);
    artfFree(v);
}

static const ArtfRendererVtbl vkTable = {vkDescription, vkResize, vkSetOverlay, vkDraw, vkReadPixels, vkPixels, vkStats, vkFree};

bool loadLibrary(Vulkan* v) {
#ifdef _WIN32
    v->library = LoadLibraryA("vulkan-1.dll");
    if (!v->library) return false;
    *(void**)&v->getInstanceProcAddr = (void*)GetProcAddress(v->library, "vkGetInstanceProcAddr");
#else
#ifdef __APPLE__
    static const char* const names[] = {"libvulkan.1.dylib", "libMoltenVK.dylib"};
#else
    static const char* const names[] = {"libvulkan.so.1", "libvulkan.so"};
#endif
    for (size_t i = 0; i < ARTF_COUNT(names) && !v->library; ++i) v->library = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!v->library) return false;
    *(void**)&v->getInstanceProcAddr = dlsym(v->library, "vkGetInstanceProcAddr");
#endif
    return v->getInstanceProcAddr != NULL;
}

bool createSurface(Vulkan* v) {
    typedef VkResult(VKAPI * Create)(VkInstance, const void*, const void*, VkSurfaceKHR*);
    Create create;
    const ArtfSurface* n = &v->native;
    switch (n->kind) {
    case ARTF_SURFACE_WIN32: {
#ifdef _WIN32
        *(void**)&create = v->getInstanceProcAddr(v->instance, "vkCreateWin32SurfaceKHR");
        const Win32SurfaceCreateInfo info = {ST_WIN32_SURFACE, NULL, 0, (void*)GetModuleHandleA(NULL), n->a};
        return create && create(v->instance, &info, NULL, &v->surface) == VK_OK;
#else
        return false;
#endif
    }
    case ARTF_SURFACE_XLIB: {
        *(void**)&create = v->getInstanceProcAddr(v->instance, "vkCreateXlibSurfaceKHR");
        const XlibSurfaceCreateInfo info = {ST_XLIB_SURFACE, NULL, 0, n->a, (unsigned long)(uintptr_t)n->b};
        return create && create(v->instance, &info, NULL, &v->surface) == VK_OK;
    }
    case ARTF_SURFACE_WAYLAND: {
        *(void**)&create = v->getInstanceProcAddr(v->instance, "vkCreateWaylandSurfaceKHR");
        const WaylandSurfaceCreateInfo info = {ST_WAYLAND_SURFACE, NULL, 0, n->a, n->b};
        return create && create(v->instance, &info, NULL, &v->surface) == VK_OK;
    }
    case ARTF_SURFACE_ANDROID: {
        *(void**)&create = v->getInstanceProcAddr(v->instance, "vkCreateAndroidSurfaceKHR");
        const AndroidSurfaceCreateInfo info = {ST_ANDROID_SURFACE, NULL, 0, n->a};
        return create && create(v->instance, &info, NULL, &v->surface) == VK_OK;
    }
    case ARTF_SURFACE_METAL: {
        *(void**)&create = v->getInstanceProcAddr(v->instance, "vkCreateMetalSurfaceEXT");
        const MetalSurfaceCreateInfo info = {ST_METAL_SURFACE, NULL, 0, n->a};
        return create && create(v->instance, &info, NULL, &v->surface) == VK_OK;
    }
    default:
        return true;
    }
}

bool createInstance(Vulkan* v) {
    *(void**)&v->vkCreateInstance = v->getInstanceProcAddr(NULL, "vkCreateInstance");
    if (!v->vkCreateInstance) return false;
    const char* extensions[4];
    uint32_t count = 0;
    VkFlags flags = 0;
    if (v->native.kind != ARTF_SURFACE_NONE && v->displayWanted != ARTF_DISPLAY_SRGB) {
        /* (another screen's colors, where the instance has them) */
        VkResult(VKAPI * enumerate)(const char*, uint32_t*, ExtensionProperties*);
        *(void**)&enumerate = v->getInstanceProcAddr(NULL, "vkEnumerateInstanceExtensionProperties");
        ExtensionProperties have[128];
        uint32_t n = 128;
        if (enumerate && enumerate(NULL, &n, have) >= 0)
            for (uint32_t e = 0; e < n; ++e)
                if (!strcmp(have[e].name, "VK_EXT_swapchain_colorspace")) v->colorspaces = true;
        if (v->colorspaces) extensions[count++] = "VK_EXT_swapchain_colorspace";
    }
    if (v->native.kind != ARTF_SURFACE_NONE) {
        extensions[count++] = "VK_KHR_surface";
        switch (v->native.kind) {
        case ARTF_SURFACE_WIN32: extensions[count++] = "VK_KHR_win32_surface"; break;
        case ARTF_SURFACE_XLIB: extensions[count++] = "VK_KHR_xlib_surface"; break;
        case ARTF_SURFACE_WAYLAND: extensions[count++] = "VK_KHR_wayland_surface"; break;
        case ARTF_SURFACE_ANDROID: extensions[count++] = "VK_KHR_android_surface"; break;
        default:
            extensions[count++] = "VK_EXT_metal_surface";
            extensions[count++] = "VK_KHR_portability_enumeration";
            flags = 1;
            break;
        }
    }
    const ApplicationInfo app = {ST_APPLICATION_INFO, NULL, "artful", 1, "artful", 1, 1u << 22 | 2u << 12};
    const InstanceCreateInfo info = {ST_INSTANCE_CREATE_INFO, NULL, flags, &app, 0, NULL, count, extensions};
    if (v->vkCreateInstance(&info, NULL, &v->instance) != VK_OK) return false;
#define X(name, result, args) *(void**)&v->name = v->getInstanceProcAddr(v->instance, #name);
    ARTF_VK_INSTANCE_FUNCTIONS(X)
#undef X
    return v->vkEnumeratePhysicalDevices != NULL;
}

/* The best device with what this backend needs; its queue family. */
bool chooseDevice(Vulkan* v, PhysicalDeviceProperties* chosen) {
    VkPhysicalDevice gpus[16];
    uint32_t count = 16;
    if (v->vkEnumeratePhysicalDevices(v->instance, &count, gpus) < 0) return false;
    int best = -1;
    for (uint32_t i = 0; i < count; ++i) {
        PhysicalDeviceProperties props;
        v->vkGetPhysicalDeviceProperties(gpus[i], &props);
        if (props.apiVersion < (1u << 22 | 2u << 12)) {
            artfLog("artful: %s has only Vulkan %u.%u", props.deviceName, props.apiVersion >> 22, props.apiVersion >> 12 & 1023);
            continue;
        }
        Vulkan12Features f12;
        memset(&f12, 0, sizeof f12);
        f12.sType = ST_VULKAN12_FEATURES;
        PhysicalDeviceFeatures2 features;
        memset(&features, 0, sizeof features);
        features.sType = ST_PHYSICAL_DEVICE_FEATURES2;
        features.pNext = &f12;
        v->vkGetPhysicalDeviceFeatures2(gpus[i], &features);
        /* the ray tracing hardware: both extensions, and what they need (ARTFUL_NO_RAY_QUERY: as a device without) */
        bool rayQuery = false, deviceFault = false, nonSemantic = false, postDepth = false;
        {
            ExtensionProperties* have = artfMalloc(512 * sizeof *have);
            uint32_t extensionCount = 512;
            int found = 0;
            if (v->vkEnumerateDeviceExtensionProperties && v->vkEnumerateDeviceExtensionProperties(gpus[i], NULL, &extensionCount, have) >= 0)
                for (uint32_t e = 0; e < extensionCount; ++e)
                    found |= (!strcmp(have[e].name, "VK_KHR_acceleration_structure") ? 1 : 0) | (!strcmp(have[e].name, "VK_KHR_ray_query") ? 2 : 0) | (!strcmp(have[e].name, "VK_KHR_deferred_host_operations") ? 4 : 0) |
                             (!strcmp(have[e].name, "VK_EXT_device_fault") ? 8 : 0) | (!strcmp(have[e].name, "VK_KHR_shader_non_semantic_info") ? 16 : 0) |
                             (!strcmp(have[e].name, "VK_EXT_post_depth_coverage") ? 32 : 0);
            nonSemantic = (found & 16) != 0;
            postDepth = (found & 32) != 0 && !getenv("ARTFUL_NO_POST_DEPTH");
            artfFree(have);
            if (found & 8) {
                FaultFeatures fault = {ST_FAULT_FEATURES, NULL, 0, 0};
                PhysicalDeviceFeatures2 all;
                memset(&all, 0, sizeof all);
                all.sType = ST_PHYSICAL_DEVICE_FEATURES2;
                all.pNext = &fault;
                v->vkGetPhysicalDeviceFeatures2(gpus[i], &all);
                deviceFault = fault.deviceFault != 0;
            }
            if ((found & 7) == 7 && !getenv("ARTFUL_NO_RAY_QUERY")) {
                RayQueryFeatures query = {ST_RAY_QUERY_FEATURES, NULL, 0};
                AsFeatures as = {ST_AS_FEATURES, &query, 0, 0, 0, 0, 0};
                Vulkan12Features more;
                memset(&more, 0, sizeof more);
                more.sType = ST_VULKAN12_FEATURES;
                more.pNext = &as;
                PhysicalDeviceFeatures2 all;
                memset(&all, 0, sizeof all);
                all.sType = ST_PHYSICAL_DEVICE_FEATURES2;
                all.pNext = &more;
                v->vkGetPhysicalDeviceFeatures2(gpus[i], &all);
                rayQuery = query.rayQuery && as.accelerationStructure && more.rest[8] /* bufferDeviceAddress */;
            }
        }
        if (!features.features.drawIndirectFirstInstance || !f12.shaderSampledImageArrayNonUniformIndexing) {
            artfLog("artful: %s lacks drawIndirectFirstInstance or non-uniform texture indexing", props.deviceName);
            continue;
        }

        QueueFamilyProperties families[32];
        uint32_t familyCount = 32;
        v->vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &familyCount, families);
        int family = -1;
        for (uint32_t q = 0; q < familyCount && family < 0; ++q) {
            if ((families[q].flags & 3) != 3) continue; /* graphics and compute */
            VkBool32 present = 1;
            if (v->surface) v->vkGetPhysicalDeviceSurfaceSupportKHR(gpus[i], q, v->surface, &present);
            if (present) family = (int)q;
        }
        if (family < 0) continue;
        const int score = props.deviceType == 2 ? 3 : props.deviceType == 1 ? 2 : 1; /* discrete, integrated, others */
        if (score > best) {
            best = score;
            v->gpu = gpus[i];
            v->family = (uint32_t)family;
            /* (ARTFUL_SINGLE_DRAW: as a device without it, for tests) */
            v->multiDraw = features.features.multiDrawIndirect && !getenv("ARTFUL_SINGLE_DRAW");
            v->drawCounted = v->multiDraw && f12.drawIndirectCount;
            v->fragmentStores = features.features.fragmentStoresAndAtomics;
            v->rayQuery = rayQuery;
            v->deviceFault = deviceFault;
            v->depthCoverage = postDepth;
            /* (ARTFUL_SHADER_DEBUG: shaders that say which C built them, for RenderDoc: spirv.h's artfShaderDebugInfo) */
            v->shaderDebug = nonSemantic && getenv("ARTFUL_SHADER_DEBUG");
            v->sampleShading = features.features.sampleRateShading;
            v->independentBlend = features.features.independentBlend;
            v->primitiveIds = features.features.geometryShader && !getenv("ARTFUL_NO_PRIMITIVE_ID"); /* (for the meshlets view) */
            v->takesEtc = features.features.textureCompressionETC2 && !getenv("ARTFUL_NO_BLOCKS");
            v->takesAstc = features.features.textureCompressionASTC_LDR && !getenv("ARTFUL_NO_BLOCKS");
            v->takesBc = features.features.textureCompressionBC && !getenv("ARTFUL_NO_BLOCKS");
            /* (a few samplers stay for the overlay, the depth buffer and what the driver keeps) */
            const uint32_t samplers = props.maxPerStageDescriptorSamplers > 8 ? props.maxPerStageDescriptorSamplers - 8 : 1;
            v->maxTextures = samplers < MAX_TEXTURES ? samplers : MAX_TEXTURES;
            *chosen = props;
        }
    }
    return best >= 0;
}

/* BGRA, as the frames in memory are; or else RGBA, as Android offers. Shaders write colors, not bytes,
 * so nothing else changes. */
/* Another screen's (ArtfRendererOptions.output): HDR10 in 10 bits (A2B10G10R10, read back as A2R10G10B10), Display-P3 in
 * 8; on a surface, where it offers that color space (VK_EXT_swapchain_colorspace), else sRGB. */
static void chooseDisplay(Vulkan* v, const SurfaceFormat* formats, uint32_t count) {
    v->display = ARTF_DISPLAY_SRGB;
    if (v->displayWanted == ARTF_DISPLAY_SRGB) return;
    if (!v->surface) {
        v->display = v->displayWanted;
        if (v->display == ARTF_DISPLAY_HDR10) v->colorFormat = FORMAT_A2B10G10R10;
        return;
    }
    const int space = v->displayWanted == ARTF_DISPLAY_HDR10 ? 1000104008 /* HDR10_ST2084 */ : 1000104001 /* DISPLAY_P3_NONLINEAR */;
    for (uint32_t i = 0; v->colorspaces && i < count; ++i) {
        const int f = formats[i].format;
        const bool fits = v->displayWanted == ARTF_DISPLAY_HDR10 ? f == FORMAT_A2B10G10R10 || f == FORMAT_A2R10G10B10 : f == FORMAT_BGRA8 || f == FORMAT_RGBA8;
        if (fits && formats[i].colorSpace == space) {
            v->colorFormat = f;
            v->colorSpace = space;
            v->display = v->displayWanted;
            return;
        }
    }
    artfLog("artful: the surface does not show %s: sRGB", v->displayWanted == ARTF_DISPLAY_HDR10 ? "HDR10" : "Display-P3");
}

bool chooseFormat(Vulkan* v) {
    v->colorFormat = FORMAT_BGRA8;
    v->colorSpace = 0;
    SurfaceFormat formats[64];
    uint32_t count = 64;
    if (v->surface && v->vkGetPhysicalDeviceSurfaceFormatsKHR(v->gpu, v->surface, &count, formats) < 0) return false;
    chooseDisplay(v, formats, v->surface ? count : 0);
    v->base.output = (ArtfOutput)v->display;
    if (v->display == ARTF_DISPLAY_SRGB) v->displayInverse = v->displayPeak = 1.f;
    if (!v->surface || v->display != ARTF_DISPLAY_SRGB) return true;
    int found = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (formats[i].format == FORMAT_BGRA8) { v->colorSpace = formats[i].colorSpace; return true; }
        if (formats[i].format == FORMAT_RGBA8 && !found) { found = 1; v->colorSpace = formats[i].colorSpace; }
    }
    if (found) v->colorFormat = FORMAT_RGBA8;
    return found != 0;
}

/* The device is lost (result: what said so): nothing more is drawn. Where VK_EXT_device_fault is on, the driver is
 * asked why, and what it says goes to the log and to ARTF_EVENT_DEVICE_LOST: its words, each place on the GPU that was
 * at fault (read, written, run) and the vendor's codes. */
/* Devices the driver said were lost in this process (not those a test made up): past ARTF_VULKAN_LOSSES_MOST, no more
 * renderers on the GPU (artfVulkanNew), so that what loses it each time does not reset the driver again and again:
 * a few resets in a row can leave Windows unable to bring the screen back. ARTFUL_KEEP_GPU: no such limit. */
uint32_t artfVulkanDevicesLost = 0;

void lose(Vulkan* v, VkResult result) {
    v->lost = true;
    if (result == -4 /* VK_ERROR_DEVICE_LOST */) ++artfVulkanDevicesLost;
    if (result != -4 /* VK_ERROR_DEVICE_LOST */ || !v->deviceFault || !v->vkGetDeviceFaultInfoEXT || v->base.lostWhy[0]) return;
    FaultCounts counts = {ST_FAULT_COUNTS, NULL, 0, 0, 0};
    if (v->vkGetDeviceFaultInfoEXT(v->device, &counts, NULL) < 0) return;
    if (counts.addressCount > 16) counts.addressCount = 16;
    if (counts.vendorCount > 4) counts.vendorCount = 4;
    counts.binarySize = 0; /* (not asked for at the device's making) */
    FaultAddress addresses[16];
    FaultVendor vendors[4];
    FaultInfo info;
    memset(&info, 0, sizeof info);
    info.sType = ST_FAULT_INFO;
    info.addresses = addresses;
    info.vendors = vendors;
    const VkResult got = v->vkGetDeviceFaultInfoEXT(v->device, &counts, &info);
    if (got < 0) return;
    char* out = v->base.lostWhy;
    const size_t size = sizeof v->base.lostWhy;
    size_t n = (size_t)snprintf(out, size, "%s", info.description[0] ? info.description : "no words for it");
    static const char* const kinds[7] = {"none", "a read of", "a write to", "running", "running near", "running, at no place it could", "running, at"};
    for (uint32_t k = 0; k < counts.addressCount && n < size; ++k)
        n += (size_t)snprintf(out + n, size - n, "; %s 0x%llx (to %llu bytes)", addresses[k].type >= 0 && addresses[k].type < 7 ? kinds[addresses[k].type] : "?",
                              (unsigned long long)addresses[k].address, (unsigned long long)addresses[k].precision);
    for (uint32_t k = 0; k < counts.vendorCount && n < size; ++k)
        n += (size_t)snprintf(out + n, size - n, "; %s (0x%llx, 0x%llx)", vendors[k].description, (unsigned long long)vendors[k].code, (unsigned long long)vendors[k].data);
    artfLog("artful: what the driver says of the lost device: %s", out);
}

bool artfVulkanFaultReports(const ArtfRenderer* r) { return r && r->backend == ARTF_BACKEND_VULKAN && ((const Vulkan*)r)->deviceFault; }

bool createDevice(Vulkan* v) {
    const float priority = 1.f;
    const DeviceQueueCreateInfo queue = {ST_DEVICE_QUEUE_CREATE_INFO, NULL, 0, v->family, 1, &priority};
    Vulkan12Features f12;
    memset(&f12, 0, sizeof f12);
    f12.sType = ST_VULKAN12_FEATURES;
    f12.shaderSampledImageArrayNonUniformIndexing = 1;
    f12.drawIndirectCount = v->drawCounted;
    PhysicalDeviceFeatures2 features;
    memset(&features, 0, sizeof features);
    features.sType = ST_PHYSICAL_DEVICE_FEATURES2;
    features.pNext = &f12;
    features.features.multiDrawIndirect = v->multiDraw;
    /* (ARTFUL_ROBUST, as the tests set it: a read past a buffer gives 0, not a fault, so a shader's mistake does not
     * take the device down) */
    features.features.robustBufferAccess = getenv("ARTFUL_ROBUST") != NULL;
    features.features.textureCompressionETC2 = v->takesEtc;
    features.features.textureCompressionASTC_LDR = v->takesAstc;
    features.features.textureCompressionBC = v->takesBc;
    features.features.drawIndirectFirstInstance = 1;
    features.features.fragmentStoresAndAtomics = v->fragmentStores;
    features.features.sampleRateShading = v->sampleShading;
    features.features.independentBlend = v->independentBlend;
    features.features.geometryShader = v->primitiveIds;
    const char* extensions[10];
    uint32_t extensionCount = 0;
    if (v->surface) extensions[extensionCount++] = "VK_KHR_swapchain";
    if (v->depthCoverage) extensions[extensionCount++] = "VK_EXT_post_depth_coverage";
    if (v->surface && v->native.kind == ARTF_SURFACE_METAL) extensions[extensionCount++] = "VK_KHR_portability_subset";
    RayQueryFeatures query = {ST_RAY_QUERY_FEATURES, NULL, 1};
    AsFeatures as = {ST_AS_FEATURES, &query, 1, 0, 0, 0, 0};
    if (v->rayQuery) {
        extensions[extensionCount++] = "VK_KHR_acceleration_structure";
        extensions[extensionCount++] = "VK_KHR_ray_query";
        extensions[extensionCount++] = "VK_KHR_deferred_host_operations";
        f12.rest[8] = 1; /* bufferDeviceAddress */
        f12.pNext = &as;
    }
    if (v->shaderDebug) {
        extensions[extensionCount++] = "VK_KHR_shader_non_semantic_info";
        artfShaderDebugInfo = true; /* (for every renderer after: a process that asks for it debugs) */
        artfLog("artful: shaders carry where in C each is built (ARTFUL_SHADER_DEBUG)");
    }
    FaultFeatures fault = {ST_FAULT_FEATURES, &f12, 1, 0};
    if (v->deviceFault) {
        extensions[extensionCount++] = "VK_EXT_device_fault";
        features.pNext = &fault;
    }
    const DeviceCreateInfo info = {ST_DEVICE_CREATE_INFO, &features, 0, 1, &queue, 0, NULL, extensionCount, extensions, NULL};
    if (v->vkCreateDevice(v->gpu, &info, NULL, &v->device) != VK_OK) return false;
#define X(name, result, args) *(void**)&v->name = v->vkGetDeviceProcAddr(v->device, #name);
    ARTF_VK_DEVICE_FUNCTIONS(X)
#undef X
    v->vkGetDeviceQueue(v->device, v->family, 0, &v->queue);
    v->vkGetPhysicalDeviceMemoryProperties(v->gpu, &v->memory);
    return true;
}

ArtfRenderer* artfVulkanNew(const ArtfRendererOptions* o) {
    if (artfVulkanDevicesLost >= ARTF_VULKAN_LOSSES_MOST && !getenv("ARTFUL_KEEP_GPU")) {
        artfLog("artful: the GPU was lost %u times in this process: no renderer on it again (ARTFUL_KEEP_GPU: go on)", artfVulkanDevicesLost);
        return NULL;
    }
    Vulkan* v = ARTF_NEW(Vulkan);
    v->base.vt = &vkTable;
    v->base.backend = ARTF_BACKEND_VULKAN;
    v->native = o->surface;
    v->width = o->width;
    v->height = o->height;
    v->noVsync = o->noVsync;
    v->samples = o->antialias ? 4 : 1;
    v->floats = o->hdr;
    v->displayWanted = o->hdr && o->output >= ARTF_OUTPUT_SRGB && o->output <= ARTF_OUTPUT_HDR10 ? (int)o->output : ARTF_DISPLAY_SRGB;
    artfDisplayNumbers(o, (ArtfOutput)v->displayWanted, &v->displayInverse, &v->displayPeak);
    v->renderScale = o->hdr && o->renderScale > 0 && o->renderScale < 1.f ? artfMaxf(o->renderScale, .25f) : 1.f;
    v->preRotate = o->preRotate;
    v->base.renderScale = v->base.renderScaleMost = v->renderScale;
    v->compressTextures = o->compressTextures;
    PhysicalDeviceProperties props;
    if (o->pipelineCache) {
        v->pipelineCachePath = artfMalloc(strlen(o->pipelineCache) + 1);
        strcpy(v->pipelineCachePath, o->pipelineCache);
    }
    const char* missing = !loadLibrary(v) ? "no Vulkan library" : !createInstance(v) ? "no instance" : !createSurface(v) ? "no surface" : !chooseDevice(v, &props) ? "no device with what it needs" : !createDevice(v) ? "no logical device" : !chooseFormat(v) ? "no format for the surface" : NULL;
    if (missing) {
        artfLog("artful: Vulkan cannot run here: %s", missing);
        goto failed;
    }
    snprintf(v->description, sizeof v->description, "%s", props.deviceName);
    if (v->floats && !v->independentBlend) {
        artfLog("artful: this device cannot draw a frame in floats (no independentBlend)");
        goto failed;
    }
    v->sceneFormat = v->floats ? FORMAT_RGBA16_FLOAT : v->colorFormat;
    /* (timestampComputeAndGraphics and timestampPeriod, where the limits have them) */
    uint32_t timestampsThere;
    memcpy(&timestampsThere, props.rest + 348, 4);
    memcpy(&v->timestampPeriod, props.rest + 352, 4);

    const CommandPoolCreateInfo pool = {ST_COMMAND_POOL, NULL, 2 /* each buffer resets alone */, v->family};
    if (v->vkCreateCommandPool(v->device, &pool, NULL, &v->commandPool) != VK_OK) goto failed;
    const FenceCreateInfo fence = {ST_FENCE_CREATE_INFO, NULL, 1 /* signaled */};
    const SemaphoreCreateInfo semaphore = {ST_SEMAPHORE_CREATE_INFO, NULL, 0};
    for (int i = 0; i < SLOTS; ++i) {
        Slot* s = &v->slots[i];
        const CommandBufferAllocateInfo alloc = {ST_COMMAND_BUFFER_ALLOCATE, NULL, v->commandPool, 0, 1};
        if (v->vkAllocateCommandBuffers(v->device, &alloc, &s->commands) != VK_OK) goto failed;
        if (v->vkCreateFence(v->device, &fence, NULL, &s->fence) != VK_OK) goto failed;
        if (v->vkCreateSemaphore(v->device, &semaphore, NULL, &s->acquired) != VK_OK) goto failed;
        s->fresh = true;
    }
    for (int i = 0; i < MAX_SWAPCHAIN_IMAGES; ++i)
        if (v->surface && v->vkCreateSemaphore(v->device, &semaphore, NULL, &v->rendered[i]) != VK_OK) goto failed;
    if (!pipelinesMake(v)) {
        artfLog("artful: the driver refused a pipeline");
        goto failed;
    }
    if (v->floats) {
        /* (what the vertices were: nothing yet, but the vertex shader reads it where a pose bent something) */
        if (!bufferNew(v, &v->pastVertices, 16, BUFFER_STORAGE | BUFFER_TRANSFER_DST, false)) goto failed;
        for (int i = 0; i < SLOTS; ++i) bindStorage(v, &v->slots[i], 68, &v->pastVertices);
    }
    if (timestampsThere && v->timestampPeriod > 0) {
        const QueryPoolCreateInfo queries = {11 /* ST_QUERY_POOL */, NULL, 0, 2 /* timestamps */, SLOTS * QUERIES, 0};
        if (v->vkCreateQueryPool(v->device, &queries, NULL, &v->timestamps) != VK_OK) v->timestamps = 0;
    }

    /* room to start with; each grows as the scene does */
    v->instanceCapacity = 1024;
    v->materialCapacity = 64;
    for (int i = 0; i < SLOTS; ++i) {
        Slot* s = &v->slots[i];
        if (!bufferNew(v, &s->frame, sizeof(ArtfFrameData), BUFFER_UNIFORM, true)) goto failed;
        /* (the lights past the frame's, the profiles of its own, and the highlight's linearly transformed cosines,
         * written once) */
        const size_t profilesEnd = (size_t)ARTF_MAX_LIGHTS * sizeof(ArtfLightData) + ARTF_FRAME_LIGHTS * ARTF_PROFILE_FLOATS * sizeof(float);
        if (!bufferNew(v, &s->extraLights, (VkDeviceSize)(profilesEnd + sizeof artfLtcTable), BUFFER_STORAGE, true)) goto failed;
        memcpy((char*)s->extraLights.mapped + profilesEnd, artfLtcTable, sizeof artfLtcTable);
        if (!bufferNew(v, &s->decals, (VkDeviceSize)ARTF_MAX_DECALS * sizeof(ArtfDecalData), BUFFER_STORAGE, true)) goto failed;
        if (!bufferNew(v, &s->tileLights, 4, BUFFER_STORAGE, true)) goto failed;
        if (!bufferNew(v, &s->probes, 16, BUFFER_STORAGE, true)) goto failed;
        if (!bufferNew(v, &s->background, sizeof(ArtfBackgroundBlock), BUFFER_UNIFORM, true)) goto failed;
        if (!bufferNew(v, &s->trace, sizeof(ArtfTraceFrame), BUFFER_UNIFORM, true)) goto failed;
        if (!bufferNew(v, &s->shadow, sizeof(ArtfShadowFrame), BUFFER_UNIFORM, true)) goto failed;
        if (!bufferNew(v, &s->stream, 16, BUFFER_STORAGE, true)) goto failed;
        if (!bufferNew(v, &s->instances, (VkDeviceSize)v->instanceCapacity * (sizeof(ArtfInstanceData) + 64), BUFFER_STORAGE, true)) goto failed;
        if (!bufferNew(v, &s->visible, visibleSize(v), BUFFER_STORAGE | BUFFER_TRANSFER_DST, false)) goto failed;
        if (!bufferNew(v, &s->materials, (VkDeviceSize)v->materialCapacity * (sizeof(ArtfMaterialData) + sizeof(ArtfMaterialMore)), BUFFER_STORAGE, true)) goto failed;
        if (!bufferNew(v, &s->indirect, 64 * 80, BUFFER_INDIRECT | BUFFER_STORAGE | BUFFER_TRANSFER_DST, false)) goto failed;
        if (!bufferNew(v, &s->template_, 64 * 80, BUFFER_TRANSFER_SRC, true)) goto failed;
    }
    v->meshCapacity = 64;
    if (!bufferNew(v, &v->meshTable, 64 * 80, BUFFER_STORAGE, true)) goto failed;
    if (!bufferNew(v, &v->colors, 16, BUFFER_STORAGE | BUFFER_TRANSFER_DST, false) || !colorsReset(v)) goto failed;
    if (!flagsMake(v)) goto failed;
    for (int i = 0; i < SLOTS; ++i) writeBufferDescriptors(v, &v->slots[i]);
    if (!shadowAtlasMake(v, 1, 1)) goto failed;
    if (!targetsMake(v)) goto failed;
    return &v->base;

failed:
    vkFree(&v->base);
    return NULL;
}

void artfVulkanTargetBytes(const ArtfRenderer* r, uint64_t* bytes) {
    const Vulkan* v = (const Vulkan*)r;
    for (int t = 0; t < FG_TARGETS; ++t) bytes[t] = 0;
    if (!r || r->backend != ARTF_BACKEND_VULKAN) return;
    bytes[FG_FLOAT_COLOR] = v->floatColor.held;
    bytes[FG_FLOAT_SURFACE] = v->floatSurface.held;
    bytes[FG_DEPTH_IMAGE] = v->depth.held;
    bytes[FG_HIZ] = v->hiz.held;
    bytes[FG_FOG] = v->postFog.held;
    bytes[FG_LIGHT] = v->postLight.held;
    bytes[FG_SECOND] = v->postScratch.held;
    bytes[FG_HISTORY] = v->postHistory.held;
    bytes[FG_DEPTH] = v->postDepth.held;
    bytes[FG_SURFACE] = v->postSurface.held;
    bytes[FG_OCCLUSION] = v->postOcclusion.held;
    bytes[FG_BLOOM] = v->postBloom.held;
    bytes[FG_BLUR] = 0; /* (they share ambient occlusion's buffer) */
    bytes[FG_LOCKS] = v->postLocks.held;
    bytes[FG_GATHERED] = v->postGathered.held;
    bytes[FG_FRAME] = v->offscreen.held;
}

void artfVulkanPostBarriers(const ArtfRenderer* r, uint32_t* barriers, uint32_t* dispatches) {
    const Vulkan* v = (const Vulkan*)r;
    *barriers = r && r->backend == ARTF_BACKEND_VULKAN ? v->postBarriers : 0;
    *dispatches = r && r->backend == ARTF_BACKEND_VULKAN ? v->postDispatches : 0;
}
