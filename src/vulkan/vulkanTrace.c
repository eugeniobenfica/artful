/* artful — the Vulkan backend: tracing (light from all around, the ray tracing hardware's structures, the trees on the
 * GPU, the tracing shader's buffers, denoising). Part of vulkan.c, split by what it does. */
#include "vulkan/vulkanInternal.h"

/* ---- light from all around ---- */

/* The scene's maps go up when they change; until a scene has some, a buffer of four bytes stands in, which the
 * shaders do not read. */
bool syncEnvironment(Vulkan* v, const ArtfScene* scene) {
    const VkDeviceSize full = ARTF_ENV_FLOATS * sizeof(float);
    const bool fresh = scene->environment && (v->environmentVersion != scene->environmentVersion || v->environmentScene != scene->id);
    if (v->environment.buffer && !fresh) return true;
    if (v->environment.buffer) v->vkDeviceWaitIdle(v->device);
    if (!v->environment.buffer || (fresh && v->environment.size < full)) {
        bufferFree(v, &v->environment);
        if (!bufferNew(v, &v->environment, fresh ? full : 4, BUFFER_STORAGE | BUFFER_TRANSFER_DST, false)) return false;
        const DescriptorBufferInfo info = {v->environment.buffer, 0, ~(VkDeviceSize)0};
        for (int i = 0; i < SLOTS; ++i) {
            const WriteDescriptorSet w = {ST_WRITE_DESCRIPTOR_SET, NULL, v->slots[i].set, 49, 0, 1, DESCRIPTOR_STORAGE, NULL, &info, NULL};
            v->vkUpdateDescriptorSets(v->device, 1, &w, 0, NULL);
        }
    }
    if (fresh) {
        if (!upload(v, &v->environment, 0, scene->environment, full)) return false;
        v->environmentVersion = scene->environmentVersion;
        v->environmentScene = scene->id;
    }
    return true;
}

/* ---- tracing ---- */

/* ---- the ray tracing hardware: acceleration structures ---- */


/* A buffer the GPU knows by its address: where the CPU writes it (inputs), or the GPU's own (the structures, scratch). */
bool rtBufferNew(Vulkan* v, GpuBuffer* b, VkDeviceSize size, VkFlags usage, bool written) {
    memset(b, 0, sizeof *b);
    BufferCreateInfo info = {ST_BUFFER_CREATE_INFO, NULL, 0, size ? size : 16, usage | BUFFER_DEVICE_ADDRESS, 0, 0, NULL};
    if (v->vkCreateBuffer(v->device, &info, NULL, &b->buffer) != VK_OK) return false;
    MemoryRequirements req;
    v->vkGetBufferMemoryRequirements(v->device, b->buffer, &req);
    int type = written ? memoryType(v, req.typeBits, MEMORY_HOST_VISIBLE | MEMORY_HOST_COHERENT) : memoryType(v, req.typeBits, MEMORY_DEVICE_LOCAL);
    if (type < 0) return false;
    const MemoryAllocateFlagsInfo flags = {ST_MEMORY_ALLOCATE_FLAGS_INFO, NULL, 2 /* device address */, 0};
    MemoryAllocateInfo alloc = {ST_MEMORY_ALLOCATE_INFO, &flags, req.size, (uint32_t)type};
    if (v->vkAllocateMemory(v->device, &alloc, NULL, &b->memory) != VK_OK) return false;
    b->held = req.size;
    v->deviceBytes += req.size;
    v->vkBindBufferMemory(v->device, b->buffer, b->memory, 0);
    b->size = info.size;
    b->serial = ++v->bufferSerial;
    if (written && v->vkMapMemory(v->device, b->memory, 0, ~(VkDeviceSize)0, 0, &b->mapped) != VK_OK) return false;
    return true;
}

VkDeviceAddress rtAddress(Vulkan* v, const GpuBuffer* b) {
    const BufferDeviceAddressInfo info = {ST_BUFFER_DEVICE_ADDRESS_INFO, NULL, b->buffer};
    return v->vkGetBufferDeviceAddress(v->device, &info);
}

/* ---- tracing ---- */

/* One of the buffers rays read: replaced when it has outgrown its room. built: the vertices, with the hardware, which
 * its bottom levels are built from, by their address. */
bool traceBuffer(Vulkan* v, GpuBuffer* b, uint32_t binding, const void* data, VkDeviceSize size, bool built) {
    if (size > b->size || !b->buffer) {
        bufferFree(v, b);
        const VkFlags usage = BUFFER_STORAGE | BUFFER_TRANSFER_DST;
        if (built ? !rtBufferNew(v, b, size * 2, usage | BUFFER_AS_INPUT, false) : !bufferNew(v, b, size * 2, usage, false)) return false;
        const DescriptorBufferInfo info = {b->buffer, 0, ~(VkDeviceSize)0};
        for (int i = 0; i < SLOTS; ++i) {
            const WriteDescriptorSet w = {ST_WRITE_DESCRIPTOR_SET, NULL, v->slots[i].set, binding, 0, 1, DESCRIPTOR_STORAGE, NULL, &info, NULL};
            v->vkUpdateDescriptorSets(v->device, 1, &w, 0, NULL);
        }
    }
    return upload(v, b, 0, data, size);
}

/* Rays on the GPU go through the same trees as on the CPU, which makes them, as they change only when
 * the scene does: the scene's arrays go up as they are. The shader is large, so it is made only when a
 * scene first asks. It needs no ray tracing hardware. */





/* The structure goes once the frames that may read it are done; its room, with the block once that is empty. */
void rtRelease(Vulkan* v, RtBottom* b) {
    structureRetire(v, b->structure);
    if (b->room && b->block < v->rtBlocks.len) {
        RtBlock* k = &v->rtBlocks.items[b->block];
        if (k->live && --k->live == 0) {
            bufferRetire(v, &k->buffer);
            k->used = 0;
        }
    }
    memset(b, 0, sizeof *b);
}

/* now: the device is idle. */
void rtBottomsFree(Vulkan* v, bool now) {
    for (uint32_t i = 0; i < v->rtBottomCount; ++i) {
        if (now && v->rtBottoms[i].structure) v->vkDestroyAccelerationStructureKHR(v->device, v->rtBottoms[i].structure, NULL);
        else if (v->rtBottoms[i].structure) structureRetire(v, v->rtBottoms[i].structure);
    }
    for (size_t i = 0; i < v->rtBlocks.len; ++i) {
        if (now) bufferFree(v, &v->rtBlocks.items[i].buffer);
        else bufferRetire(v, &v->rtBlocks.items[i].buffer);
    }
    v->rtBlocks.len = 0;
    artfFree(v->rtBottoms);
    v->rtBottoms = NULL;
    v->rtBottomCount = 0;
}

void rtFree(Vulkan* v) {
    if (!v->rayQuery) return;
    rtBottomsFree(v, true);
    artfVecFree(&v->rtBlocks);
    for (int k = 0; k < SLOTS; ++k) {
        Slot* s = &v->slots[k];
        if (s->rtTopLevel) v->vkDestroyAccelerationStructureKHR(v->device, s->rtTopLevel, NULL);
        s->rtTopLevel = 0;
        s->rtTopRoom = 0;
        GpuBuffer* const tops[3] = {&s->rtInstances, &s->rtTop, &s->rtScratch};
        for (int i = 0; i < 3; ++i) bufferFree(v, tops[i]);
    }
    bufferFree(v, &v->rtIndices);
    bufferFree(v, &v->rtScratch);
}

/* A room of this size: at the end of a block with that much left, or in a new block, of at least `block`. */
bool rtRoom(Vulkan* v, RtBottom* b, VkDeviceSize size, VkDeviceSize block) {
    size = (size + 255) & ~(VkDeviceSize)255;
    uint32_t at = UINT32_MAX;
    for (uint32_t k = 0; k < v->rtBlocks.len && at == UINT32_MAX; ++k)
        if (v->rtBlocks.items[k].buffer.buffer && v->rtBlocks.items[k].buffer.size - v->rtBlocks.items[k].used >= size) at = k;
    if (at == UINT32_MAX) {
        for (uint32_t k = 0; k < v->rtBlocks.len && at == UINT32_MAX; ++k)
            if (!v->rtBlocks.items[k].buffer.buffer) at = k;
        if (at == UINT32_MAX) {
            const RtBlock none = {{0}, 0, 0};
            artfVecPush(&v->rtBlocks, none);
            at = (uint32_t)v->rtBlocks.len - 1;
        }
        RtBlock* k = &v->rtBlocks.items[at];
        k->used = 0;
        k->live = 0;
        if (!rtBufferNew(v, &k->buffer, block > size ? block : size, BUFFER_AS_STORAGE, false)) return false;
    }
    RtBlock* k = &v->rtBlocks.items[at];
    b->block = at;
    b->offset = k->used;
    b->room = size;
    k->used += size;
    ++k->live;
    return true;
}

/* Each mesh's corners, from its first vertex (so that they do not change as meshes come or go before it): written
 * anew when meshes come or go, into a buffer of their own, the one before retired. */
bool rtIndicesSync(Vulkan* v, const ArtfScene* scene) {
    const ArtfTrace* t = &scene->trace;
    if (v->rtIndices.buffer && v->rtIndexScene == scene->id && v->rtIndexRemovals == t->meshRemovals && v->rtIndexTriangles == t->triangles.len) return true;
    bufferRetire(v, &v->rtIndices);
    if (!rtBufferNew(v, &v->rtIndices, (VkDeviceSize)t->triangles.len * 12 + 12, BUFFER_AS_INPUT, true)) return false;
    uint32_t* corners = v->rtIndices.mapped;
    for (size_t m = 0; m < t->ranges.len; ++m) {
        const ArtfTraceRange* r = &t->ranges.items[m];
        for (uint32_t i = r->firstTriangle; i < r->firstTriangle + r->triangleCount; ++i)
            for (int c = 0; c < 3; ++c) corners[(size_t)i * 3 + (size_t)c] = t->triangles.items[i].v[c] - r->firstVertex;
    }
    v->rtIndexScene = scene->id;
    v->rtIndexRemovals = t->meshRemovals;
    v->rtIndexTriangles = t->triangles.len;
    return true;
}

/* A mesh's triangles as the hardware reads them: its corners, and the positions the trees have of its vertices (on the
 * GPU, binding 19, as this frame's commands leave them). */
AsGeometry rtTriangles(VkDeviceAddress vertices, VkDeviceAddress corners, const ArtfTraceRange* r, uint32_t vertexCount) {
    AsGeometry g;
    memset(&g, 0, sizeof g);
    g.sType = ST_AS_GEOMETRY;
    g.geometryType = 0; /* triangles */
    g.flags = 1;        /* opaque */
    AsGeometryTriangles* tri = &g.geometry.triangles;
    tri->sType = ST_AS_GEOMETRY_TRIANGLES;
    tri->vertexFormat = 106; /* R32G32B32_SFLOAT */
    tri->vertexData = vertices + (VkDeviceAddress)r->firstVertex * sizeof(ArtfTraceVertex);
    tri->vertexStride = sizeof(ArtfTraceVertex);
    tri->maxVertex = vertexCount ? vertexCount - 1 : 0;
    tri->indexType = 1; /* uint32 */
    tri->indexData = corners;
    return g;
}

_Static_assert(offsetof(ArtfTraceVertex, p) == 0, "the hardware reads the trees' vertices' positions where they are");

/* The diagonal of the box of a mesh's vertices, as the trees have them. */
float rtExtent(const ArtfTrace* t, const ArtfTraceRange* r, uint32_t vertexCount) {
    float lo[3] = {3e38f, 3e38f, 3e38f}, hi[3] = {-3e38f, -3e38f, -3e38f};
    for (uint32_t i = 0; i < vertexCount && r->firstVertex + i < t->vertices.len; ++i) {
        const float* p = t->vertices.items[r->firstVertex + i].p;
        for (int k = 0; k < 3; ++k) {
            lo[k] = p[k] < lo[k] ? p[k] : lo[k];
            hi[k] = p[k] > hi[k] ? p[k] : hi[k];
        }
    }
    if (lo[0] > hi[0]) return 0;
    return sqrtf((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]));
}

uint64_t artfVulkanBottomLevels[2];


/* The trees' lengths, as they are on the GPU: when they are the same, only poses changed them. */
bool traceLayoutSame(const Vulkan* v, const ArtfScene* scene) {
    const ArtfTrace* t = &scene->trace;
    return v->traceUp && v->traceScene == scene->id && v->traceRemovals == t->meshRemovals && v->traceLengths[0] == t->wide.len && v->traceLengths[1] == t->triangles.len &&
           v->traceLengths[2] == t->vertices.len && v->traceLengths[3] == t->ranges.len;
}

/* What bones or targets moved of the trees: each such mesh's nodes and vertices, from the slot's staging into the trees,
 * in the frame's commands. Recorded in v->traceCopies; false when out of memory. */
bool traceMovedUp(Vulkan* v, Slot* s, const ArtfScene* scene) {
    const ArtfTrace* t = &scene->trace;
    v->traceCopies[0].len = v->traceCopies[1].len = 0;
    VkDeviceSize bytes = 0;
    for (size_t i = 0; i < scene->deforming.len; ++i) {
        const uint32_t mesh = scene->deforming.items[i];
        if (mesh >= t->ranges.len || mesh >= v->traceUpCount) continue;
        const ArtfTraceRange* r = &t->ranges.items[mesh];
        TraceUp* up = &v->traceUp[mesh];
        if (up->deform == r->deformVersion && up->moved == r->moved) continue;
        up->deform = r->deformVersion;
        up->moved = r->moved;
        if (r->wideEnd > r->firstWide) {
            const BufferCopy n = {bytes, (VkDeviceSize)r->firstWide * sizeof(ArtfTraceWide), (VkDeviceSize)(r->wideEnd - r->firstWide) * sizeof(ArtfTraceWide)};
            artfVecPush(&v->traceCopies[0], n);
            bytes += n.size;
        }
        /* (as tracing wrote them: what its refit wrote, the GPU has) */
        const size_t first = r->firstVertex, count = scene->meshes.items[mesh].vertexCount;
        const size_t end = first + count < t->vertices.len ? first + count : t->vertices.len;
        if (end > first) {
            const BufferCopy p = {bytes, (VkDeviceSize)first * sizeof(ArtfTraceVertex), (VkDeviceSize)(end - first) * sizeof(ArtfTraceVertex)};
            artfVecPush(&v->traceCopies[1], p);
            bytes += p.size;
        }
    }
    if (!bytes) return true;
    if (s->traceStaging.size < bytes) {
        /* (what the slot held is done with: its fence was waited for) */
        bufferFree(v, &s->traceStaging);
        if (!bufferNew(v, &s->traceStaging, bytes + bytes / 2, BUFFER_TRANSFER_SRC, true)) return false;
    }
    const char* from[2] = {(const char*)t->wide.items, (const char*)t->vertices.items};
    for (int k = 0; k < 2; ++k)
        for (size_t i = 0; i < v->traceCopies[k].len; ++i) {
            const BufferCopy* b = &v->traceCopies[k].items[i];
            memcpy((char*)s->traceStaging.mapped + b->src, from[k] + b->dst, b->size);
        }
    return true;
}

/* What changed of the meshes rays meet, made so in the frame's own commands c, before anything reads it, with no wait
 * for the device: the trees' nodes and vertices of what bones or targets moved (all else that changes meshes went up at
 * once, waiting, in traceBuffersSync); and with the hardware, the bottom level of each mesh that came, changed or moved,
 * and of no other. What bones or targets move is built to allow an update, and refitted in place from this frame's
 * vertices; built again after RT_REFITS refits, or when its box has grown by half. A bottom level gone, or one that
 * outgrew its room, is retired: destroyed once the frames that read it are done. Frames before this one read what is
 * written here: the barrier first waits for them, on the GPU. */
bool slotMeshSync(Vulkan* v, Slot* s, VkCommandBuffer c, const ArtfScene* scene, bool hardware) {
    const ArtfTrace* t = &scene->trace;
    const bool trees = v->traceMeshVersion != t->meshVersion && traceLayoutSame(v, scene);
    const bool bottoms = hardware && (v->rtScene != scene->id || v->rtMeshVersion != t->meshVersion || !v->rtBottoms);
    if (!trees && !bottoms) return true;
    if (trees) {
        if (!traceMovedUp(v, s, scene)) return false;
        v->traceMeshVersion = t->meshVersion;
    }
    const bool copies = trees && (v->traceCopies[0].len || v->traceCopies[1].len);
    const VkFlags as = v->rayQuery ? STAGE_AS_BUILD : 0;
    const VkFlags readers = STAGE_COMPUTE | STAGE_FRAGMENT | as;

    RtPlan* plans = NULL;
    uint32_t planCount = 0;
    bool released = false;
    VkDeviceSize scratch = 0;
    if (bottoms) {
        if (v->rtScene != scene->id) rtBottomsFree(v, false);
        if (!rtIndicesSync(v, scene)) return false;
        const uint32_t meshes = (uint32_t)t->ranges.len;
        if (v->rtBottomCount < meshes || !v->rtBottoms) {
            v->rtBottoms = artfRealloc(v->rtBottoms, ((size_t)meshes + 1) * sizeof *v->rtBottoms);
            memset(v->rtBottoms + v->rtBottomCount, 0, ((size_t)meshes + 1 - v->rtBottomCount) * sizeof *v->rtBottoms);
            v->rtBottomCount = meshes;
        }
        /* (meshes come and go: the trees' arrays started over, and each mesh's triangles may be elsewhere in them) */
        const bool relaid = v->rtRemovals != t->meshRemovals;
        const VkDeviceAddress vertices = rtAddress(v, &v->traceVertices), corners = rtAddress(v, &v->rtIndices);
        plans = artfMalloc(((size_t)v->rtBottomCount + 1) * sizeof *plans);
        VkDeviceSize fresh = 0;
        for (uint32_t m = 0; m < v->rtBottomCount; ++m) {
            RtBottom* b = &v->rtBottoms[m];
            const ArtfTraceRange* r = m < meshes ? &t->ranges.items[m] : NULL;
            /* (a range past the arrays would have the device read what is not there: it is not built) */
            const bool within = r && m < scene->meshes.len && (uint64_t)r->firstTriangle + r->triangleCount <= t->triangles.len &&
                                (uint64_t)r->firstVertex + scene->meshes.items[m].vertexCount <= t->vertices.len;
            if (r && r->triangleCount && !within) artfLog("artful: mesh %u's triangles are not where tracing says: not traced by the hardware", m);
            if (!r || !r->triangleCount || !within) {
                if (b->structure) {
                    rtRelease(v, b);
                    released = true;
                }
                continue;
            }
            const ArtfMeshData* mesh = &scene->meshes.items[m];
            const bool bends = mesh->skin != NULL;
            /* (the same mesh, as it was built: what does not bend stays where it is in the trees or not, as its corners
             * are its own; what bends was put in the trees again in the pose it had, its triangles maybe in another order) */
            const bool same = b->structure && b->generation == mesh->generation && b->triangleCount == r->triangleCount && b->vertexCount == mesh->vertexCount && !(relaid && bends);
            if (same && b->deform == r->deformVersion) {
                b->firstTriangle = r->firstTriangle;
                b->firstVertex = r->firstVertex;
                continue;
            }
            RtPlan p = {m, false, false, 0, 0};
            const float extent = rtExtent(t, r, mesh->vertexCount);
            p.update = same && b->refittable && b->firstTriangle == r->firstTriangle && b->firstVertex == r->firstVertex && b->refits + 1 < RT_REFITS && extent <= b->extent * 1.5f;
            if (p.update) {
                p.scratch = b->updateScratch;
            } else {
                AsGeometry g = rtTriangles(vertices, corners, r, mesh->vertexCount);
                const AsBuildGeometryInfo build = {ST_AS_BUILD_GEOMETRY_INFO, NULL, 1 /* bottom */, 4 /* fast to trace */ | (bends ? 1u /* allow update */ : 0u), 0, 0, 0, 1, &g, NULL, 0};
                AsBuildSizes sizes = {ST_AS_BUILD_SIZES, NULL, 0, 0, 0};
                v->vkGetAccelerationStructureBuildSizesKHR(v->device, 1 /* on the device */, &build, &r->triangleCount, &sizes);
                p.size = sizes.structureSize;
                p.scratch = sizes.buildScratchSize;
                /* (built again where it was, if it fits: the frames before are done with it, by the barrier) */
                p.fresh = !b->structure || ((p.size + 255) & ~(VkDeviceSize)255) > b->room;
                if (p.fresh) fresh += (p.size + 255) & ~(VkDeviceSize)255;
                b->buildScratch = sizes.buildScratchSize;
                b->updateScratch = sizes.updateScratchSize;
                b->refittable = bends;
                b->extent = extent;
            }
            scratch += (p.scratch + 255) & ~(VkDeviceSize)255;
            plans[planCount++] = p;
        }
        /* rooms: those that are new go together in one block, with some to spare for meshes to come */
        for (uint32_t i = 0; i < planCount; ++i) {
            if (!plans[i].fresh) continue;
            RtBottom* b = &v->rtBottoms[plans[i].mesh];
            const RtBottom kept = *b;
            if (b->structure) rtRelease(v, b);
            b->buildScratch = kept.buildScratch;
            b->updateScratch = kept.updateScratch;
            b->refittable = kept.refittable;
            b->extent = kept.extent;
            if (!rtRoom(v, b, plans[i].size, fresh + fresh / 4 + (1u << 20))) {
                artfFree(plans);
                return false;
            }
            const AsCreateInfo create = {ST_AS_CREATE_INFO, NULL, 0, v->rtBlocks.items[b->block].buffer.buffer, b->offset, b->room, 1 /* bottom */, 0};
            if (v->vkCreateAccelerationStructureKHR(v->device, &create, NULL, &b->structure) != VK_OK) {
                artfFree(plans);
                return false;
            }
            const AsDeviceAddressInfo address = {ST_AS_DEVICE_ADDRESS_INFO, NULL, b->structure};
            b->address = v->vkGetAccelerationStructureDeviceAddressKHR(v->device, &address);
            /* (staggered: so that what bends is not all built again in the same frame) */
            b->refits = plans[i].mesh % RT_REFITS;
            released = true;
        }
        if (scratch > v->rtScratch.size) {
            bufferRetire(v, &v->rtScratch);
            if (!rtBufferNew(v, &v->rtScratch, scratch + scratch / 2, BUFFER_STORAGE, false)) {
                artfFree(plans);
                return false;
            }
        }
    }

    /* the frames before are done reading, and building, what is written now */
    memoryBarrier(v, c, readers | STAGE_TRANSFER, STAGE_TRANSFER | as, ACCESS_TRANSFER_WRITE | (as ? ACCESS_AS_WRITE : 0u),
                  ACCESS_TRANSFER_WRITE | (as ? ACCESS_AS_READ | ACCESS_AS_WRITE : 0u));
    if (copies) {
        if (v->traceCopies[0].len) v->vkCmdCopyBuffer(c, s->traceStaging.buffer, v->traceNodes.buffer, (uint32_t)v->traceCopies[0].len, v->traceCopies[0].items);
        if (v->traceCopies[1].len) v->vkCmdCopyBuffer(c, s->traceStaging.buffer, v->traceVertices.buffer, (uint32_t)v->traceCopies[1].len, v->traceCopies[1].items);
        memoryBarrier(v, c, STAGE_TRANSFER, readers, ACCESS_TRANSFER_WRITE, ACCESS_SHADER_READ);
    }
    if (planCount) {
        AsGeometry* geometries = artfMalloc((size_t)planCount * sizeof *geometries);
        AsBuildGeometryInfo* builds = artfMalloc((size_t)planCount * sizeof *builds);
        AsBuildRange* ranges = artfMalloc((size_t)planCount * sizeof *ranges);
        const AsBuildRange** rangeOf = artfMalloc((size_t)planCount * sizeof *rangeOf);
        const VkDeviceAddress vertices = rtAddress(v, &v->traceVertices), corners = rtAddress(v, &v->rtIndices);
        VkDeviceAddress scratchAt = rtAddress(v, &v->rtScratch);
        for (uint32_t i = 0; i < planCount; ++i) {
            const RtPlan* p = &plans[i];
            RtBottom* b = &v->rtBottoms[p->mesh];
            const ArtfTraceRange* r = &t->ranges.items[p->mesh];
            const uint32_t vertexCount = scene->meshes.items[p->mesh].vertexCount;
            geometries[i] = rtTriangles(vertices, corners, r, vertexCount);
            /* an update: as it was built (the same flags, triangles, corners and format), in place, from where the
             * vertices are now; the scratch of an update, each its own */
            const AsBuildGeometryInfo build = {ST_AS_BUILD_GEOMETRY_INFO, NULL, 1 /* bottom */, 4 /* fast to trace */ | (b->refittable ? 1u /* allow update */ : 0u),
                                               p->update ? 1 /* update */ : 0 /* build */, p->update ? b->structure : 0, b->structure, 1, &geometries[i], NULL, scratchAt};
            builds[i] = build;
            /* (the mesh's triangles, from its first: the triangle a ray meets is the first plus what the hardware says) */
            const AsBuildRange range = {r->triangleCount, r->firstTriangle * 12, 0, 0};
            ranges[i] = range;
            rangeOf[i] = &ranges[i];
            scratchAt += (p->scratch + 255) & ~(VkDeviceSize)255;
            b->deform = r->deformVersion;
            b->generation = scene->meshes.items[p->mesh].generation;
            b->firstTriangle = r->firstTriangle;
            b->triangleCount = r->triangleCount;
            b->firstVertex = r->firstVertex;
            b->vertexCount = vertexCount;
            if (p->update) ++b->refits;
            else if (!p->fresh) b->refits = 0;
            ++artfVulkanBottomLevels[p->update ? 1 : 0];
        }
        /* (all at once: each its own structure and scratch) */
        v->vkCmdBuildAccelerationStructuresKHR(c, planCount, builds, rangeOf);
        memoryBarrier(v, c, STAGE_AS_BUILD, readers, ACCESS_AS_WRITE, ACCESS_AS_READ);
        artfFree(geometries);
        artfFree(builds);
        artfFree(ranges);
        artfFree(rangeOf);
    }
    artfFree(plans);
    if (bottoms) {
        /* (the top levels hold the bottom levels' boxes and addresses) */
        if (planCount || released) ++v->rtBottomsBuilt;
        v->rtMeshVersion = t->meshVersion;
        v->rtRemovals = t->meshRemovals;
        v->rtScene = scene->id;
    }
    return true;
}

/* A slot's buffer of what rays read that changes as instances move: host-visible, written where the GPU reads it, as
 * the slot's fence was waited for. */
bool slotTraceBuffer(Vulkan* v, Slot* s, GpuBuffer* b, uint32_t binding, const void* data, VkDeviceSize size) {
    if (size > b->size || !b->buffer) {
        bufferFree(v, b);
        if (!bufferNew(v, b, size * 2, BUFFER_STORAGE, true)) return false;
        bindStorage(v, s, binding, b);
    }
    if (size) memcpy(b->mapped, data, size);
    return true;
}

/* What rays read of the instances, for this slot's frame: the trees' top and the table of instances; and with the
 * hardware, the top level, built in the frame's own commands c, before anything reads it. No wait: what the slot held
 * is done with, and the other slot's frame keeps its own. */
bool slotTopSync(Vulkan* v, Slot* s, VkCommandBuffer c, const ArtfScene* scene, bool hardware) {
    const ArtfTrace* t = &scene->trace;
    if (s->traceTopScene != scene->id || s->traceTopVersion != t->topVersion || !s->traceTop.buffer) {
        if (!slotTraceBuffer(v, s, &s->traceTop, 20, t->top.items, t->top.len * sizeof *t->top.items)) return false;
        if (!slotTraceBuffer(v, s, &s->traceInstances, 21, t->instances.items, (t->instances.len + 1) * sizeof *t->instances.items)) return false;
        s->traceTopScene = scene->id;
        s->traceTopVersion = t->topVersion;
    }
    if (!hardware || (s->rtTopLevel && s->rtTopScene == scene->id && s->rtTopVersion == t->topVersion && s->rtTopBottoms == v->rtBottomsBuilt)) return true;
    {
        const uint32_t count = (uint32_t)t->instances.len;
        if (s->rtInstances.size < (VkDeviceSize)(count + 1) * sizeof(AsInstance)) {
            bufferFree(v, &s->rtInstances);
            if (!rtBufferNew(v, &s->rtInstances, (VkDeviceSize)(count + 1) * 2 * sizeof(AsInstance), BUFFER_AS_INPUT, true)) return false;
        }
        AsInstance* out = s->rtInstances.mapped;
        uint32_t made = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t mesh = t->instanceMeshes.items[i];
            const VkAccelerationStructureKHR bottom = mesh < v->rtBottomCount ? v->rtBottoms[mesh].structure : 0;
            AsInstance o;
            memcpy(o.transform, t->instanceWorlds.items + (size_t)i * 12, 48);
            /* (no custom index: the table of instances has its mesh's first triangle, with no limit of 24 bits; fronts
             * counter-clockwise, as the trees have them; and both sides of what has two) */
            o.customAndMask = 0xFFu << 24;
            o.offsetAndFlags = (uint32_t)(4u /* opaque; counter-clockwise is the front, as the trees have it, with no flag */ | (t->instances.items[i].twoSided & 1u ? 1u /* no culling */ : 0u)) << 24;
            o.reference = bottom ? v->rtBottoms[mesh].address : 0;
            /* (one with no triangles is there, empty: ids are places among the traced instances) */
            if (!bottom) o.customAndMask &= 0xFFFFFFu; /* (a mask of 0: no ray meets it) */
            out[made++] = o;
        }
        AsGeometry geometry;
        memset(&geometry, 0, sizeof geometry);
        geometry.sType = ST_AS_GEOMETRY;
        geometry.geometryType = 2; /* instances */
        geometry.geometry.instances.sType = ST_AS_GEOMETRY_INSTANCES;
        geometry.geometry.instances.data = rtAddress(v, &s->rtInstances);
        AsBuildGeometryInfo build = {ST_AS_BUILD_GEOMETRY_INFO, NULL, 0 /* top */, 4, 0, 0, 0, 1, &geometry, NULL, 0};
        if (!s->rtTopLevel || made > s->rtTopRoom) {
            /* (room for more than there are, so that a few more do not make it again; a build of fewer fits) */
            const uint32_t room = made + made / 2 + 16;
            AsBuildSizes sizes = {ST_AS_BUILD_SIZES, NULL, 0, 0, 0};
            v->vkGetAccelerationStructureBuildSizesKHR(v->device, 1, &build, &room, &sizes);
            if (s->rtTopLevel) v->vkDestroyAccelerationStructureKHR(v->device, s->rtTopLevel, NULL);
            s->rtTopLevel = 0;
            if (s->rtTop.size < sizes.structureSize) {
                bufferFree(v, &s->rtTop);
                if (!rtBufferNew(v, &s->rtTop, sizes.structureSize, BUFFER_AS_STORAGE, false)) return false;
            }
            if (s->rtScratch.size < sizes.buildScratchSize) {
                bufferFree(v, &s->rtScratch);
                if (!rtBufferNew(v, &s->rtScratch, sizes.buildScratchSize, BUFFER_STORAGE, false)) return false;
            }
            const AsCreateInfo create = {ST_AS_CREATE_INFO, NULL, 0, s->rtTop.buffer, 0, sizes.structureSize, 0, 0};
            if (v->vkCreateAccelerationStructureKHR(v->device, &create, NULL, &s->rtTopLevel) != VK_OK) return false;
            s->rtTopRoom = room;
            const WriteDescriptorSetAs structures = {ST_WRITE_DESCRIPTOR_SET_AS, NULL, 1, &s->rtTopLevel};
            const WriteDescriptorSet w = {ST_WRITE_DESCRIPTOR_SET, &structures, s->set, 64, 0, 1, DESCRIPTOR_AS, NULL, NULL, NULL};
            v->vkUpdateDescriptorSets(v->device, 1, &w, 0, NULL);
        }
        build.dst = s->rtTopLevel;
        build.scratch = rtAddress(v, &s->rtScratch);
        const AsBuildRange range = {made, 0, 0, 0};
        const AsBuildRange* ranges = &range;
        v->vkCmdBuildAccelerationStructuresKHR(c, 1, &build, &ranges);
        memoryBarrier(v, c, STAGE_AS_BUILD, STAGE_COMPUTE | STAGE_FRAGMENT, ACCESS_AS_WRITE, ACCESS_AS_READ);
        s->rtTopScene = scene->id;
        s->rtTopVersion = t->topVersion;
        s->rtTopBottoms = v->rtBottomsBuilt;
    }
    return true;
}

/* (with pointers of 8 bytes: of 4, as armv7's, the same structures are smaller, Vulkan's as well as these) */
_Static_assert(sizeof(void*) != 8 || (sizeof(AsGeometryTriangles) == 64 && sizeof(AsGeometry) == 96 && sizeof(AsBuildGeometryInfo) == 80 && sizeof(AsInstance) == 64 && sizeof(AsCreateInfo) == 64),
               "the ray tracing structures are laid out as Vulkan's");

/* The trees rays walk, as tracing holds them (artfSceneTraceSync first): sent up when they changed, waiting for the
 * device, when meshes came or went. When only poses changed them, what they moved goes up in the frame's own commands,
 * and so do the bottom levels (slotMeshSync); what moves with the instances is the slot's (slotTopSync). */
bool traceBuffersSync(Vulkan* v, const ArtfScene* scene) {
    const ArtfTrace* t = &scene->trace;
    if ((v->traceScene != scene->id || v->traceMeshVersion != t->meshVersion) && !traceLayoutSame(v, scene)) {
        v->vkDeviceWaitIdle(v->device);
        if (!traceBuffer(v, &v->traceNodes, 17, t->wide.items, t->wide.len * sizeof *t->wide.items, false)) return false;
        if (!traceBuffer(v, &v->traceTriangles, 18, t->triangles.items, t->triangles.len * sizeof *t->triangles.items, false)) return false;
        if (!traceBuffer(v, &v->traceVertices, 19, t->vertices.items, t->vertices.len * sizeof *t->vertices.items, v->rayQuery)) return false;
        v->traceMeshVersion = t->meshVersion;
        v->traceRemovals = t->meshRemovals;
        v->traceLengths[0] = t->wide.len;
        v->traceLengths[1] = t->triangles.len;
        v->traceLengths[2] = t->vertices.len;
        v->traceLengths[3] = t->ranges.len;
        artfFree(v->traceUp);
        v->traceUpCount = (uint32_t)t->ranges.len;
        v->traceUp = artfCalloc((size_t)v->traceUpCount + 1, sizeof *v->traceUp);
        for (uint32_t m = 0; m < v->traceUpCount; ++m) {
            v->traceUp[m].deform = t->ranges.items[m].deformVersion;
            v->traceUp[m].moved = t->ranges.items[m].moved;
        }
    }
    v->traceScene = scene->id;
    return true;
}

/* Shadows and reflections from rays while drawing: what rays read, kept up to date as instances move (particles left
 * out), with the hardware's structures where there are some. (The reflections' pass: computeFollow.) */
bool syncTraced(Vulkan* v, ArtfScene* scene) {
    artfSceneTraceSync(scene);
    if (!traceBuffersSync(v, scene)) return false;
    return true;
}

/* What tracing reads and writes. (Its shader and the filter's: computeFollow.) */
bool syncTrace(Vulkan* v, ArtfScene* scene) {
    artfSceneTraceSync(scene);
    const ArtfTrace* t = &scene->trace;
    const bool guides = t->desc.denoise;
    if (!traceBuffersSync(v, scene)) return false;
    if (!v->traceSum.buffer) {
        v->vkDeviceWaitIdle(v->device);
        if (!bufferNew(v, &v->traceSum, (VkDeviceSize)v->sceneWidth * v->sceneHeight * 16, BUFFER_STORAGE | BUFFER_TRANSFER_DST, false)) return false;
        const DescriptorBufferInfo info = {v->traceSum.buffer, 0, ~(VkDeviceSize)0};
        for (int i = 0; i < SLOTS; ++i) {
            const WriteDescriptorSet w = {ST_WRITE_DESCRIPTOR_SET, NULL, v->slots[i].set, 22, 0, 1, DESCRIPTOR_STORAGE, NULL, &info, NULL};
            v->vkUpdateDescriptorSets(v->device, 1, &w, 0, NULL);
        }
        v->traceDone = 0;
    }
    if (guides && !v->denoiseBuffers[0].buffer) {
        v->vkDeviceWaitIdle(v->device);
        for (uint32_t i = 0; i < 7; ++i) {
            /* (what was gathered, and is: with the luminance's moments after) */
            const VkDeviceSize each = i == 3 || i == 4 ? 32 : 16;
            if (!bufferNew(v, &v->denoiseBuffers[i], (VkDeviceSize)v->sceneWidth * v->sceneHeight * each, BUFFER_STORAGE | BUFFER_TRANSFER_SRC | BUFFER_TRANSFER_DST, false)) return false;
            const DescriptorBufferInfo info = {v->denoiseBuffers[i].buffer, 0, ~(VkDeviceSize)0};
            for (int k = 0; k < SLOTS; ++k) {
                const WriteDescriptorSet w = {ST_WRITE_DESCRIPTOR_SET, NULL, v->slots[k].set, 27 + i, 0, 1, DESCRIPTOR_STORAGE, NULL, &info, NULL};
                v->vkUpdateDescriptorSets(v->device, 1, &w, 0, NULL);
            }
        }
        if (!bufferNew(v, &v->denoiseShift, (VkDeviceSize)v->sceneWidth * v->sceneHeight * 16, BUFFER_STORAGE, false)) return false;
        for (int k = 0; k < SLOTS; ++k) {
            const DescriptorBufferInfo info = {v->denoiseShift.buffer, 0, ~(VkDeviceSize)0};
            const WriteDescriptorSet w = {ST_WRITE_DESCRIPTOR_SET, NULL, v->slots[k].set, 48, 0, 1, DESCRIPTOR_STORAGE, NULL, &info, NULL};
            v->vkUpdateDescriptorSets(v->device, 1, &w, 0, NULL);
        }
        v->denoiseFirst = true;
    }
    return true;
}

/* The frame's light, time, how much it varies, five passes in space and back to light; then what this frame found
 * becomes the frame before. */
void denoise(Vulkan* v, VkCommandBuffer c, Slot* s) {
    const VkFlags readWrite = ACCESS_SHADER_READ | ACCESS_SHADER_WRITE;
    const uint32_t gx = ((uint32_t)v->sceneWidth + 7) / 8, gy = ((uint32_t)v->sceneHeight + 7) / 8;
    const VkPipeline passes[DENOISE_PIPELINES] = {v->denoiseLight, v->denoiseTime, v->denoiseVariance, v->denoiseSpace[0], v->denoiseSpace[1], v->denoiseSpace[2], v->denoiseSpace[3], v->denoiseSpace[4], v->denoiseEnd};
    v->vkCmdBindDescriptorSets(c, 1, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
    for (int i = 0; i < DENOISE_PIPELINES; ++i) {
        memoryBarrier(v, c, STAGE_COMPUTE, STAGE_COMPUTE, ACCESS_SHADER_WRITE, readWrite);
        v->vkCmdBindPipeline(c, 1, passes[i]);
        v->vkCmdDispatch(c, gx, gy, 1);
    }
    memoryBarrier(v, c, STAGE_COMPUTE, STAGE_TRANSFER | STAGE_FRAGMENT, ACCESS_SHADER_WRITE, ACCESS_TRANSFER_READ | ACCESS_SHADER_READ);
    /* guide to the frame before's, what is gathered to what was */
    const BufferCopy all = {0, 0, (VkDeviceSize)v->sceneWidth * v->sceneHeight * 16};
    v->vkCmdCopyBuffer(c, v->denoiseBuffers[0].buffer, v->denoiseBuffers[2].buffer, 1, &all);
    const BufferCopy gathered = {0, 0, (VkDeviceSize)v->sceneWidth * v->sceneHeight * 32};
    v->vkCmdCopyBuffer(c, v->denoiseBuffers[4].buffer, v->denoiseBuffers[3].buffer, 1, &gathered);
    memoryBarrier(v, c, STAGE_TRANSFER, STAGE_COMPUTE, ACCESS_TRANSFER_WRITE, readWrite);
}

