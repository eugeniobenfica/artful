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
#include "render/rendererInternal.h"
#include "scene/sceneInternal.h"
#include "core/threads.h"
#include "vulkan/shaders.h"
#include "vulkan/vulkanApi.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

enum {
    FORMAT_RGBA8 = 37, FORMAT_BGRA8 = 44, FORMAT_RGBA16_FLOAT = 97, FORMAT_R32_UINT = 98, FORMAT_R32G32_UINT = 101, FORMAT_D32 = 126, FORMAT_RGBA16_UNORM = 91, FORMAT_RG16_SNORM = 78, FORMAT_RG16_FLOAT = 83,
    LAYOUT_UNDEFINED = 0, LAYOUT_COLOR = 2, LAYOUT_DEPTH = 3, LAYOUT_DEPTH_READ = 4, LAYOUT_SHADER_READ = 5, LAYOUT_TRANSFER_SRC = 6, LAYOUT_TRANSFER_DST = 7, LAYOUT_PRESENT = 1000001002,
    IMAGE_TRANSFER_SRC = 1, IMAGE_TRANSFER_DST = 2, IMAGE_SAMPLED = 4, IMAGE_COLOR = 0x10, IMAGE_DEPTH = 0x20,
    BUFFER_TRANSFER_SRC = 1, BUFFER_TRANSFER_DST = 2, BUFFER_UNIFORM = 0x10, BUFFER_STORAGE = 0x20, BUFFER_INDEX = 0x40, BUFFER_VERTEX = 0x80, BUFFER_INDIRECT = 0x100,
    MEMORY_DEVICE_LOCAL = 1, MEMORY_HOST_VISIBLE = 2, MEMORY_HOST_COHERENT = 4, MEMORY_HOST_CACHED = 8,
    STAGE_TOP = 1, STAGE_DRAW_INDIRECT = 2, STAGE_VERTEX_INPUT = 4, STAGE_VERTEX_SHADER = 8, STAGE_FRAGMENT = 0x80, STAGE_EARLY_TESTS = 0x100, STAGE_LATE_TESTS = 0x200, STAGE_COLOR = 0x400, STAGE_COMPUTE = 0x800, STAGE_TRANSFER = 0x1000, STAGE_BOTTOM = 0x2000, STAGE_HOST = 0x4000,
    ACCESS_INDIRECT = 1, ACCESS_VERTEX_READ = 4, ACCESS_SHADER_READ = 0x20, ACCESS_SHADER_WRITE = 0x40, ACCESS_COLOR_READ = 0x80, ACCESS_COLOR_WRITE = 0x100, ACCESS_DEPTH_READ = 0x200, ACCESS_DEPTH_WRITE = 0x400, ACCESS_TRANSFER_READ = 0x800, ACCESS_TRANSFER_WRITE = 0x1000, ACCESS_HOST_READ = 0x2000,
    DESCRIPTOR_COMBINED = 1, DESCRIPTOR_UNIFORM = 6, DESCRIPTOR_STORAGE = 7,
    SHADER_VERTEX = 1, SHADER_FRAGMENT = 0x10, SHADER_COMPUTE = 0x20,
    ASPECT_COLOR = 1, ASPECT_DEPTH = 2
};

#define SLOTS 2            /* frames in flight */
#define QUERIES (ARTF_PASS_TIMES + 1) /* timestamps of a slot: where its frame starts, and where each pass ends */
#define MAX_TEXTURES 1024  /* in the one descriptor array, where the device allows; what is beyond draws white */
#define MAX_SWAPCHAIN_IMAGES 8

typedef struct GpuBuffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size;
    void* mapped;
} GpuBuffer;

typedef struct GpuImage {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
} GpuImage;

/* What belongs to one frame in flight. */
typedef struct Slot {
    VkCommandBuffer commands;
    VkFence fence;
    VkSemaphore acquired;
    VkDescriptorSet set;
    bool timed; /* its timestamps hold a frame */
    const char* passNames[ARTF_PASS_TIMES]; /* the passes its timestamps end, after the first */
    uint32_t passCount;
    GpuBuffer frame, instances, visible, indirect, template_, materials, overlayStaging, background, stream, layerList, trace;
    GpuBuffer extraLights, tileLights; /* lights past the frame's own, and each square's list of them (60, 61) */
    GpuBuffer decals;                  /* the decals that may show (63) */
    GpuBuffer probes;                  /* light that bounced (62) */
    /* What rays read of the instances, this slot's, as they move: the trees' top and the table of instances (20, 21);
     * with the hardware, its instances, the top level built from them in the frame's own commands (64), and that
     * build's scratch. Each says which of tracing's versions it holds. */
    GpuBuffer traceTop, traceInstances, rtInstances, rtTop, rtScratch;
    GpuBuffer traceStaging; /* what bones moved of the trees, on its way up in the frame's commands (slotMeshSync) */
    VkAccelerationStructureKHR rtTopLevel;
    uint32_t rtTopRoom;
    uint64_t traceTopVersion, traceTopScene, rtTopVersion, rtTopScene, rtTopBottoms;
    GpuBuffer swarmRows;               /* this frame's rows for emitters of many (67) */
    GpuBuffer shadow, shadowList, shadowCommands, shadowViewCommands, shadowCasters, posed, skinTable, motion, meshletCommands, meshletState, layerCommands;
    VkBuffer meshletBoundTable; /* what its set says bindings 40 and 41 are */
    bool meshletBound;
    uint32_t shadowCasterCount, shadowViews;
    uint64_t motionVersion;
    uint64_t motionStep;  /* (what of ArtfMotion this slot holds, after its instances) */
    size_t motionCount;
    VkBuffer skinBoundData, skinBoundVertices; /* what its set says binding 34 and 36 are */
    uint64_t shadowInstances, shadowStructure, shadowMaterials, shadowScene;
    uint32_t shadowMeshes;
    uint64_t instanceVersion, structureVersion, materialVersion, lodVersion, streamStamp, streamScene;
    bool fresh; /* nothing written yet */
} Slot;

typedef struct SkinPlace { const ArtfSkinData* skin; uint32_t row; } SkinPlace;

typedef struct MeshRange { uint32_t firstIndex; int32_t vertexOffset; uint32_t indexCount, colorBase; uint64_t deformVersion; uint32_t vertexLength, colorLength, generation; } MeshRange;

/* Stretches of a buffer that were given back, by where they start. */
typedef struct Span { uint32_t at, length; } Span;
typedef ARTF_VEC(Span) SpanList;

/* Room for length, in what was given back or after end; UINT32_MAX: none. */
static uint32_t spanTake(SpanList* list, uint32_t length, uint32_t* end, uint32_t capacity) {
    for (size_t i = 0; i < list->len; ++i) {
        Span* s = &list->items[i];
        if (s->length < length) continue;
        const uint32_t at = s->at;
        s->at += length;
        s->length -= length;
        if (!s->length) {
            memmove(s, s + 1, (list->len - i - 1) * sizeof *s);
            --list->len;
        }
        return at;
    }
    if (length > capacity || *end > capacity - length) return UINT32_MAX;
    const uint32_t at = *end;
    *end += length;
    return at;
}

static void spanGive(SpanList* list, uint32_t at, uint32_t length) {
    if (!length) return;
    size_t i = 0;
    while (i < list->len && list->items[i].at < at) ++i;
    /* (joined to what touches it, so that large meshes find room again) */
    if (i && list->items[i - 1].at + list->items[i - 1].length == at) {
        list->items[i - 1].length += length;
        if (i < list->len && list->items[i].at == at + length) {
            list->items[i - 1].length += list->items[i].length;
            memmove(&list->items[i], &list->items[i + 1], (list->len - i - 1) * sizeof *list->items);
            --list->len;
        }
        return;
    }
    if (i < list->len && list->items[i].at == at + length) {
        list->items[i].at = at;
        list->items[i].length += length;
        return;
    }
    artfVecReserve(list, list->len + 1);
    memmove(&list->items[i + 1], &list->items[i], (list->len - i) * sizeof *list->items);
    list->items[i].at = at;
    list->items[i].length = length;
    ++list->len;
}

typedef struct Vulkan {
    ArtfRenderer base;
    void* library;
    void* (VKAPI* getInstanceProcAddr)(VkInstance, const char*);
    VkResult (VKAPI* vkCreateInstance)(const InstanceCreateInfo*, const void*, VkInstance*);
#define X(name, result, args) result (VKAPI* name) args;
    ARTF_VK_INSTANCE_FUNCTIONS(X)
    ARTF_VK_DEVICE_FUNCTIONS(X)
#undef X
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    PhysicalDeviceMemoryProperties memory;
    char description[300];
    bool noVsync;
    bool compressTextures, takesBc, takesEtc, takesAstc;
    char* pipelineCachePath;
    VkPipelineCache pipelineCache;
    VkQueryPool timestamps;
    float timestampPeriod; /* nanoseconds a tick; 0: this device has no timestamps */
    double gpuMs;
    ArtfPassTime passTimes[ARTF_PASS_TIMES]; /* of the last frame the GPU finished */
    uint32_t passTimeCount;

    ArtfSurface native;
    VkSurfaceKHR surface;
    int colorFormat, colorSpace; /* what the frame is drawn in: what the surface offers */
    VkSwapchainKHR swapchain;
    uint32_t imageCount;
    VkImage images[MAX_SWAPCHAIN_IMAGES];
    VkImageView views[MAX_SWAPCHAIN_IMAGES];
    VkFramebuffer framebuffers[MAX_SWAPCHAIN_IMAGES];
    VkSemaphore rendered[MAX_SWAPCHAIN_IMAGES];
    GpuImage offscreen; /* in place of a swapchain */
    GpuBuffer readback; /* where artfRendererReadPixels copies it, kept from one frame to the next */
    GpuImage depth;
    GpuImage samples4;  /* with antialiasing: what is drawn, before it is resolved into the frame */
    int samples;        /* of a pixel: 1 or 4 */
    int width, height;
    bool targetsStale; /* a new size: made again before the next frame */
    /* What the surface said when the swapchain was made: a frame that is only suboptimal makes it again
     * when this changes, not each time (Android says suboptimal for as long as the compositor turns it). */
    VkFlags surfaceTransform;
    uint32_t surfaceWidth, surfaceHeight;
    bool lost;         /* nothing more can be drawn: it can only be freed */
    uint64_t shadowKeys[ARTF_SHADOW_VIEWS]; /* what each map holds */
    uint64_t shadowSeen;                    /* the scene's changes these know of */
    /* layers by each pixel's depth */
    bool fragmentStores, sampleShading;
    GpuBuffer layerCounts, layerFragments;
    VkPipeline layerListPipeline, layerResolvePipeline;
    VkRenderPass midPass, resolvePass;
    VkFramebuffer resolveFramebuffer;
    /* meshlets on the GPU: their table, and what each mesh has of it */
    bool drawCounted;
    GpuBuffer meshletTable, meshletMeshes;
    uint32_t mostMeshlets;
    VkPipeline meshletPipelines[3]; /* by ArtfCullPhase */
    /* whether each meshlet of each instance showed, and where each instance's are; 0: none has */
    GpuBuffer meshletFlags, meshletFlagStarts;
    uint64_t meshletFlagVersion;
    uint32_t shadowMapsDrawn;
    VkPipeline shadowCullPipeline;
    /* bones, on the GPU: what meshes were, where each skin is in it, and what this frame moves */
    GpuBuffer skinData;
    ARTF_VEC(SkinPlace) skinPlaces;
    uint64_t skinRemovals, skinScene;
    VkPipeline skinPipeline;
    ARTF_VEC(uint32_t) skinJobs, scratchSkinCounts;
    /* emitters of many (particles.c): their particles, each emitter's after the one before (binding 66); which they
     * are, how many steps each has seen, and where each starts */
    GpuBuffer swarmState;
    VkPipeline swarmPipeline;
    ARTF_VEC(ArtfSwarm*) swarms, swarmsBefore;
    ARTF_VEC(uint64_t) swarmSeen;
    ARTF_VEC(uint32_t) swarmAt;
    uint32_t swarmRoom;   /* particles the instances' and copies' buffers have room for, after the rest */
    uint32_t swarmPlaces; /* this frame's */
    uint64_t swarmScene;

    VkRenderPass renderPass, earlyPass, latePass;
    VkDescriptorSetLayout setLayout;
    VkPipelineLayout pipelineLayout;
    VkPipeline cullPipelines[3], hizFirstPipeline, hizNextPipeline, scenePipeline, cutoutPipeline, overlayPipeline, layerDepthPipeline, layerPipeline;
    VkPipeline particlePipeline;  /* particles one after another, in one draw: depth tested and written as each goes */
    VkPipeline tracePipeline, traceShowPipeline, shadowPipeline, shadowHolesPipeline;
    VkShaderModule sceneVertexModule; /* kept: a scene's graphs make new pipelines */
    uint64_t graphScene;
    uint32_t graphCount;
    bool probes, walls, froxels, coats, softShadows, shapes; /* the scene's pixel shaders read probes, their maps, the fog's grid, coats, lights' shapes: made again when that changes */
    /* views (ArtfView): the scene's pixel shaders have them, and each triangle's meshlet; the overdraw view's pipelines,
     * one for what is culled and one for both sides; and each triangle's meshlet, by mesh (binding 65) */
    bool viewShaders, meshletShaders, primitiveIds;
    VkPipeline overdrawPipelines[2];
    GpuBuffer meshletIds;
    uint64_t meshletIdsKey;
    bool tracedShadows, tracedHardware; /* and shadows from rays (ArtfShadowDesc.traced), through the trees or the hardware */
    VkPipeline reflectionTraced;        /* reflections from rays (ArtfPostDesc.screen.tracedReflections): made when first asked */
    bool reflectionTracedHardware;
    /* shadow maps: all in one atlas of depth */
    VkRenderPass shadowPass;
    VkFramebuffer shadowFramebuffer;
    GpuImage shadowAtlas;
    int shadowWidth, shadowHeight;
    VkPipeline backgroundPipeline, bloomFirstPipeline, bloomNextPipeline, bloomUpPipeline, backdropPipeline;
    VkDescriptorPool descriptorPool;
    VkSampler sampler, depthSampler;
    bool multiDraw;       /* several draws in one indirect call */
    /* The ray tracing hardware (ArtfTraceDesc.hardware): a bottom level for each mesh traced, in one buffer; the top
     * level over the traced instances, in their order (binding 64). */
    bool rayQuery;        /* the device has acceleration structures and ray queries, and they are on */
    bool traceHardware;   /* the trace pipeline goes through them */
    bool traceMore;       /* the trace pipeline reads materials past glTF's core and lights' shapes */
    /* Each mesh's bottom level is its own, built or refitted in the frame's commands (slotMeshSync), in a room of one of
     * the blocks; the corners are each mesh's own (from 0), and the positions are the trees' vertices (binding 19). */
    GpuBuffer rtIndices, rtScratch;
    struct RtBottom* rtBottoms;
    uint32_t rtBottomCount;
    ARTF_VEC(struct RtBlock) rtBlocks;
    uint64_t rtBottomsBuilt; /* how many times bottom levels were built: a slot's top level built before is stale */
    uint64_t rtMeshVersion, rtScene, rtRemovals;
    uint64_t rtIndexScene, rtIndexRemovals; /* what the corners were written for, and how many triangles */
    size_t rtIndexTriangles;
    uint32_t maxTextures;
    /* the depth pyramid; flags: what showed last frame */
    GpuBuffer hiz, flags;
    uint32_t hizLevels;
    uint32_t hizTable[ARTF_HIZ_LEVELS][4];
    VkCommandPool commandPool;
    Slot slots[SLOTS];
    uint64_t frameNumber;
    /* Pixel shaders of another variant, being made on a thread of their own while frames go on with these (see
     * shadersStep); and pipelines no frame takes any more, destroyed once the frames that took them are done. */
    ArtfTask* shaderTask;
    struct ShaderJob* shaderJob;
    ARTF_VEC(struct Retired) retired;

    /* the scene, as the GPU has it */
    uint64_t sceneId;
    GpuBuffer vertices, indices, meshTable, colors;
    SpanList vertexSpans, indexSpans, colorSpans;
    uint64_t meshRemovals, textureVersion;
    ARTF_VEC(uint64_t) textureMarks; /* of each texture here: its generation and resident mipmap */
    uint32_t vertexCount, indexCount, colorCount;
    ARTF_VEC(MeshRange) meshes;
    ARTF_VEC(GpuImage) textures;
    uint32_t instanceCapacity, materialCapacity, meshCapacity;
    ARTF_VEC(uint32_t) copyStarts; /* scratch: where each mesh's copies start */
    ARTF_VEC(ArtfLayer) layers;    /* scratch: what shows what is behind it, in view */
    ARTF_VEC(BufferCopy) posedCopies; /* scratch: meshes a pose moved */
    ARTF_VEC(BufferCopy) pastCopies;  /* scratch: what the vertices of meshes that bend were */
    uint64_t meshTableVersion;

    /* tracing: what rays read, and what each pixel gathered */
    GpuBuffer traceNodes, traceTriangles, traceVertices, traceSum;
    /* the filter: what surfaces are, what was gathered, what its passes hand on */
    GpuBuffer denoiseBuffers[7], denoiseShift;
    VkPipeline denoiseLight, denoiseTime, denoiseSpace[3], denoiseEnd;
    bool traceGuides, denoiseFirst;
    uint32_t traceFrames;
    float tracePrevious[16];
    uint64_t traceScene, traceMeshVersion, traceKey;
    uint32_t traceDone;
    /* what the trees on the GPU were last sent up as: their lengths, and each mesh's pose (what bones move goes up in
     * the frame's commands, slotMeshSync) */
    size_t traceLengths[4];
    uint64_t traceRemovals;
    struct TraceUp* traceUp;
    uint32_t traceUpCount;
    ARTF_VEC(BufferCopy) traceCopies[2]; /* scratch: of the trees' nodes and vertices */

    /* a background pass: its light, its bloom and its data; its shader, the program's */
    GpuBuffer hdr, bloom, backgroundData;
    GpuBuffer environment;          /* a scene's maps of all around (binding 49): four bytes until one has some */
    uint64_t environmentVersion, environmentScene;
    VkPipeline environmentPipeline; /* behind the scene */
    /* A frame in floats (ArtfRendererOptions.hdr): the scene draws light and surfaces, the passes of postShaders.inc
     * follow, and presentPass puts it in the frame, with the overlay. */
    bool floats, independentBlend;
    int sceneFormat;                /* what the scene's passes draw into: floats, or the frame's own */
    GpuImage floatColor, floatSurface;
    VkRenderPass presentPass;
    VkFramebuffer presentFramebuffers[MAX_SWAPCHAIN_IMAGES];
    VkPipeline presentPipeline;
    VkPipeline postPipelines[16];   /* gather, occlusion, its blur, reflections, froxels, temporal, focus, bloom down, up, reflections' use, meter, motion blur,
                                     * velocity, tiles' largest, upscaling, sharpening */
    ArtfMotion motion;              /* how each instance moved: after the instances in each slot's buffer */
    bool exposureValid;             /* auto exposure has a state to go on from */
    VkPipeline fogPipeline;         /* fog that light goes through, over what is behind the scene */
    GpuBuffer postLight, postDepth, postSurface, postScratch, postHistory, postOcclusion, postFog, postBloom;
    /* Light, the second light and the history are three buffers alike, and which plays which part changes in a frame
     * in place of copying one into another: the passes read and write them through set 1 (0 light, 1 second, 2 history),
     * one set for each way of giving the three parts to the three buffers (roleSets[light * 9 + second * 3 + history],
     * of postLight, postScratch and postHistory), and role says which is which now. */
    VkDescriptorSetLayout roleLayout;
    VkDescriptorPool rolePool;
    VkDescriptorSet roleSets[27];
    uint8_t role[3];
    uint32_t postBloomSizes[6][3];  /* of each level: where it starts, its width, its height */
    /* The look-up table (ArtfPostDesc.look.lut), in bloom's buffer past auto exposure's state: room for so many vec4s, which
     * scene and version it holds, and a copy of it to make from the staging buffer at the next frame. */
    VkDeviceSize lutRoom;
    uint64_t lutScene, lutVersion;
    bool lutPending;
    GpuBuffer lutStaging;
    float previous[16];             /* the frame before's viewProj */
    bool hasPrevious, historyValid;
    uint32_t postFrames;
    uint32_t grainFrames;           /* for film grain */
    /* The scene's size: the frame's, unless it is drawn smaller and upscaled (ArtfRendererOptions.renderScale). What
     * the scene draws and what reads it before upscaling are this size; the rest, the overlay and the frame, the
     * frame's. */
    int sceneWidth, sceneHeight;
    float renderScale;
    bool surfaceWide;               /* the surface target has its second word: motion is followed */
    GpuBuffer pastVertices;         /* what the vertex buffer held a frame ago, where a pose moves vertices (binding 68) */
    GpuBuffer postBlur;             /* motion blur's velocities (binding 69) */
    ArtfPool* pool;                 /* workers for what the CPU does for the GPU: the probes */
    ArtfLightData* lightScratch;    /* lights past the frame's own, and their lists, before they go up */
    uint32_t* listScratch;
    VkDeviceSize listScratchSize;
    uint64_t backgroundDataStamp, backgroundDataScene, backgroundModuleStamp, backgroundModuleScene;

    /* the overlay */
    GpuImage overlay;
    const uint32_t* overlayPixels;
    int overlayWidth, overlayHeight, overlayStride;
    bool overlayFull;     /* all of it must go up */
    bool overlayPending;  /* something must go up */
    bool overlayUsable;   /* the image holds a picture */
    ARTF_VEC(ArtfRect) overlayDamage;

    double cpuMs;
} Vulkan;

/* ---- memory ---- */

static int memoryType(const Vulkan* v, uint32_t bits, VkFlags wanted) {
    for (uint32_t i = 0; i < v->memory.typeCount; ++i)
        if ((bits >> i & 1) && (v->memory.types[i].propertyFlags & wanted) == wanted) return (int)i;
    return -1;
}

typedef enum BufferUse { BUFFER_GPU, BUFFER_WRITTEN_BY_CPU, BUFFER_READ_BY_CPU } BufferUse;

static bool bufferNewFor(Vulkan* v, GpuBuffer* b, VkDeviceSize size, VkFlags usage, BufferUse use) {
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
    if (v->vkAllocateMemory(v->device, &alloc, NULL, &b->memory) != VK_OK) return false;
    v->vkBindBufferMemory(v->device, b->buffer, b->memory, 0);
    b->size = info.size;
    if (use != BUFFER_GPU && v->vkMapMemory(v->device, b->memory, 0, ~(VkDeviceSize)0, 0, &b->mapped) != VK_OK) return false;
    return true;
}

static bool bufferNew(Vulkan* v, GpuBuffer* b, VkDeviceSize size, VkFlags usage, bool hostVisible) {
    return bufferNewFor(v, b, size, usage, hostVisible ? BUFFER_WRITTEN_BY_CPU : BUFFER_GPU);
}

static void bufferFree(Vulkan* v, GpuBuffer* b) {
    if (b->buffer) v->vkDestroyBuffer(v->device, b->buffer, NULL);
    if (b->memory) v->vkFreeMemory(v->device, b->memory, NULL);
    memset(b, 0, sizeof *b);
}

static bool imageNew(Vulkan* v, GpuImage* g, int format, int width, int height, uint32_t mips, int samples, VkFlags usage, VkFlags aspect) {
    memset(g, 0, sizeof *g);
    ImageCreateInfo info = {ST_IMAGE_CREATE_INFO, NULL, 0, 1 /* 2D */, format, {(uint32_t)width, (uint32_t)height, 1}, mips, 1, samples, 0 /* optimal */, usage, 0, 0, NULL, LAYOUT_UNDEFINED};
    if (v->vkCreateImage(v->device, &info, NULL, &g->image) != VK_OK) return false;
    MemoryRequirements req;
    v->vkGetImageMemoryRequirements(v->device, g->image, &req);
    int type = memoryType(v, req.typeBits, MEMORY_DEVICE_LOCAL);
    if (type < 0) return false;
    MemoryAllocateInfo alloc = {ST_MEMORY_ALLOCATE_INFO, NULL, req.size, (uint32_t)type};
    if (v->vkAllocateMemory(v->device, &alloc, NULL, &g->memory) != VK_OK) return false;
    v->vkBindImageMemory(v->device, g->image, g->memory, 0);
    ImageViewCreateInfo view = {ST_IMAGE_VIEW_CREATE_INFO, NULL, 0, g->image, 1 /* 2D */, format, {0, 0, 0, 0}, {aspect, 0, mips, 0, 1}};
    return v->vkCreateImageView(v->device, &view, NULL, &g->view) == VK_OK;
}

static void imageFree(Vulkan* v, GpuImage* g) {
    if (g->view) v->vkDestroyImageView(v->device, g->view, NULL);
    if (g->image) v->vkDestroyImage(v->device, g->image, NULL);
    if (g->memory) v->vkFreeMemory(v->device, g->memory, NULL);
    memset(g, 0, sizeof *g);
}

static void imageBarrier(Vulkan* v, VkCommandBuffer c, VkImage image, uint32_t mips, int from, int to, VkFlags srcStage, VkFlags dstStage, VkFlags srcAccess, VkFlags dstAccess) {
    ImageMemoryBarrier b = {ST_IMAGE_MEMORY_BARRIER, NULL, srcAccess, dstAccess, from, to, ~0u, ~0u, image, {ASPECT_COLOR, 0, mips, 0, 1}};
    v->vkCmdPipelineBarrier(c, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &b);
}

static void memoryBarrier(Vulkan* v, VkCommandBuffer c, VkFlags srcStage, VkFlags dstStage, VkFlags srcAccess, VkFlags dstAccess) {
    GlobalBarrier b = {ST_MEMORY_BARRIER, NULL, srcAccess, dstAccess};
    v->vkCmdPipelineBarrier(c, srcStage, dstStage, 0, 1, &b, 0, NULL, 0, NULL);
}

/* Commands to run now, for what happens rarely: loading a mesh, say. */
static VkCommandBuffer oneShotBegin(Vulkan* v) {
    CommandBufferAllocateInfo alloc = {ST_COMMAND_BUFFER_ALLOCATE, NULL, v->commandPool, 0, 1};
    VkCommandBuffer c = NULL;
    v->vkAllocateCommandBuffers(v->device, &alloc, &c);
    CommandBufferBeginInfo begin = {ST_COMMAND_BUFFER_BEGIN, NULL, 1 /* one time */, NULL};
    v->vkBeginCommandBuffer(c, &begin);
    return c;
}

static bool oneShotEnd(Vulkan* v, VkCommandBuffer c) {
    v->vkEndCommandBuffer(c);
    SubmitInfo submit = {ST_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &c, 0, NULL};
    VkResult r = v->vkQueueSubmit(v->queue, 1, &submit, 0);
    if (r == VK_OK) r = v->vkQueueWaitIdle(v->queue);
    v->vkFreeCommandBuffers(v->device, v->commandPool, 1, &c);
    return r == VK_OK;
}

/* ---- the frame's targets ---- */

static void targetsFree(Vulkan* v) {
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
    GpuBuffer* const post[9] = {&v->postLight, &v->postDepth, &v->postSurface, &v->postScratch, &v->postHistory, &v->postOcclusion, &v->postFog, &v->postBloom, &v->postBlur};
    for (int i = 0; i < 9; ++i) bufferFree(v, post[i]);
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

static void writeOverlayDescriptor(Vulkan* v) {
    DescriptorImageInfo image = {v->sampler, v->overlay.view, LAYOUT_SHADER_READ};
    for (int i = 0; i < SLOTS; ++i) {
        WriteDescriptorSet w = {ST_WRITE_DESCRIPTOR_SET, NULL, v->slots[i].set, 7, 0, 1, DESCRIPTOR_COMBINED, &image, NULL, NULL};
        v->vkUpdateDescriptorSets(v->device, 1, &w, 0, NULL);
    }
}

/* The pyramid's levels, from half the frame down to a texel, and what the depth buffer's size is. */
static bool hizMake(Vulkan* v) {
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
bool artfVulkanRefuseSwapchain = false;

static void lightDescriptors(Vulkan* v, Slot* s);
static void bindStorage(Vulkan* v, Slot* s, uint32_t binding, const GpuBuffer* b);

/* With floats: what the scene draws into, and what comes after reads and writes, bound once for every slot. */
static bool postTargetsMake(Vulkan* v) {
    if (!imageNew(v, &v->floatColor, v->sceneFormat, v->sceneWidth, v->sceneHeight, 1, 1, IMAGE_COLOR | IMAGE_SAMPLED, ASPECT_COLOR)) return false;
    /* (the surface word, and where motion is followed, the instance that shows + 1 or where it was) */
    if (!imageNew(v, &v->floatSurface, v->surfaceWide ? FORMAT_R32G32_UINT : FORMAT_R32_UINT, v->sceneWidth, v->sceneHeight, 1, v->samples, IMAGE_COLOR | IMAGE_SAMPLED, ASPECT_COLOR)) return false;
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
    /* (bloom's levels, and past them auto exposure's state) */
    /* (the surface's words, then the instance that shows at each pixel) */
    /* (light, the second and the history end at the frame's size) */
    const VkDeviceSize sizes[8] = {framePixels * 16, pixels * 4, pixels * 8, framePixels * 16, framePixels * 16, pixels * 4, fog * 16, (bloom + 1 + v->lutRoom) * 16};
    for (int i = 0; i < 8; ++i)
        if (!bufferNew(v, buffers[i], sizes[i], BUFFER_STORAGE | BUFFER_TRANSFER_DST | BUFFER_TRANSFER_SRC, false)) return false;
    /* (motion blur's: each pixel's velocity, then of each tile of 16 the largest, and of its neighbours) */
    const VkDeviceSize blurTiles = (VkDeviceSize)((v->width + 15) / 16) * ((v->height + 15) / 16);
    if (!bufferNew(v, &v->postBlur, (framePixels + blurTiles * 2) * 8, BUFFER_STORAGE, false)) return false;
    for (int k = 0; k < SLOTS; ++k) {
        WriteDescriptorSet writes[10];
        DescriptorBufferInfo infos[8];
        for (int i = 0; i < 8; ++i) {
            const DescriptorBufferInfo info = {buffers[i]->buffer, 0, ~(VkDeviceSize)0};
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
        bindStorage(v, &v->slots[k], 69, &v->postBlur);
    }
    const GpuBuffer* const three[3] = {&v->postLight, &v->postScratch, &v->postHistory};
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

/* The scene's look-up table, into the staging buffer, and bloom's buffer made larger when it has no room for it: the
 * copy is made as the frame's commands begin (postRecord). Rare: it waits for the GPU. */
static bool syncLut(Vulkan* v, const ArtfScene* scene) {
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

static bool targetsMake(Vulkan* v) {
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
        SwapchainCreateInfo info = {ST_SWAPCHAIN_CREATE_INFO, NULL, 0, v->surface, images, v->colorFormat, v->colorSpace, {(uint32_t)v->width, (uint32_t)v->height}, 1, IMAGE_COLOR, 0, 0, NULL, upright ? 1u : caps.currentTransform, alpha, mode, 1, old};
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
        if (!imageNew(v, &v->offscreen, FORMAT_BGRA8, v->width, v->height, 1, 1, IMAGE_COLOR | IMAGE_TRANSFER_SRC, ASPECT_COLOR)) return false;
        v->imageCount = 1;
        v->images[0] = v->offscreen.image;
        ImageViewCreateInfo view = {ST_IMAGE_VIEW_CREATE_INFO, NULL, 0, v->offscreen.image, 1, FORMAT_BGRA8, {0, 0, 0, 0}, {ASPECT_COLOR, 0, 1, 0, 1}};
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
    if (many) {
        FramebufferCreateInfo info = {ST_FRAMEBUFFER, NULL, 0, v->resolvePass, 1, &v->samples4.view, (uint32_t)v->sceneWidth, (uint32_t)v->sceneHeight, 1};
        if (v->vkCreateFramebuffer(v->device, &info, NULL, &v->resolveFramebuffer) != VK_OK) return false;
    }
    if (v->floats && !postTargetsMake(v)) return false;
    for (uint32_t i = 0; i < v->imageCount; ++i) {
        const VkImageView one[2] = {v->views[i], v->depth.view}, resolved[3] = {v->samples4.view, v->depth.view, v->views[i]};
        const VkImageView floats[3] = {v->floatColor.view, v->depth.view, v->floatSurface.view};
        const VkImageView floatsMany[4] = {v->samples4.view, v->depth.view, v->floatColor.view, v->floatSurface.view};
        FramebufferCreateInfo info = {ST_FRAMEBUFFER, NULL, 0, v->renderPass, v->floats ? (many ? 4 : 3) : many ? 3 : 2, v->floats ? (many ? floatsMany : floats) : many ? resolved : one, (uint32_t)v->sceneWidth, (uint32_t)v->sceneHeight, 1};
        if (v->vkCreateFramebuffer(v->device, &info, NULL, &v->framebuffers[i]) != VK_OK) return false;
        if (v->floats) {
            FramebufferCreateInfo present = {ST_FRAMEBUFFER, NULL, 0, v->presentPass, 1, &v->views[i], (uint32_t)v->width, (uint32_t)v->height, 1};
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
static bool surfaceChanged(Vulkan* v) {
    SurfaceCapabilities caps;
    if (v->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v->gpu, v->surface, &caps) != VK_OK) return true;
    return caps.currentTransform != v->surfaceTransform || caps.current.width != v->surfaceWidth || caps.current.height != v->surfaceHeight;
}

static bool targetsRemake(Vulkan* v) {
    v->vkDeviceWaitIdle(v->device);
    targetsFree(v);
    if (v->overlayPixels) { v->overlayFull = true; v->overlayPending = true; }
    return targetsMake(v);
}

/* ---- pipelines ---- */

static VkShaderModule shaderModule(Vulkan* v, uint32_t* code, size_t size) {
    ShaderModuleCreateInfo info = {ST_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkShaderModule module = 0;
    v->vkCreateShaderModule(v->device, &info, NULL, &module);
    artfFree(code);
    return module;
}

/* colorLoad, depthLoad: 0 clears, 1 keeps what is there; depthStore: what is drawn stays for a shader. */
/* A pipeline's blending into the scene's passes: with a frame in floats, the surface is beside the color, and only
 * what is solid writes it. */
static PipelineColorBlend blendOf(const Vulkan* v, ColorBlendAttachment* pair, ColorBlendAttachment color, bool surface) {
    pair[0] = color;
    const ColorBlendAttachment kept = {0, 0, 0, 0, 0, 0, 0, surface ? 3u : 0u};
    pair[1] = kept;
    const PipelineColorBlend b = {ST_PIPELINE_COLOR_BLEND, NULL, 0, 0, 0, v->floats ? 2u : 1u, pair, {0, 0, 0, 0}};
    return b;
}

static bool renderPassMake(Vulkan* v, VkRenderPass* out, bool load, bool store, int colorFinal, bool readDepth) {
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
        {0, v->surfaceWide ? FORMAT_R32G32_UINT : FORMAT_R32_UINT, samples, load ? 0 : 1, 0, 2, 1, colorFrom, colorFinal}};
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
    const SubpassDependency dependencies[2] = {
        {~0u, 0, draw | STAGE_COMPUTE, draw, ACCESS_DEPTH_WRITE | ACCESS_COLOR_WRITE, ACCESS_COLOR_READ | ACCESS_COLOR_WRITE | ACCESS_DEPTH_READ | ACCESS_DEPTH_WRITE, 0},
        {0, ~0u, draw, STAGE_COMPUTE | STAGE_FRAGMENT, ACCESS_DEPTH_WRITE | ACCESS_COLOR_WRITE, ACCESS_SHADER_READ, 0}};
    const RenderPassCreateInfo pass = {ST_RENDER_PASS, NULL, 0, count, used, 1, &subpass, 2, dependencies};
    return v->vkCreateRenderPass(v->device, &pass, NULL, out) == VK_OK;
}

/* What the driver compiled last time, if a file was named. */
static void pipelineCacheOpen(Vulkan* v) {
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

static void pipelineCacheSave(Vulkan* v) {
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
static bool shadowPassMake(Vulkan* v) {
    const AttachmentDescription attachment = {0, FORMAT_D32, 1, 0, 0, 2, 1, LAYOUT_DEPTH_READ, LAYOUT_DEPTH_READ};
    const AttachmentReference depth = {0, LAYOUT_DEPTH};
    const SubpassDescription subpass = {0, 0, 0, NULL, 0, NULL, NULL, &depth, 0, NULL};
    const VkFlags tests = STAGE_EARLY_TESTS | STAGE_LATE_TESTS;
    const SubpassDependency dependencies[2] = {
        {~0u, 0, STAGE_FRAGMENT, tests, ACCESS_SHADER_READ, ACCESS_DEPTH_READ | ACCESS_DEPTH_WRITE, 0},
        {0, ~0u, tests, STAGE_FRAGMENT, ACCESS_DEPTH_WRITE, ACCESS_SHADER_READ, 0}};
    const RenderPassCreateInfo pass = {ST_RENDER_PASS, NULL, 0, 1, &attachment, 1, &subpass, 2, dependencies};
    return v->vkCreateRenderPass(v->device, &pass, NULL, &v->shadowPass) == VK_OK;
}

/* An atlas of this size, in the layout the frame's pass reads. */
static bool shadowAtlasMake(Vulkan* v, int width, int height) {
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

/* What the scene's pixel shaders are made with, beyond floats: what vkDraw finds the scene asks for. */
typedef struct Features { bool probes, walls, froxels, coats, softShadows, shapes, viewShaders, meshletShaders, tracedShadows, tracedHardware; } Features;

static Features featuresOf(const Vulkan* v) {
    const Features f = {v->probes, v->walls, v->froxels, v->coats, v->softShadows, v->shapes, v->viewShaders, v->meshletShaders, v->tracedShadows, v->tracedHardware};
    return f;
}

static void featuresSet(Vulkan* v, const Features* f) {
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

static uint32_t variantOf(bool floats, const Features* f) {
    return (floats ? ARTF_SHADER_FLOATS : 0u) | (f->probes ? ARTF_SHADER_PROBES : 0u) | (f->probes && f->walls ? ARTF_SHADER_WALLS : 0u) | (f->froxels ? ARTF_SHADER_FROXELS : 0u) |
           (f->coats ? ARTF_SHADER_COAT : 0u) | (f->softShadows ? ARTF_SHADER_SOFT : 0u) | (f->viewShaders ? ARTF_SHADER_VIEW : 0u) | (f->meshletShaders ? ARTF_SHADER_MESHLETS : 0u) | (f->tracedShadows ? ARTF_SHADER_TRACED : 0u) | (f->shapes ? ARTF_SHADER_SHAPES : 0u) | (f->tracedShadows && f->tracedHardware ? ARTF_SHADER_RAY_QUERY : 0u);
}

static uint32_t shaderVariant(const Vulkan* v) {
    const Features f = featuresOf(v);
    return variantOf(v->floats, &f);
}

/* A pipeline no frame will take from now on: destroyed when the frames before this one are done (retiredFree), so
 * that making another needs no wait for the device. So too what the ray tracing hardware no longer takes: a bottom
 * level, a buffer (of bottom levels, scratch, corners). */
typedef struct Retired { VkPipeline pipeline; VkAccelerationStructureKHR structure; GpuBuffer buffer; uint64_t frame; } Retired;

static void pipelineRetire(Vulkan* v, VkPipeline pipeline) {
    if (!pipeline) return;
    Retired r;
    memset(&r, 0, sizeof r);
    r.pipeline = pipeline;
    r.frame = v->frameNumber;
    artfVecPush(&v->retired, r);
}

static void structureRetire(Vulkan* v, VkAccelerationStructureKHR structure) {
    if (!structure) return;
    Retired r;
    memset(&r, 0, sizeof r);
    r.structure = structure;
    r.frame = v->frameNumber;
    artfVecPush(&v->retired, r);
}

/* (the buffer is the list's now: b is left empty) */
static void bufferRetire(Vulkan* v, GpuBuffer* b) {
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
static void retiredFree(Vulkan* v, bool all) {
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
static uint32_t* sceneFragmentCode(const Vulkan* v, bool cutout, const ArtfGraphData* graphs, uint32_t count, uint32_t variant, size_t* size) {
    if (!cutout && !(variant & ARTF_SHADER_TRACED)) graphs = NULL, count = 0;
    return artfShaderSceneFragment(size, v->maxTextures, cutout, graphs, count, 0, variant);
}

/* The overdraw view's: each triangle adds its step wherever it is, whatever is in front (artfShaderOverdraw). */
static bool overdrawPipelinesMake(Vulkan* v) {
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

/* The pipeline of what is plain and solid, from its pixel shader. On any thread: it reads only what stays while it is
 * made (passes are made again only after shaderJobEnd). */
static bool scenePipelineCreate(Vulkan* v, VkShaderModule fragment, VkPipeline* out) {
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
    const GraphicsPipelineCreateInfo info = {ST_GRAPHICS_PIPELINE, NULL, 0, 2, stages, &vertexInput, &assembly, NULL, &viewport, &rasterBack, &multisample, &depthOn, &blendOff, &dynamic, v->pipelineLayout, v->renderPass, 0, 0, -1};
    *out = 0;
    return v->vkCreateGraphicsPipelines(v->device, v->pipelineCache, 1, &info, NULL, out) == VK_OK;
}

/* The pipeline of what is plain and solid, made again: when the probes come or go. */
static bool scenePipelineMake(Vulkan* v, const ArtfGraphData* graphs, uint32_t graphCount) {
    pipelineRetire(v, v->scenePipeline);
    v->scenePipeline = 0;
    size_t size;
    uint32_t* code = sceneFragmentCode(v, false, graphs, graphCount, shaderVariant(v), &size);
    VkShaderModule fragment = shaderModule(v, code, size);
    if (!fragment) return false;
    const bool ok = scenePipelineCreate(v, fragment, &v->scenePipeline);
    v->vkDestroyShaderModule(v->device, fragment, NULL);
    return ok;
}

/* What depends on a scene's materials of its own: all that is drawn with the cutout shader, which has a branch
 * for each. That is what plain surfaces do not pay for. */
/* made[4]: a layer's depth, a layer, what has holes, particles. On any thread, as scenePipelineCreate. */
static bool cutoutPipelinesCreate(Vulkan* v, VkShaderModule fragment, VkPipeline* made) {
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
        {ST_GRAPHICS_PIPELINE, NULL, 0, 2, stages, &vertexInput, &assembly, NULL, &viewport, &rasterNone, &multisample, &depthOn, &blendNone, &dynamic, v->pipelineLayout, v->renderPass, 0, 0, -1},
        {ST_GRAPHICS_PIPELINE, NULL, 0, 2, stages, &vertexInput, &assembly, NULL, &viewport, &rasterNone, &multisample, &depthSame, &blendOn, &dynamic, v->pipelineLayout, v->renderPass, 0, 0, -1},
        /* what has holes or backs: solid, but it decides itself which pixels it has */
        {ST_GRAPHICS_PIPELINE, NULL, 0, 2, stages, &vertexInput, &assembly, NULL, &viewport, &rasterNone, &multisample, &depthOn, &blendOff, &dynamic, v->pipelineLayout, v->renderPass, 0, 0, -1},
        /* particles: a quad has one surface, so one pass does what the two do for a layer */
        {ST_GRAPHICS_PIPELINE, NULL, 0, 2, stages, &vertexInput, &assembly, NULL, &viewport, &rasterNone, &multisample, &depthOn, &blendOn, &dynamic, v->pipelineLayout, v->renderPass, 0, 0, -1}};
    for (int i = 0; i < 4; ++i) made[i] = 0;
    return v->vkCreateGraphicsPipelines(v->device, v->pipelineCache, 4, info, NULL, made) == VK_OK;
}

/* What the cutout shader draws, and what is made from it when first asked (the layers' lists), retired. */
static void cutoutPipelinesRetire(Vulkan* v) {
    VkPipeline* const old[5] = {&v->layerDepthPipeline, &v->layerPipeline, &v->cutoutPipeline, &v->layerListPipeline, &v->particlePipeline};
    for (int i = 0; i < 5; ++i) {
        pipelineRetire(v, *old[i]);
        *old[i] = 0;
    }
}

static bool cutoutPipelinesMake(Vulkan* v, const ArtfGraphData* graphs, uint32_t count) {
    cutoutPipelinesRetire(v);
    size_t size;
    uint32_t* code = sceneFragmentCode(v, true, graphs, count, shaderVariant(v), &size);
    VkShaderModule fragment = shaderModule(v, code, size);
    if (!fragment) return false;
    VkPipeline made[4];
    const bool ok = cutoutPipelinesCreate(v, fragment, made);
    v->layerDepthPipeline = made[0];
    v->layerPipeline = made[1];
    v->cutoutPipeline = made[2];
    v->particlePipeline = made[3];
    v->vkDestroyShaderModule(v->device, fragment, NULL);
    return ok;
}

/* ---- pixel shaders made off the frame ----
 *
 * A feature that asks for another variant of the scene's pixel shaders (probes, their walls, fog's grid, a coat, soft
 * shadows, shapes, a view) has them made on a thread of its own: the frames go on with the shaders they had, and so
 * without the feature, until they are made, a few frames later; then they take the new ones, and the old are destroyed
 * once the frames that drew with them are done. Shadows from rays change what the frame draws in the shadow maps, so
 * they are made before the frame that asks, as are the first ones and those that a program's own materials change: and
 * each of these waits for its compiling, never for the device. artfVulkanShadersWait (the tests) makes every change so. */
bool artfVulkanShadersWait = false;

typedef struct ShaderJob {
    Vulkan* v;
    Features features;
    uint32_t variant;
    ArtfGraphData* graphs; /* a copy: the program may change the scene's while this is made */
    uint32_t graphCount;
    uint64_t scene;
    VkPipeline plain, cutout[4];
    bool ok;
} ShaderJob;

static void shaderJobRun(void* user) {
    ShaderJob* j = user;
    Vulkan* v = j->v;
    size_t size;
    uint32_t* code = sceneFragmentCode(v, false, j->graphs, j->graphCount, j->variant, &size);
    VkShaderModule plain = shaderModule(v, code, size);
    code = sceneFragmentCode(v, true, j->graphs, j->graphCount, j->variant, &size);
    VkShaderModule cutout = shaderModule(v, code, size);
    j->ok = plain && cutout && scenePipelineCreate(v, plain, &j->plain) && cutoutPipelinesCreate(v, cutout, j->cutout);
    if (plain) v->vkDestroyShaderModule(v->device, plain, NULL);
    if (cutout) v->vkDestroyShaderModule(v->device, cutout, NULL);
}

static void shaderJobStart(Vulkan* v, const ArtfScene* scene, const Features* want) {
    ShaderJob* j = ARTF_NEW(ShaderJob);
    j->v = v;
    j->features = *want;
    j->variant = variantOf(v->floats, want);
    j->graphCount = (uint32_t)scene->graphs.len;
    j->scene = scene->id;
    if (j->graphCount) {
        j->graphs = artfMalloc(j->graphCount * sizeof *j->graphs);
        for (uint32_t i = 0; i < j->graphCount; ++i) {
            j->graphs[i] = scene->graphs.items[i];
            j->graphs[i].nodes = artfMalloc((size_t)j->graphs[i].count * sizeof(ArtfGraphNode) + 1);
            memcpy(j->graphs[i].nodes, scene->graphs.items[i].nodes, (size_t)j->graphs[i].count * sizeof(ArtfGraphNode));
        }
    }
    v->shaderJob = j;
    v->shaderTask = artfTaskStart(shaderJobRun, j);
}

/* Waits for what is being made (if anything), and takes it if use and it is still what the scene draws with; or
 * throws it away, as no frame has drawn with it. */
static void shaderJobEnd(Vulkan* v, const ArtfScene* scene, bool use) {
    if (!v->shaderJob) return;
    artfTaskJoin(v->shaderTask); /* (none if artfVulkanWarm waited for it) */
    v->shaderTask = NULL;
    ShaderJob* j = v->shaderJob;
    v->shaderJob = NULL;
    VkPipeline* const made[5] = {&j->plain, &j->cutout[0], &j->cutout[1], &j->cutout[2], &j->cutout[3]};
    if (use && j->ok && scene && j->scene == scene->id && j->graphCount == scene->graphs.len) {
        pipelineRetire(v, v->scenePipeline);
        cutoutPipelinesRetire(v);
        v->scenePipeline = j->plain;
        v->layerDepthPipeline = j->cutout[0];
        v->layerPipeline = j->cutout[1];
        v->cutoutPipeline = j->cutout[2];
        v->particlePipeline = j->cutout[3];
        featuresSet(v, &j->features);
    } else {
        for (int i = 0; i < 5; ++i)
            if (*made[i]) v->vkDestroyPipeline(v->device, *made[i], NULL);
    }
    for (uint32_t i = 0; i < j->graphCount; ++i) artfFree(j->graphs[i].nodes);
    artfFree(j->graphs);
    artfFree(j);
}

static void bindStorage(Vulkan* v, Slot* s, uint32_t binding, const GpuBuffer* b);

/* Layers by each pixel's depth, when a scene first asks: their lists, for this size, and what fills and reads them. */
static bool layerListsMake(Vulkan* v, const ArtfScene* scene) {
    const VkDeviceSize pixels = (VkDeviceSize)v->sceneWidth * v->sceneHeight;
    if (v->layerCounts.size < pixels * 4) {
        v->vkDeviceWaitIdle(v->device);
        bufferFree(v, &v->layerCounts);
        bufferFree(v, &v->layerFragments);
        if (!bufferNew(v, &v->layerCounts, pixels * 4, BUFFER_STORAGE | BUFFER_TRANSFER_DST, false)) return false;
        if (!bufferNew(v, &v->layerFragments, pixels * ARTF_LAYER_FRAGMENTS * 12, BUFFER_STORAGE, false)) return false;
        for (int i = 0; i < SLOTS; ++i) {
            bindStorage(v, &v->slots[i], 44, &v->layerCounts);
            bindStorage(v, &v->slots[i], 45, &v->layerFragments);
        }
    }
    if (v->layerListPipeline && v->layerResolvePipeline) return true;
    size_t size;
    uint32_t* code = artfShaderSceneFragment(&size, v->maxTextures, true, scene->graphs.items, (uint32_t)scene->graphs.len, v->samples, shaderVariant(v) & ~(uint32_t)ARTF_SHADER_FLOATS);
    VkShaderModule fragment = shaderModule(v, code, size);
    code = artfShaderOverlayVertex(&size);
    VkShaderModule whole = shaderModule(v, code, size);
    code = artfShaderLayerResolve(&size, v->samples);
    VkShaderModule resolve = shaderModule(v, code, size);
    if (!fragment || !whole || !resolve) return false;
    const VertexBinding binding = {0, sizeof(ArtfPackedVertex), 0};
    const VertexAttribute attributes[3] = {{0, 0, FORMAT_RGBA16_UNORM, 0}, {1, 0, FORMAT_RG16_SNORM, 8}, {2, 0, FORMAT_RG16_FLOAT, 12}};
    const PipelineVertexInput vertexInput = {ST_PIPELINE_VERTEX_INPUT, NULL, 0, 1, &binding, 3, attributes};
    const PipelineVertexInput noInput = {ST_PIPELINE_VERTEX_INPUT, NULL, 0, 0, NULL, 0, NULL};
    const PipelineInputAssembly assembly = {ST_PIPELINE_INPUT_ASSEMBLY, NULL, 0, 3 /* triangle list */, 0};
    const PipelineViewport viewport = {ST_PIPELINE_VIEWPORT, NULL, 0, 1, NULL, 1, NULL};
    const PipelineRasterization rasterNone = {ST_PIPELINE_RASTERIZATION, NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1.f};
    const PipelineMultisample multisample = {ST_PIPELINE_MULTISAMPLE, NULL, 0, v->samples, 0, 0, NULL, 0, 0};
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
    const PipelineShaderStage resolveStages[2] = {{ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_VERTEX, whole, "main", NULL}, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_FRAGMENT, resolve, "main", NULL}};
    const GraphicsPipelineCreateInfo info[2] = {
        {ST_GRAPHICS_PIPELINE, NULL, 0, 2, listStages, &vertexInput, &assembly, NULL, &viewport, &rasterNone, &multisample, &depthLook, &blendNone, &dynamic, v->pipelineLayout, v->renderPass, 0, 0, -1},
        {ST_GRAPHICS_PIPELINE, NULL, 0, 2, resolveStages, &noInput, &assembly, NULL, &viewport, &rasterNone, &multisample, &depthOff, &blendOn, &dynamic, v->pipelineLayout, v->samples > 1 ? v->resolvePass : v->renderPass, 0, 0, -1}};
    VkPipeline made[2] = {0, 0};
    const bool ok = v->vkCreateGraphicsPipelines(v->device, v->pipelineCache, 2, info, NULL, made) == VK_OK;
    if (v->layerResolvePipeline) v->vkDestroyPipeline(v->device, v->layerResolvePipeline, NULL);
    v->layerListPipeline = made[0];
    v->layerResolvePipeline = made[1];
    v->vkDestroyShaderModule(v->device, fragment, NULL);
    v->vkDestroyShaderModule(v->device, whole, NULL);
    v->vkDestroyShaderModule(v->device, resolve, NULL);
    return ok;
}

static bool pipelinesMake(Vulkan* v) {
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
    if (!renderPassMake(v, &v->renderPass, false, false, finalLayout, false)) return false;
    if (!renderPassMake(v, &v->earlyPass, false, true, LAYOUT_COLOR, false)) return false;
    if (!renderPassMake(v, &v->latePass, true, false, finalLayout, false)) return false;
    if (!renderPassMake(v, &v->midPass, true, true, LAYOUT_COLOR, false)) return false;
    if (!renderPassMake(v, &v->resolvePass, true, true, LAYOUT_COLOR, true)) return false;

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
        {44, DESCRIPTOR_STORAGE, 1, SHADER_FRAGMENT, NULL}, {45, DESCRIPTOR_STORAGE, 1, SHADER_FRAGMENT, NULL},
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
    DescriptorSetLayoutBinding all[70];
    uint32_t bindingCount = v->floats ? 64 : 54;
    memcpy(all, bindings, bindingCount * sizeof *all);
    if (v->floats) {
        /* (what the vertices were a frame ago; motion blur's velocities) */
        const DescriptorSetLayoutBinding past = {68, DESCRIPTOR_STORAGE, 1, SHADER_VERTEX, NULL}, blur = {69, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL};
        all[bindingCount++] = past;
        all[bindingCount++] = blur;
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
    const DescriptorSetLayoutCreateInfo layout = {ST_DESCRIPTOR_SET_LAYOUT, NULL, 0, bindingCount, all};
    if (v->vkCreateDescriptorSetLayout(v->device, &layout, NULL, &v->setLayout) != VK_OK) return false;
    /* (set 1: light, the second light and the history, by the part each plays: see roleSets) */
    const DescriptorSetLayoutBinding roles[3] = {{0, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE | SHADER_FRAGMENT, NULL}, {1, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}, {2, DESCRIPTOR_STORAGE, 1, SHADER_COMPUTE, NULL}};
    const DescriptorSetLayoutCreateInfo roleLayout = {ST_DESCRIPTOR_SET_LAYOUT, NULL, 0, 3, roles};
    if (v->vkCreateDescriptorSetLayout(v->device, &roleLayout, NULL, &v->roleLayout) != VK_OK) return false;
    const VkDescriptorSetLayout setLayouts[2] = {v->setLayout, v->roleLayout};
    const PushConstantRange push[2] = {{SHADER_COMPUTE, 0, 4}, {SHADER_VERTEX, 4, 4}};
    const PipelineLayoutCreateInfo pipelineLayout = {ST_PIPELINE_LAYOUT, NULL, 0, 2, setLayouts, 2, push};
    if (v->vkCreatePipelineLayout(v->device, &pipelineLayout, NULL, &v->pipelineLayout) != VK_OK) return false;

    /* (54 storage bindings of the 64, then 65 to 69) */
    const DescriptorPoolSize sizes[4] = {{DESCRIPTOR_UNIFORM, SLOTS * 4}, {DESCRIPTOR_STORAGE, SLOTS * 59}, {DESCRIPTOR_COMBINED, SLOTS * (v->maxTextures + 5)}, {DESCRIPTOR_AS, SLOTS}};
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
    code = artfShaderOverlayFragment(&size);
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
        uint32_t* (*const makers[16])(size_t*) = {NULL, artfShaderPostOcclusion, artfShaderPostOcclusionBlur, artfShaderPostReflection, artfShaderPostFroxels,
                                                  artfShaderPostTemporal, artfShaderPostFocus, artfShaderPostBloomDown, artfShaderPostBloomUp, artfShaderPostReflectionApply,
                                                  artfShaderPostMeter, artfShaderPostMotionBlur, artfShaderPostVelocity, artfShaderPostTileMost, artfShaderPostUpscale,
                                                  artfShaderPostSharpen};
        for (int i = 0; i < 16 && ok; ++i) {
            uint32_t* postCode = i == 0 ? artfShaderPostGather(&size, v->samples) : makers[i](&size);
            VkShaderModule module = shaderModule(v, postCode, size);
            const ComputePipelineCreateInfo c = {ST_COMPUTE_PIPELINE, NULL, 0, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_COMPUTE, module, "main", NULL}, v->pipelineLayout, 0, -1};
            ok = module && v->vkCreateComputePipelines(v->device, v->pipelineCache, 1, &c, NULL, &v->postPipelines[i]) == VK_OK;
            if (module) v->vkDestroyShaderModule(v->device, module, NULL);
        }
        uint32_t* finalCode = artfShaderPostFinal(&size);
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
    ok = ok && cutoutPipelinesMake(v, NULL, 0);

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

static void writeBufferDescriptors(Vulkan* v, Slot* s) {
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
static bool flagsMake(Vulkan* v) {
    bufferFree(v, &v->flags);
    if (!bufferNew(v, &v->flags, (VkDeviceSize)v->instanceCapacity * 4, BUFFER_STORAGE | BUFFER_TRANSFER_DST, false)) return false;
    VkCommandBuffer c = oneShotBegin(v);
    v->vkCmdFillBuffer(c, v->flags.buffer, 0, ~(VkDeviceSize)0, 0);
    return oneShotEnd(v, c);
}

/* Copies data to a buffer of the GPU's. */
static bool upload(Vulkan* v, GpuBuffer* to, VkDeviceSize offset, const void* data, VkDeviceSize size) {
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

static bool takesFormat(const Vulkan* v, uint32_t format) { return format >= 157 ? v->takesAstc : format >= 147 ? v->takesEtc : v->takesBc; }

/* Blocks go up as they are, where the device takes them; plain pixels are made into blocks if that was asked. */
static bool uploadTexture(Vulkan* v, ArtfTextureData* t, uint32_t index, bool compress) {
    GpuImage image;
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
    /* what is resident: the mipmaps from that one down, as an image of their own */
    const int first = artfMini(t->residentMip, t->mipCount - 1);
    const uint32_t levels = (uint32_t)(t->mipCount - first);
    int width = t->width, height = t->height;
    for (int level = 0; level < first; ++level) {
        width = artfMaxi(width / 2, 1);
        height = artfMaxi(height / 2, 1);
    }
    if (!imageNew(v, &image, format ? (int)format : FORMAT_BGRA8, width, height, levels, 1, IMAGE_SAMPLED | IMAGE_TRANSFER_DST, ASPECT_COLOR)) return false;
    BufferImageCopy regions[16];
    for (uint32_t level = 0, w = (uint32_t)width, h = (uint32_t)height; level < levels; ++level) {
        BufferImageCopy r = {(VkDeviceSize)offsets[(uint32_t)first + level], 0, 0, {ASPECT_COLOR, level, 0, 1}, {0, 0, 0}, {w, h, 1}};
        regions[level] = r;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    GpuBuffer staging;
    if (!bufferNew(v, &staging, bytes, BUFFER_TRANSFER_SRC, true)) return false;
    memcpy(staging.mapped, source, bytes);
    artfFree(encoded);
    VkCommandBuffer c = oneShotBegin(v);
    imageBarrier(v, c, image.image, levels, LAYOUT_UNDEFINED, LAYOUT_TRANSFER_DST, STAGE_TOP, STAGE_TRANSFER, 0, ACCESS_TRANSFER_WRITE);
    v->vkCmdCopyBufferToImage(c, staging.buffer, image.image, LAYOUT_TRANSFER_DST, levels, regions);
    imageBarrier(v, c, image.image, levels, LAYOUT_TRANSFER_DST, LAYOUT_SHADER_READ, STAGE_TRANSFER, STAGE_FRAGMENT, ACCESS_TRANSFER_WRITE, ACCESS_SHADER_READ);
    bool ok = oneShotEnd(v, c);
    bufferFree(v, &staging);
    const uint64_t mark = (uint64_t)t->generation << 8 | (uint64_t)first;
    if (index < v->textures.len) {
        imageFree(v, &v->textures.items[index]);
        v->textures.items[index] = image;
        v->textureMarks.items[index] = mark;
    } else {
        artfVecPush(&v->textures, image);
        artfVecPush(&v->textureMarks, mark);
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
static bool colorsReset(Vulkan* v) {
    const uint32_t white = 0xFFFFFFFFu;
    v->colorCount = 1;
    return upload(v, &v->colors, 0, &white, 4);
}

/* The list of copies: two words each, for four parts (early and late, plain and with holes or backs), with
 * room for every instance in each of its levels, as any of them may be the one chosen; then the layers,
 * which the CPU writes. */
static uint32_t layersAt(const Vulkan* v) { return v->instanceCapacity * (ARTF_MAX_LODS + 1) * 4; }
/* (and after all that, a copy for each particle of emitters of many: their instances follow the instances and their
 * motion, at twice the room for instances) */
static uint32_t swarmCopiesAt(const Vulkan* v) { return layersAt(v) + v->instanceCapacity * 3; }
static VkDeviceSize visibleSize(const Vulkan* v) { return ((VkDeviceSize)swarmCopiesAt(v) + v->swarmRoom) * 2 * 4; }
static VkDeviceSize instanceBytes(const Vulkan* v) { return (VkDeviceSize)v->instanceCapacity * 128 + (VkDeviceSize)v->swarmRoom * 64; }

/* (below that many meshlets, about 65,000 triangles, what culling saves is less than it costs) */
enum { MESHLET_COMMANDS = 1 << 16, MESHLET_LEAST = 512, MESHLET_HEADER = 12, MESHLET_DRAWN = 8 };

/* What the shaders need of meshlets, for meshes that have many and do not bend. */
static bool meshletsUp(Vulkan* v, const ArtfScene* scene) {
    size_t rows = 1;
    for (size_t i = 0; i < scene->meshes.len; ++i) {
        const ArtfMeshData* m = &scene->meshes.items[i];
        if (m->meshletCount >= MESHLET_LEAST && !m->skin) rows += (size_t)m->meshletCount * 3;
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
        const bool used = m->meshletCount >= MESHLET_LEAST && !m->skin && !m->removed;
        const uint32_t info[4] = {first, used ? m->meshletCount : 0, range->firstIndex, (uint32_t)range->vertexOffset};
        memcpy(meshes + i * 4, info, sizeof info);
        if (!used) continue;
        v->mostMeshlets = artfMaxi((int)v->mostMeshlets, (int)m->meshletCount);
        for (uint32_t k = 0; k < m->meshletCount; ++k, table += 12) {
            const ArtfMeshlet* l = &m->meshlets[k];
            const uint32_t indices[2] = {l->triangleOffset * 3, l->triangleCount * 3};
            memcpy(table, l->center, 16);
            memcpy(table + 4, l->cone, 16);
            memcpy(table + 8, indices, 8);
        }
        first += m->meshletCount;
    }
    return true;
}

/* What the scene gained since it was last drawn. That is rare, so it may wait for the GPU. */
static bool syncScene(Vulkan* v, ArtfScene* scene) {
    bool rebind = false;
    if (v->sceneId != scene->id) {
        v->vkDeviceWaitIdle(v->device);
        v->sceneId = scene->id;
        for (size_t i = 0; i < v->textures.len; ++i) imageFree(v, &v->textures.items[i]);
        v->textures.len = 0;
        v->textureMarks.len = 0;
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
            if (!m->removed && range->generation == m->generation) continue;
            refill = refill || !m->removed;
            if (!range->indexCount) { range->generation = m->removed ? m->generation : ~m->generation; continue; }
            spanGive(&v->vertexSpans, (uint32_t)range->vertexOffset, range->vertexLength);
            spanGive(&v->indexSpans, range->firstIndex, range->indexCount);
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
        /* in what was given back, or after what is there; outgrown: larger buffers, and all goes up again */
        for (;;) {
            bool fits = true;
            for (size_t i = 0; i < scene->meshes.len && fits; ++i) {
                const ArtfMeshData* m = &scene->meshes.items[i];
                if (i < v->meshes.len && (m->removed || v->meshes.items[i].generation == m->generation)) continue;
                MeshRange range = {0, 0, 0, 0, m->skin ? m->pose.applied : m->deformVersion, 0, 0, m->generation};
                if (m->vertexCount && !m->evicted) {
                    const uint32_t atVertex = spanTake(&v->vertexSpans, m->vertexCount, &v->vertexCount, (uint32_t)(v->vertices.size / sizeof(ArtfPackedVertex)));
                    const uint32_t atIndex = atVertex == UINT32_MAX ? UINT32_MAX : spanTake(&v->indexSpans, m->indexCount, &v->indexCount, (uint32_t)(v->indices.size / 4));
                    const uint32_t atColor = atIndex == UINT32_MAX || !m->colors ? 0 : spanTake(&v->colorSpans, m->vertexCount, &v->colorCount, (uint32_t)(v->colors.size / 4));
                    if (atVertex == UINT32_MAX || atIndex == UINT32_MAX || atColor == UINT32_MAX) {
                        fits = false;
                        break;
                    }
                    if (!upload(v, &v->vertices, (VkDeviceSize)atVertex * sizeof(ArtfPackedVertex), m->vertices, (VkDeviceSize)m->vertexCount * sizeof(ArtfPackedVertex))) return false;
                    if (!upload(v, &v->indices, (VkDeviceSize)atIndex * 4, m->indices, (VkDeviceSize)m->indexCount * 4)) return false;
                    if (m->colors && !upload(v, &v->colors, (VkDeviceSize)atColor * 4, m->colors, (VkDeviceSize)m->vertexCount * 4)) return false;
                    range.firstIndex = atIndex;
                    range.vertexOffset = (int32_t)atVertex;
                    range.indexCount = m->indexCount;
                    range.colorBase = atColor;
                    range.vertexLength = m->vertexCount;
                    range.colorLength = m->colors ? m->vertexCount : 0;
                }
                if (i < v->meshes.len) v->meshes.items[i] = range;
                else artfVecPush(&v->meshes, range);
            }
            if (fits) break;
            uint64_t vertices = 0, indices = 0, colors = 1;
            for (size_t k = 0; k < scene->meshes.len; ++k) {
                vertices += scene->meshes.items[k].vertexCount;
                indices += scene->meshes.items[k].indexCount;
                if (scene->meshes.items[k].colors) colors += scene->meshes.items[k].vertexCount;
            }
            bufferFree(v, &v->vertices);
            bufferFree(v, &v->indices);
            bufferFree(v, &v->colors);
            if (!bufferNew(v, &v->vertices, vertices * 2 * sizeof(ArtfPackedVertex), BUFFER_VERTEX | BUFFER_STORAGE | BUFFER_TRANSFER_DST | BUFFER_TRANSFER_SRC, false)) return false;
            if (!bufferNew(v, &v->indices, indices * 2 * 4, BUFFER_INDEX | BUFFER_TRANSFER_DST, false)) return false;
            if (!bufferNew(v, &v->colors, colors * 2 * 4, BUFFER_STORAGE | BUFFER_TRANSFER_DST, false) || !colorsReset(v)) return false;
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
        if (!meshletsUp(v, scene)) return false;
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
                /* (white in its place, which every entry of the array must have) */
                if (i >= v->textures.len) continue;
                const DescriptorImageInfo white = {v->sampler, v->textures.items[0].view, LAYOUT_SHADER_READ};
                for (int k = 0; k < SLOTS; ++k) {
                    const WriteDescriptorSet w = {ST_WRITE_DESCRIPTOR_SET, NULL, v->slots[k].set, 6, (uint32_t)i, 1, DESCRIPTOR_COMBINED, &white, NULL, NULL};
                    v->vkUpdateDescriptorSets(v->device, 1, &w, 0, NULL);
                }
                v->textureMarks.items[i] = mark;
                continue;
            }
            bool compress = v->compressTextures && i;
            for (size_t m = 0; m < scene->materials.len && compress; ++m) compress = scene->materials.items[m].normalMap != i;
            if (!uploadTexture(v, t, (uint32_t)i, compress)) return false;
        }
    }

    if (scene->instances.len > v->instanceCapacity || v->swarmPlaces > v->swarmRoom) {
        v->vkDeviceWaitIdle(v->device);
        if (scene->instances.len > v->instanceCapacity) v->instanceCapacity = (uint32_t)scene->instances.len * 2;
        if (v->swarmPlaces > v->swarmRoom) v->swarmRoom = v->swarmPlaces;
        for (int i = 0; i < SLOTS; ++i) {
            Slot* s = &v->slots[i];
            bufferFree(v, &s->instances);
            bufferFree(v, &s->visible);
            if (!bufferNew(v, &s->instances, instanceBytes(v), BUFFER_STORAGE, true)) return false;
            if (!bufferNew(v, &s->visible, visibleSize(v), BUFFER_STORAGE | BUFFER_TRANSFER_DST, false)) return false;
            s->fresh = true;
        }
        if (!flagsMake(v)) return false;
        rebind = true;
    }
    if (scene->materials.len > v->materialCapacity) {
        v->vkDeviceWaitIdle(v->device);
        v->materialCapacity = (uint32_t)scene->materials.len * 2;
        for (int i = 0; i < SLOTS; ++i) {
            Slot* s = &v->slots[i];
            bufferFree(v, &s->materials);
            if (!bufferNew(v, &s->materials, (VkDeviceSize)v->materialCapacity * (sizeof(ArtfMaterialData) + sizeof(ArtfMaterialMore)), BUFFER_STORAGE, true)) return false;
            s->fresh = true;
        }
        rebind = true;
    }
    if (rebind)
        for (int i = 0; i < SLOTS; ++i) {
        writeBufferDescriptors(v, &v->slots[i]);
        lightDescriptors(v, &v->slots[i]);
    }
    return true;
}

/* Brings this slot's copy of what changes up to date. */
static void syncSlot(Vulkan* v, Slot* s, const ArtfScene* scene) {
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

/* ---- light from all around ---- */

/* The scene's maps go up when they change; until a scene has some, a buffer of four bytes stands in, which the
 * shaders do not read. */
static bool syncEnvironment(Vulkan* v, const ArtfScene* scene) {
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

enum {
    BUFFER_DEVICE_ADDRESS = 0x20000, BUFFER_AS_INPUT = 0x80000, BUFFER_AS_STORAGE = 0x100000, STAGE_AS_BUILD = 0x02000000, ACCESS_AS_READ = 0x00200000, ACCESS_AS_WRITE = 0x00400000
};

/* A buffer the GPU knows by its address: where the CPU writes it (inputs), or the GPU's own (the structures, scratch). */
static bool rtBufferNew(Vulkan* v, GpuBuffer* b, VkDeviceSize size, VkFlags usage, bool written) {
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
    v->vkBindBufferMemory(v->device, b->buffer, b->memory, 0);
    b->size = info.size;
    if (written && v->vkMapMemory(v->device, b->memory, 0, ~(VkDeviceSize)0, 0, &b->mapped) != VK_OK) return false;
    return true;
}

static VkDeviceAddress rtAddress(Vulkan* v, const GpuBuffer* b) {
    const BufferDeviceAddressInfo info = {ST_BUFFER_DEVICE_ADDRESS_INFO, NULL, b->buffer};
    return v->vkGetBufferDeviceAddress(v->device, &info);
}

/* ---- tracing ---- */

/* One of the buffers rays read: replaced when it has outgrown its room. built: the vertices, with the hardware, which
 * its bottom levels are built from, by their address. */
static bool traceBuffer(Vulkan* v, GpuBuffer* b, uint32_t binding, const void* data, VkDeviceSize size, bool built) {
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

/* A mesh's bottom level: where it is (a room in one of the blocks, as big as the structure made there), what it was
 * built from, and how often it was refitted since. */
typedef struct RtBottom {
    VkAccelerationStructureKHR structure;
    VkDeviceAddress address;
    uint32_t block;
    VkDeviceSize offset, room, buildScratch, updateScratch;
    uint64_t deform, generation;
    uint32_t firstTriangle, triangleCount, firstVertex, vertexCount;
    uint32_t refits;  /* since it was built */
    float extent;     /* the diagonal of its box when it was built */
    bool refittable;  /* built to allow an update: what bones or targets move */
} RtBottom;

/* Bottom levels side by side, each in its room; a block goes once none is left in it. */
typedef struct RtBlock { GpuBuffer buffer; VkDeviceSize used; uint32_t live; } RtBlock;

/* Of each mesh, the pose its nodes and vertices on the GPU are at. */
typedef struct TraceUp { uint64_t deform; bool moved; } TraceUp;

/* A refit makes the boxes fit where the triangles went, not the tree fit them: after so many, or once the mesh's box
 * has grown by half, it is built again. */
enum { RT_REFITS = 60 };

/* The structure goes once the frames that may read it are done; its room, with the block once that is empty. */
static void rtRelease(Vulkan* v, RtBottom* b) {
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
static void rtBottomsFree(Vulkan* v, bool now) {
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

static void rtFree(Vulkan* v) {
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
static bool rtRoom(Vulkan* v, RtBottom* b, VkDeviceSize size, VkDeviceSize block) {
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
static bool rtIndicesSync(Vulkan* v, const ArtfScene* scene) {
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
static AsGeometry rtTriangles(VkDeviceAddress vertices, VkDeviceAddress corners, const ArtfTraceRange* r, uint32_t vertexCount) {
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
static float rtExtent(const ArtfTrace* t, const ArtfTraceRange* r, uint32_t vertexCount) {
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

/* What one bottom level does this frame. */
typedef struct RtPlan { uint32_t mesh; bool update, fresh; VkDeviceSize size, scratch; } RtPlan;

/* The trees' lengths, as they are on the GPU: when they are the same, only poses changed them. */
static bool traceLayoutSame(const Vulkan* v, const ArtfScene* scene) {
    const ArtfTrace* t = &scene->trace;
    return v->traceUp && v->traceScene == scene->id && v->traceRemovals == t->meshRemovals && v->traceLengths[0] == t->nodes.len && v->traceLengths[1] == t->triangles.len &&
           v->traceLengths[2] == t->vertices.len && v->traceLengths[3] == t->ranges.len;
}

/* What bones or targets moved of the trees: each such mesh's nodes and vertices, from the slot's staging into the trees,
 * in the frame's commands. Recorded in v->traceCopies; false when out of memory. */
static bool traceMovedUp(Vulkan* v, Slot* s, const ArtfScene* scene) {
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
        if (r->nodeEnd > r->firstNode) {
            const BufferCopy n = {bytes, (VkDeviceSize)r->firstNode * sizeof(ArtfTraceNode), (VkDeviceSize)(r->nodeEnd - r->firstNode) * sizeof(ArtfTraceNode)};
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
    const char* from[2] = {(const char*)t->nodes.items, (const char*)t->vertices.items};
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
static bool slotMeshSync(Vulkan* v, Slot* s, VkCommandBuffer c, const ArtfScene* scene, bool hardware) {
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
            if (!r || !r->triangleCount || m >= scene->meshes.len) {
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
static bool slotTraceBuffer(Vulkan* v, Slot* s, GpuBuffer* b, uint32_t binding, const void* data, VkDeviceSize size) {
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
static bool slotTopSync(Vulkan* v, Slot* s, VkCommandBuffer c, const ArtfScene* scene, bool hardware) {
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

_Static_assert(sizeof(AsGeometryTriangles) == 64 && sizeof(AsGeometry) == 96 && sizeof(AsBuildGeometryInfo) == 80 && sizeof(AsInstance) == 64 && sizeof(AsCreateInfo) == 64,
               "the ray tracing structures are laid out as Vulkan's");

/* The trees rays walk, as tracing holds them (artfSceneTraceSync first): sent up when they changed, waiting for the
 * device, when meshes came or went. When only poses changed them, what they moved goes up in the frame's own commands,
 * and so do the bottom levels (slotMeshSync); what moves with the instances is the slot's (slotTopSync). */
static bool traceBuffersSync(Vulkan* v, const ArtfScene* scene) {
    const ArtfTrace* t = &scene->trace;
    if ((v->traceScene != scene->id || v->traceMeshVersion != t->meshVersion) && !traceLayoutSame(v, scene)) {
        v->vkDeviceWaitIdle(v->device);
        if (!traceBuffer(v, &v->traceNodes, 17, t->nodes.items, t->nodes.len * sizeof *t->nodes.items, false)) return false;
        if (!traceBuffer(v, &v->traceTriangles, 18, t->triangles.items, t->triangles.len * sizeof *t->triangles.items, false)) return false;
        if (!traceBuffer(v, &v->traceVertices, 19, t->vertices.items, t->vertices.len * sizeof *t->vertices.items, v->rayQuery)) return false;
        v->traceMeshVersion = t->meshVersion;
        v->traceRemovals = t->meshRemovals;
        v->traceLengths[0] = t->nodes.len;
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
 * out), with the hardware's structures where there are some; and the reflections' pass, made when first asked. */
static bool syncTraced(Vulkan* v, ArtfScene* scene, bool reflections) {
    artfSceneTraceSync(scene);
    if (!traceBuffersSync(v, scene)) return false;
    const bool hardware = v->rayQuery;
    if (reflections && v->reflectionTraced && v->reflectionTracedHardware != hardware) {
        v->vkDeviceWaitIdle(v->device);
        v->vkDestroyPipeline(v->device, v->reflectionTraced, NULL);
        v->reflectionTraced = 0;
    }
    if (reflections && !v->reflectionTraced) {
        size_t size;
        v->reflectionTracedHardware = hardware;
        uint32_t* code = artfShaderPostReflectionTraced(&size, v->maxTextures, scene->graphs.items, (uint32_t)scene->graphs.len, hardware);
        VkShaderModule module = shaderModule(v, code, size);
        if (!module) return false;
        const ComputePipelineCreateInfo info = {ST_COMPUTE_PIPELINE, NULL, 0, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_COMPUTE, module, "main", NULL}, v->pipelineLayout, 0, -1};
        const bool ok = v->vkCreateComputePipelines(v->device, v->pipelineCache, 1, &info, NULL, &v->reflectionTraced) == VK_OK;
        v->vkDestroyShaderModule(v->device, module, NULL);
        if (!ok) return false;
    }
    return true;
}

static bool syncTrace(Vulkan* v, ArtfScene* scene) {
    artfSceneTraceSync(scene);
    const ArtfTrace* t = &scene->trace;
    const bool guides = t->desc.denoise, hardware = t->desc.hardware && v->rayQuery;
    const bool more = scene->moreMaterials > 0 || artfSceneShapedLights(scene);
    if (v->tracePipeline && (v->traceGuides != guides || v->traceHardware != hardware || v->traceMore != more)) {
        v->vkDeviceWaitIdle(v->device);
        v->vkDestroyPipeline(v->device, v->tracePipeline, NULL);
        v->tracePipeline = 0;
    }
    if (!v->tracePipeline) {
        size_t size;
        v->traceGuides = guides;
        v->traceHardware = hardware;
        v->traceMore = more;
        if (hardware) artfLog("artful: rays go through the ray tracing hardware");
        uint32_t* code = artfShaderTrace(&size, v->maxTextures, scene->graphs.items, (uint32_t)scene->graphs.len, guides, hardware, more);
        VkShaderModule module = shaderModule(v, code, size);
        if (!module) return false;
        const ComputePipelineCreateInfo info = {ST_COMPUTE_PIPELINE, NULL, 0, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_COMPUTE, module, "main", NULL}, v->pipelineLayout, 0, -1};
        const bool ok = v->vkCreateComputePipelines(v->device, v->pipelineCache, 1, &info, NULL, &v->tracePipeline) == VK_OK;
        v->vkDestroyShaderModule(v->device, module, NULL);
        if (!ok) return false;
    }
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
            if (!bufferNew(v, &v->denoiseBuffers[i], (VkDeviceSize)v->sceneWidth * v->sceneHeight * 16, BUFFER_STORAGE | BUFFER_TRANSFER_SRC | BUFFER_TRANSFER_DST, false)) return false;
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
    if (guides && !v->denoiseTime) {
        size_t size;
        uint32_t* codes[6];
        size_t sizes[6];
        codes[0] = artfShaderDenoiseTime(&size); sizes[0] = size;
        for (int i = 0; i < 3; ++i) { codes[1 + i] = artfShaderDenoiseSpace(&size, i); sizes[1 + i] = size; }
        codes[4] = artfShaderDenoiseEnd(&size); sizes[4] = size;
        codes[5] = artfShaderDenoiseLight(&size); sizes[5] = size;
        VkShaderModule modules[6];
        ComputePipelineCreateInfo info[6];
        for (int i = 0; i < 6; ++i) {
            modules[i] = shaderModule(v, codes[i], sizes[i]);
            const ComputePipelineCreateInfo c = {ST_COMPUTE_PIPELINE, NULL, 0, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_COMPUTE, modules[i], "main", NULL}, v->pipelineLayout, 0, -1};
            info[i] = c;
        }
        VkPipeline made[6];
        const bool ok = v->vkCreateComputePipelines(v->device, v->pipelineCache, 6, info, NULL, made) == VK_OK;
        for (int i = 0; i < 6; ++i) v->vkDestroyShaderModule(v->device, modules[i], NULL);
        if (!ok) return false;
        v->denoiseTime = made[0];
        memcpy(v->denoiseSpace, made + 1, sizeof v->denoiseSpace);
        v->denoiseEnd = made[4];
        v->denoiseLight = made[5];
    }
    return true;
}

/* The frame's light, time, three passes in space and back to light; then what this frame found becomes the frame
 * before. */
static void denoise(Vulkan* v, VkCommandBuffer c, Slot* s) {
    const VkFlags readWrite = ACCESS_SHADER_READ | ACCESS_SHADER_WRITE;
    const uint32_t gx = ((uint32_t)v->sceneWidth + 7) / 8, gy = ((uint32_t)v->sceneHeight + 7) / 8;
    const VkPipeline passes[6] = {v->denoiseLight, v->denoiseTime, v->denoiseSpace[0], v->denoiseSpace[1], v->denoiseSpace[2], v->denoiseEnd};
    v->vkCmdBindDescriptorSets(c, 1, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
    for (int i = 0; i < 6; ++i) {
        memoryBarrier(v, c, STAGE_COMPUTE, STAGE_COMPUTE, ACCESS_SHADER_WRITE, readWrite);
        v->vkCmdBindPipeline(c, 1, passes[i]);
        v->vkCmdDispatch(c, gx, gy, 1);
    }
    memoryBarrier(v, c, STAGE_COMPUTE, STAGE_TRANSFER | STAGE_FRAGMENT, ACCESS_SHADER_WRITE, ACCESS_TRANSFER_READ | ACCESS_SHADER_READ);
    /* guide to the frame before's, what is gathered to what was */
    const BufferCopy all = {0, 0, (VkDeviceSize)v->sceneWidth * v->sceneHeight * 16};
    v->vkCmdCopyBuffer(c, v->denoiseBuffers[0].buffer, v->denoiseBuffers[2].buffer, 1, &all);
    v->vkCmdCopyBuffer(c, v->denoiseBuffers[4].buffer, v->denoiseBuffers[3].buffer, 1, &all);
    memoryBarrier(v, c, STAGE_TRANSFER, STAGE_COMPUTE, ACCESS_TRANSFER_WRITE, readWrite);
}

/* ---- a background pass ---- */

/* The program's shader, as a pipeline, when the scene's is another. It goes out in bands, each from a base:
 * VK_PIPELINE_CREATE_DISPATCH_BASE_BIT. No shader, or one the driver will not take: no pipeline, and no light. */
static void backgroundPipelineMake(Vulkan* v, const ArtfScene* scene) {
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
static bool syncBackground(Vulkan* v, const ArtfScene* scene, uint32_t bloomPixels) {
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

static uint32_t backgroundRaysPerPixel(const ArtfScene* scene) { return (uint32_t)artfMaxi(scene->backgroundPass.pass.raysPerPixel, 1); }

static uint64_t backgroundRays(const Vulkan* v, const ArtfScene* scene) {
    return (uint64_t)v->sceneWidth * (uint64_t)v->sceneHeight * backgroundRaysPerPixel(scene);
}

static bool traceBackgroundApart(Vulkan* v, Slot* s, const ArtfScene* scene) {
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
static void traceBackground(Vulkan* v, VkCommandBuffer c, Slot* s, const ArtfBackgroundBlock* h, bool raysDone) {
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

static void vkSetOverlay(ArtfRenderer* r, const ArtfOverlay* o) {
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
static bool uploadOverlay(Vulkan* v, Slot* s, VkCommandBuffer c) {
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
static void drawCommands(Vulkan* v, VkCommandBuffer c, Slot* s, VkPipeline pipeline, uint32_t first, uint32_t count) {
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
static bool bonesAlone(const ArtfMeshData* m) {
    if (!m->skin->joints || !m->pose.jointCount || m->pose.jointCount > 65535) return false;
    for (uint32_t t = 0; m->pose.weights && t < m->skin->morphCount; ++t)
        if (m->pose.weights[t] != 0) return false;
    return true;
}

/* All skins in use go up together, when one is new: rare. */
static bool syncSkinData(Vulkan* v, const ArtfScene* scene) {
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

static bool shadowCullMake(Vulkan* v) {
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

static void bindStorage(Vulkan* v, Slot* s, uint32_t binding, const GpuBuffer* b);

static bool meshletPipelineMake(Vulkan* v) {
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
static bool meshletFlagsUp(Vulkan* v, const ArtfScene* scene) {
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
static uint32_t meshletHalf(const Vulkan* v) { return v->instanceCapacity * 4 + MESHLET_HEADER; }

/* This slot's room for jobs, and its header for this frame: 0 commands fit when meshlets are not for this frame. */
static bool stageMeshlets(Vulkan* v, Slot* s, bool on) {
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
    if (s->meshletBoundTable != v->meshletTable.buffer || !s->meshletBound) {
        bindStorage(v, s, 40, &v->meshletTable);
        bindStorage(v, s, 41, &v->meshletMeshes);
        s->meshletBoundTable = v->meshletTable.buffer;
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

static bool skinPipelineMake(Vulkan* v) {
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
static bool stageBones(Vulkan* v, Slot* s, const ArtfScene* scene) {
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
    if (rebind || s->skinBoundData != v->skinData.buffer || s->skinBoundVertices != v->vertices.buffer) {
        const DescriptorBufferInfo infos[3] = {{v->skinData.buffer, 0, ~(VkDeviceSize)0}, {s->skinTable.buffer, 0, ~(VkDeviceSize)0}, {v->vertices.buffer, 0, ~(VkDeviceSize)0}};
        for (uint32_t i = 0; i < 3; ++i) {
            const WriteDescriptorSet w = {ST_WRITE_DESCRIPTOR_SET, NULL, s->set, 34 + i, 0, 1, DESCRIPTOR_STORAGE, NULL, &infos[i], NULL};
            v->vkUpdateDescriptorSets(v->device, 1, &w, 0, NULL);
        }
        s->skinBoundData = v->skinData.buffer;
        s->skinBoundVertices = v->vertices.buffer;
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

static void runBones(Vulkan* v, VkCommandBuffer c, Slot* s) {
    if (!v->skinJobs.len) return;
    memoryBarrier(v, c, STAGE_VERTEX_INPUT, STAGE_COMPUTE, ACCESS_VERTEX_READ, ACCESS_SHADER_WRITE);
    v->vkCmdBindPipeline(c, 1, v->skinPipeline);
    v->vkCmdBindDescriptorSets(c, 1, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
    for (uint32_t i = 0; i < (uint32_t)v->skinJobs.len; ++i) {
        v->vkCmdPushConstants(c, v->pipelineLayout, SHADER_COMPUTE, 0, 4, &i);
        const uint32_t count = v->scratchSkinCounts.items[i];
        v->vkCmdDispatch(c, (count + 63) / 64, 1, 1);
    }
    memoryBarrier(v, c, STAGE_COMPUTE, STAGE_VERTEX_INPUT, ACCESS_SHADER_WRITE, ACCESS_VERTEX_READ);
}

static bool stagePoses(Vulkan* v, Slot* s, ArtfScene* scene) {
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
static void bindStorage(Vulkan* v, Slot* s, uint32_t binding, const GpuBuffer* b) {
    const DescriptorBufferInfo info = {b->buffer, 0, ~(VkDeviceSize)0};
    const WriteDescriptorSet w = {ST_WRITE_DESCRIPTOR_SET, NULL, s->set, binding, 0, 1, DESCRIPTOR_STORAGE, NULL, &info, NULL};
    v->vkUpdateDescriptorSets(v->device, 1, &w, 0, NULL);
}

static bool syncShadowList(Vulkan* v, Slot* s, const ArtfScene* scene, uint32_t views) {
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

static void shadowDraw(Vulkan* v, VkCommandBuffer c, Slot* s, VkPipeline pipeline, uint32_t first, uint32_t count) {
    v->vkCmdBindPipeline(c, 0, pipeline);
    if (v->multiDraw) {
        v->vkCmdDrawIndexedIndirect(c, s->shadowViewCommands.buffer, (VkDeviceSize)first * 20, count, 20);
    } else {
        for (uint32_t i = 0; i < count; ++i) v->vkCmdDrawIndexedIndirect(c, s->shadowViewCommands.buffer, (VkDeviceSize)(first + i) * 20, 1, 20);
    }
}

/* The maps that changed, each in its square of the atlas, with only what can reach it. */
static void drawShadows(Vulkan* v, VkCommandBuffer c, Slot* s, const ArtfShadowFrame* shadows, uint32_t views, uint64_t changed) {
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
static uint32_t swarmList(Vulkan* v, const ArtfScene* scene) {
    v->swarms.len = 0;
    const uint32_t count = artfSceneSwarms(scene, NULL, 0);
    if (!count) return 0;
    artfVecReserve(&v->swarms, count);
    v->swarms.len = artfSceneSwarms(scene, v->swarms.items, count);
    uint32_t places = 0;
    for (size_t k = 0; k < v->swarms.len; ++k) places += artfSwarmPlaces(v->swarms.items[k]);
    return places;
}

static bool swarmPipelineMake(Vulkan* v) {
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
static bool syncSwarms(Vulkan* v, Slot* s, const ArtfScene* scene) {
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
                                  v->instanceCapacity * 2 + v->swarmAt.items[k], swarmCopiesAt(v) + v->swarmAt.items[k], scene->particleQuad - 1, 0};
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
static void runSwarms(Vulkan* v, VkCommandBuffer c, Slot* s) {
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
static void drawSwarms(Vulkan* v, VkCommandBuffer c, Slot* s, const ArtfScene* scene) {
    if (!v->swarms.len || !scene->particleQuad) return;
    const MeshRange* quad = &v->meshes.items[scene->particleQuad - 1];
    const VkDeviceSize zero = 0;
    v->vkCmdBindVertexBuffers(c, 0, 1, &v->vertices.buffer, &zero);
    v->vkCmdBindIndexBuffer(c, v->indices.buffer, 0, 1 /* 32 bits */);
    v->vkCmdBindPipeline(c, 0, v->particlePipeline);
    v->vkCmdBindDescriptorSets(c, 0, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
    v->vkCmdDrawIndexed(c, quad->indexCount, v->swarmPlaces, quad->firstIndex, quad->vertexOffset, swarmCopiesAt(v));
}

/* What shows what is behind it, the farthest first: its depth among what is there, and then its colors
 * where that depth is its own, so that only its nearest surface shows. */
static void drawLayers(Vulkan* v, VkCommandBuffer c, const ArtfScene* scene, uint32_t count) {
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

static void lightDescriptors(Vulkan* v, Slot* s) {
    const DescriptorBufferInfo lights = {s->extraLights.buffer, 0, ~(VkDeviceSize)0}, lists = {s->tileLights.buffer, 0, ~(VkDeviceSize)0}, probes = {s->probes.buffer, 0, ~(VkDeviceSize)0};
    const DescriptorBufferInfo decals = {s->decals.buffer, 0, ~(VkDeviceSize)0};
    const WriteDescriptorSet writes[4] = {{ST_WRITE_DESCRIPTOR_SET, NULL, s->set, 60, 0, 1, DESCRIPTOR_STORAGE, NULL, &lights, NULL},
                                          {ST_WRITE_DESCRIPTOR_SET, NULL, s->set, 61, 0, 1, DESCRIPTOR_STORAGE, NULL, &lists, NULL},
                                          {ST_WRITE_DESCRIPTOR_SET, NULL, s->set, 62, 0, 1, DESCRIPTOR_STORAGE, NULL, &probes, NULL},
                                          {ST_WRITE_DESCRIPTOR_SET, NULL, s->set, 63, 0, 1, DESCRIPTOR_STORAGE, NULL, &decals, NULL}};
    v->vkUpdateDescriptorSets(v->device, 4, writes, 0, NULL);
}

/* A step of the probes, on this thread's workers, and what they hold into this slot's buffer. */
static bool syncIndirect(Vulkan* v, Slot* s, ArtfScene* scene, const ArtfFrameData* frame) {
    if (frame->indirect[0][3] <= 0 || scene->trace.desc.mode != ARTF_TRACE_NONE) return true;
    if (!v->pool) v->pool = artfPoolNew(0);
    artfSceneIndirectStep(scene, frame, v->pool);
    const VkDeviceSize need = (VkDeviceSize)artfIndirectCount(&scene->indirect) * ARTF_PROBE_FLOATS * 4;
    if (s->probes.size < need) {
        bufferFree(v, &s->probes);
        if (!bufferNew(v, &s->probes, need, BUFFER_STORAGE, true)) return false;
        lightDescriptors(v, s);
    }
    memcpy(s->probes.mapped, scene->indirect.probes, (size_t)need);
    return true;
}

/* Into this slot's buffers: the lights, and each square's list (its fence was waited for). */
static bool syncLights(Vulkan* v, Slot* s, const ArtfScene* scene, const ArtfFrameData* frame) {
    if (frame->eye[2][3] <= 0) return true;
    const uint32_t tilesX = ((uint32_t)v->sceneWidth + ARTF_LIGHT_TILE - 1) / ARTF_LIGHT_TILE, tilesY = ((uint32_t)v->sceneHeight + ARTF_LIGHT_TILE - 1) / ARTF_LIGHT_TILE;
    const VkDeviceSize need = (VkDeviceSize)tilesX * tilesY * (1 + ARTF_TILE_LIGHTS) * 4;
    if (s->tileLights.size < need) {
        bufferFree(v, &s->tileLights);
        if (!bufferNew(v, &s->tileLights, need, BUFFER_STORAGE, true)) return false;
        lightDescriptors(v, s);
    }
    /* (made in main memory, then copied: what the GPU reads is slow to read back, as the lists' counts are) */
    if (!v->lightScratch) v->lightScratch = artfMalloc(ARTF_MAX_LIGHTS * sizeof(ArtfLightData));
    if (v->listScratchSize < need) {
        artfFree(v->listScratch);
        v->listScratch = artfMalloc((size_t)need);
        v->listScratchSize = need;
    }
    const uint32_t count = artfSceneExtraLights(scene, frame, v->lightScratch);
    artfSceneLightTiles(frame, v->sceneWidth, v->sceneHeight, v->lightScratch, count, v->listScratch);
    memcpy(s->extraLights.mapped, v->lightScratch, (size_t)count * sizeof(ArtfLightData));
    memcpy(s->tileLights.mapped, v->listScratch, (size_t)need);
    return true;
}

/* ---- what moves: the surface's second word, and what the vertices were ---- */

/* The scene's passes again, for the surface's other format, and what is drawn in them. */
static bool scenePassesMake(Vulkan* v, const ArtfScene* scene) {
    shaderJobEnd(v, scene, false);
    VkRenderPass* const passes[5] = {&v->renderPass, &v->earlyPass, &v->latePass, &v->midPass, &v->resolvePass};
    for (int i = 0; i < 5; ++i) {
        if (*passes[i]) v->vkDestroyRenderPass(v->device, *passes[i], NULL);
        *passes[i] = 0;
    }
    const int finalLayout = LAYOUT_SHADER_READ; /* (a frame in floats: presentPass makes the frame) */
    if (!renderPassMake(v, &v->renderPass, false, false, finalLayout, false) || !renderPassMake(v, &v->earlyPass, false, true, LAYOUT_COLOR, false) ||
        !renderPassMake(v, &v->latePass, true, false, finalLayout, false) || !renderPassMake(v, &v->midPass, true, true, LAYOUT_COLOR, false) ||
        !renderPassMake(v, &v->resolvePass, true, true, LAYOUT_COLOR, true))
        return false;
    if (!scenePipelineMake(v, scene->graphs.items, (uint32_t)scene->graphs.len) || !cutoutPipelinesMake(v, scene->graphs.items, (uint32_t)scene->graphs.len)) return false;
    /* (and what is made when first asked for: the overdraw view's, the layers' lists' resolve, made again then) */
    for (int i = 0; i < 2; ++i) {
        if (v->overdrawPipelines[i]) v->vkDestroyPipeline(v->device, v->overdrawPipelines[i], NULL);
        v->overdrawPipelines[i] = 0;
    }
    if (v->layerResolvePipeline) v->vkDestroyPipeline(v->device, v->layerResolvePipeline, NULL);
    v->layerResolvePipeline = 0;
    /* what is behind the scene, what was traced, the fog over what is behind: as pipelinesMake has them */
    VkPipeline* const behind[4] = {&v->backdropPipeline, &v->traceShowPipeline, &v->environmentPipeline, &v->fogPipeline};
    for (int i = 0; i < 4; ++i) {
        if (*behind[i]) v->vkDestroyPipeline(v->device, *behind[i], NULL);
        *behind[i] = 0;
    }
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
            const GraphicsPipelineCreateInfo info = {ST_GRAPHICS_PIPELINE, NULL, 0, 2, stages, &noInput, &assembly, NULL, &viewport, &rasterNone, &multisample, &depthOff, blends[i], &dynamic, v->pipelineLayout, v->renderPass, 0, 0, -1};
            ok = v->vkCreateGraphicsPipelines(v->device, v->pipelineCache, 1, &info, NULL, behind[i]) == VK_OK;
        }
    }
    const VkShaderModule modules[5] = {whole, backdrop, traced, environment, fog};
    for (int i = 0; i < 5; ++i)
        if (modules[i]) v->vkDestroyShaderModule(v->device, modules[i], NULL);
    return ok;
}

/* Room for what the vertex buffer held a frame ago, as large as it is. */
static bool pastVerticesRoom(Vulkan* v) {
    if (v->pastVertices.size >= v->vertices.size) return true;
    v->vkDeviceWaitIdle(v->device);
    bufferFree(v, &v->pastVertices);
    if (!bufferNew(v, &v->pastVertices, v->vertices.size, BUFFER_STORAGE | BUFFER_TRANSFER_DST, false)) return false;
    for (int i = 0; i < SLOTS; ++i) bindStorage(v, &v->slots[i], 68, &v->pastVertices);
    return true;
}

/* Before poses move them: the vertices of what bends, as the frame before drew them. */
static void pastVerticesCopy(Vulkan* v, VkCommandBuffer c, const ArtfScene* scene) {
    v->pastCopies.len = 0;
    for (size_t i = 0; i < scene->deforming.len; ++i) {
        const uint32_t index = scene->deforming.items[i];
        const MeshRange* range = &v->meshes.items[index];
        const VkDeviceSize at = (VkDeviceSize)range->vertexOffset * sizeof(ArtfPackedVertex);
        const BufferCopy copy = {at, at, (VkDeviceSize)scene->meshes.items[index].vertexCount * sizeof(ArtfPackedVertex)};
        if (copy.size && at + copy.size <= v->pastVertices.size) artfVecPush(&v->pastCopies, copy);
    }
    if (!v->pastCopies.len) return;
    /* (the frame before read both; the bones write the vertices after this) */
    memoryBarrier(v, c, STAGE_VERTEX_INPUT | STAGE_VERTEX_SHADER, STAGE_TRANSFER, ACCESS_VERTEX_READ | ACCESS_SHADER_READ, ACCESS_TRANSFER_READ | ACCESS_TRANSFER_WRITE);
    v->vkCmdCopyBuffer(c, v->vertices.buffer, v->pastVertices.buffer, (uint32_t)v->pastCopies.len, v->pastCopies.items);
    memoryBarrier(v, c, STAGE_TRANSFER, STAGE_COMPUTE | STAGE_TRANSFER | STAGE_VERTEX_SHADER, ACCESS_TRANSFER_WRITE, ACCESS_SHADER_READ | ACCESS_SHADER_WRITE | ACCESS_TRANSFER_WRITE);
}

/* ---- what comes after a frame in floats: post.c's order ---- */

/* The end of a pass, on the GPU's clock: what it took is from the mark before. */
static void passMark(Vulkan* v, VkCommandBuffer c, Slot* s, const char* name) {
    if (!s->timed || s->passCount >= ARTF_PASS_TIMES) return;
    const uint32_t slot = (uint32_t)(s - v->slots);
    v->vkCmdWriteTimestamp(c, STAGE_BOTTOM, v->timestamps, slot * QUERIES + 1 + s->passCount);
    s->passNames[s->passCount++] = name;
}

static void postStepWith(Vulkan* v, VkCommandBuffer c, VkPipeline pipeline, uint32_t push, uint32_t count) {
    v->vkCmdBindPipeline(c, 1, pipeline);
    v->vkCmdPushConstants(c, v->pipelineLayout, SHADER_COMPUTE, 0, 4, &push);
    v->vkCmdDispatch(c, (count + 63) / 64, 1, 1);
    memoryBarrier(v, c, STAGE_COMPUTE, STAGE_COMPUTE | STAGE_TRANSFER, ACCESS_SHADER_WRITE, ACCESS_SHADER_READ | ACCESS_SHADER_WRITE | ACCESS_TRANSFER_READ);
}

static void postStep(Vulkan* v, VkCommandBuffer c, int pipeline, uint32_t push, uint32_t count) { postStepWith(v, c, v->postPipelines[pipeline], push, count); }

/* Set 1 as the parts are given now, for compute (1) or drawing (0). */
static void rolesBind(Vulkan* v, VkCommandBuffer c, int point) {
    v->vkCmdBindDescriptorSets(c, point, v->pipelineLayout, 1, 1, &v->roleSets[v->role[0] * 9 + v->role[1] * 3 + v->role[2]], 0, NULL);
}

/* What a pass wrote into the second light is the light from now on: it is that buffer's part, and the second goes to
 * the buffer that is neither, nor the history. With keep, it is the history too, for the frame after (temporal
 * antialiasing, upscaling). No copy: the passes before only wrote, and the barrier after each is enough. */
static void postSwap(Vulkan* v, VkCommandBuffer c, bool keep) {
    const uint8_t light = v->role[1], was = v->role[0];
    if (keep) v->role[2] = light;
    v->role[0] = light;
    v->role[1] = was != v->role[2] && was != light ? was : (uint8_t)(3 - light - v->role[2]);
    rolesBind(v, c, 1);
}

enum { POST_GATHER, POST_OCCLUSION, POST_OCCLUSION_BLUR, POST_REFLECTION, POST_FROXELS, POST_TEMPORAL, POST_FOCUS, POST_BLOOM_DOWN, POST_BLOOM_UP, POST_REFLECTION_APPLY, POST_METER,
       POST_MOTION_BLUR, POST_VELOCITY, POST_TILE_MOST, POST_UPSCALE, POST_SHARPEN };

static void postRecord(Vulkan* v, VkCommandBuffer c, Slot* s, const ArtfFrameData* frame, uint32_t image) {
    /* (the scene's size, until it is upscaled; then the frame's) */
    const uint32_t pixels = (uint32_t)v->sceneWidth * (uint32_t)v->sceneHeight, framePixels = (uint32_t)v->width * (uint32_t)v->height;
    const bool scaled = pixels != framePixels;
    const Rect2D area = {{0, 0}, {(uint32_t)v->width, (uint32_t)v->height}};
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
    /* (the light and the second are the two buffers that do not hold the history) */
    v->role[0] = v->role[2] ? 0 : 1;
    v->role[1] = (uint8_t)(3 - v->role[0] - v->role[2]);
    rolesBind(v, c, 1);
    postStep(v, c, POST_GATHER, 0, pixels);
    passMark(v, c, s, "gather");
    if (frame->post[0][2] > 0) {
        postStep(v, c, POST_OCCLUSION, 0, pixels);
        postStep(v, c, POST_OCCLUSION_BLUR, 0, pixels);
        passMark(v, c, s, "ambient occlusion");
    }
    if (frame->post[1][0] > 0) {
        /* (from rays, or marched across the frame) */
        const bool traced = frame->traced & ARTF_TRACED_REFLECTIONS && v->reflectionTraced;
        postStepWith(v, c, traced ? v->reflectionTraced : v->postPipelines[POST_REFLECTION], 0, v->postBloomSizes[0][1] * v->postBloomSizes[0][2]);
        postStep(v, c, POST_REFLECTION_APPLY, 0, pixels);
        passMark(v, c, s, "reflections");
    }
    const bool temporal = frame->post[1][3] > 0;
    if (scaled) {
        /* (from here on, the frame's size) */
        postStep(v, c, POST_UPSCALE, temporal && v->historyValid ? 1u : 0u, framePixels);
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
    memoryBarrier(v, c, STAGE_COMPUTE | STAGE_TRANSFER, STAGE_FRAGMENT, ACCESS_SHADER_WRITE | ACCESS_TRANSFER_WRITE, ACCESS_SHADER_READ);
    RenderPassBeginInfo present = {ST_RENDER_PASS_BEGIN, NULL, v->presentPass, v->presentFramebuffers[image], area, 0, NULL};
    v->vkCmdBeginRenderPass(c, &present, 0);
    v->vkCmdSetViewport(c, 0, 1, viewport);
    v->vkCmdSetScissor(c, 0, 1, &area);
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

/* The meshlets view's table (binding 65): of each mesh, where its triangles start in it; then of each triangle, its
 * meshlet, numbered as the CPU numbers them. A triangle is known by PrimitiveId, which counts from its draw's first:
 * so the meshlets view draws whole meshes. Made when the view first asks, and again when meshes come or go. */
static bool meshletIdsUp(Vulkan* v, const ArtfScene* scene) {
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
        for (uint32_t k = 0; k < m->meshletCount; ++k)
            for (uint32_t t = 0; t < m->meshlets[k].triangleCount; ++t) out[at + m->meshlets[k].triangleOffset + t] = k;
        at += m->indexCount / 3;
    }
    v->meshletIdsKey = key;
    return true;
}

static bool vkDraw(ArtfRenderer* r, ArtfScene* scene) {
    Vulkan* v = (Vulkan*)r;
    const double start = artfNowMs();
    if (v->lost) return false;
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
    const bool tracedShadows = !tracing && scene->shadows.traced, tracedHardware = tracedShadows && v->rayQuery;
    const bool tracedReflections = !tracing && v->floats && scene->post.screen.tracedReflections && scene->post.screen.reflections > 0;
    const bool shapes = artfSceneShapedLights(scene);
    /* (what was being made, if it is done: see shaderJobStart) */
    if (v->shaderJob && (!v->shaderTask || artfTaskDone(v->shaderTask))) shaderJobEnd(v, scene, true);
    const Features want = {probes, walls, fogGrid, coats, softShadows, shapes, viewShaders, meshletShaders, tracedShadows, tracedHardware};
    const Features have = featuresOf(v);
    if (memcmp(&want, &have, sizeof want)) {
        if (artfVulkanShadersWait || !v->frameNumber || tracedShadows != v->tracedShadows || tracedHardware != v->tracedHardware) {
            shaderJobEnd(v, scene, false);
            featuresSet(v, &want);
            if (tracedHardware && !have.tracedHardware) artfLog("artful: shadows from rays go through the ray tracing hardware");
            if (!scenePipelineMake(v, scene->graphs.items, (uint32_t)scene->graphs.len) || !cutoutPipelinesMake(v, scene->graphs.items, (uint32_t)scene->graphs.len)) return false;
            v->graphCount = (uint32_t)scene->graphs.len;
        } else if (!v->shaderJob) {
            shaderJobStart(v, scene, &want);
        }
    }
    /* new materials of the program's own: new shaders */
    if (v->graphCount != scene->graphs.len || (v->graphScene != scene->id && (v->graphCount || scene->graphs.len))) {
        shaderJobEnd(v, scene, false);
        if (!cutoutPipelinesMake(v, scene->graphs.items, (uint32_t)scene->graphs.len)) return false;
        if (v->tracedShadows && !scenePipelineMake(v, scene->graphs.items, (uint32_t)scene->graphs.len)) return false;
        pipelineRetire(v, v->tracePipeline);
        v->tracePipeline = 0;
        pipelineRetire(v, v->reflectionTraced);
        v->reflectionTraced = 0;
        v->graphCount = (uint32_t)scene->graphs.len;
    }
    v->graphScene = scene->id;
    /* the surface's second word only where motion is followed: its passes and pipelines made again when that changes */
    const bool wide = v->floats && (scene->post.screen.temporal || scene->post.lens.motionBlur > 0);
    if (wide != v->surfaceWide) {
        v->vkDeviceWaitIdle(v->device);
        v->surfaceWide = wide;
        if (!scenePassesMake(v, scene) || !targetsRemake(v)) return false;
        if (!v->framebuffers[0]) return true;
    }
    if (wide && scene->deforming.len && !pastVerticesRoom(v)) return false;
    if (tracing && !syncTrace(v, scene)) return false;
    if ((tracedShadows || tracedReflections) && !syncTraced(v, scene, tracedReflections)) return false;
    /* (what rays read of the instances: the slot's, once its fence is waited for) */
    const bool topRays = tracing || tracedShadows || tracedReflections;
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
            v->lost = true;
            return false;
        }
    }
    v->vkResetFences(v->device, 1, &s->fence);

    ArtfFrameData frame;
    artfSceneFrame(scene, v->sceneWidth, v->sceneHeight, &frame);
    artfFrameForView(&frame, (int)view);
    frame.output[0] = (float)v->width;
    frame.output[1] = (float)v->height;
    frame.output[3] = (float)v->postFrames;
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
    memcpy(s->frame.mapped, &frame, sizeof frame);
    /* the frame's lights' profiles, past those past them in their buffer (shaders.c's profileAt) */
    for (uint32_t k = 0; k < frame.lightCount; ++k)
        if (frame.lightShapes[k][2][3] > 0)
            memcpy((char*)s->extraLights.mapped + ARTF_MAX_LIGHTS * sizeof(ArtfLightData) + (size_t)k * ARTF_PROFILE_FLOATS * sizeof(float), scene->lightProfile[(int)frame.lightShapes[k][2][3] - 1], ARTF_PROFILE_FLOATS * sizeof(float));
    if (!syncLights(v, s, scene, &frame) || !syncIndirect(v, s, scene, &frame)) return false;
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
    const bool sorted = layerCount && v->fragmentStores && !v->floats && (v->samples == 1 || v->sampleShading) && scene->features[ARTF_FEATURE_LAYERS_SORTED] && !overdraw;
    if (sorted && !layerListsMake(v, scene)) return false;
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
        memoryBarrier(v, c, STAGE_FRAGMENT, STAGE_COMPUTE, ACCESS_SHADER_READ, ACCESS_SHADER_WRITE);
        v->vkCmdBindDescriptorSets(c, 1, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
        const uint32_t cells = (uint32_t)((v->sceneWidth + ARTF_FROXEL_TILE - 1) / ARTF_FROXEL_TILE) * (uint32_t)((v->sceneHeight + ARTF_FROXEL_TILE - 1) / ARTF_FROXEL_TILE);
        postStep(v, c, POST_FROXELS, 0, cells);
        memoryBarrier(v, c, STAGE_COMPUTE, STAGE_FRAGMENT, ACCESS_SHADER_WRITE, ACCESS_SHADER_READ);
        passMark(v, c, s, "fog grid");
    }
    if (tracing) {
        /* (the frame before read and wrote what this one is about to) */
        const VkFlags readWrite = ACCESS_SHADER_READ | ACCESS_SHADER_WRITE;
        if (traceClear) {
            memoryBarrier(v, c, STAGE_COMPUTE | STAGE_FRAGMENT, STAGE_TRANSFER, readWrite, ACCESS_TRANSFER_WRITE);
            v->vkCmdFillBuffer(c, v->traceSum.buffer, 0, ~(VkDeviceSize)0, 0);
            memoryBarrier(v, c, STAGE_TRANSFER, STAGE_COMPUTE | STAGE_FRAGMENT, ACCESS_TRANSFER_WRITE, readWrite);
        }
        if (traceAdding) {
            memoryBarrier(v, c, STAGE_COMPUTE | STAGE_FRAGMENT, STAGE_COMPUTE, readWrite, readWrite);
            v->vkCmdBindPipeline(c, 1, v->tracePipeline);
            v->vkCmdBindDescriptorSets(c, 1, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
            v->vkCmdDispatch(c, ((uint32_t)v->sceneWidth + 7) / 8, ((uint32_t)v->sceneHeight + 7) / 8, 1);
            if (scene->trace.desc.denoise) denoise(v, c, s);
            memoryBarrier(v, c, STAGE_COMPUTE, STAGE_FRAGMENT, ACCESS_SHADER_WRITE, ACCESS_SHADER_READ);
            passMark(v, c, s, "trace");
        }
    }

    ClearValue clears[3];
    memset(clears, 0, sizeof clears);
    memcpy(clears[0].color, frame.background, sizeof clears[0].color);
    if (v->floats) {
        /* (the background as light: what shows, squared, as on the CPU) */
        const float alpha = frame.background[3], over = alpha > 0 ? 1.f / alpha : 0.f;
        for (int k = 0; k < 3; ++k) clears[0].color[k] = frame.background[k] * frame.background[k] * over;
    }
    const Rect2D area = {{0, 0}, {(uint32_t)v->sceneWidth, (uint32_t)v->sceneHeight}};
    /* (the height negative: y goes up in clip space, as on the CPU) */
    const Viewport viewport = {0, (float)v->sceneHeight, (float)v->sceneWidth, -(float)v->sceneHeight, 0, 1};
    if (sorted) {
        /* (the frame before read them) */
        memoryBarrier(v, c, STAGE_FRAGMENT, STAGE_TRANSFER, ACCESS_SHADER_READ | ACCESS_SHADER_WRITE, ACCESS_TRANSFER_WRITE);
        v->vkCmdFillBuffer(c, v->layerCounts.buffer, 0, ~(VkDeviceSize)0, 0);
        memoryBarrier(v, c, STAGE_TRANSFER, STAGE_FRAGMENT, ACCESS_TRANSFER_WRITE, ACCESS_SHADER_READ | ACCESS_SHADER_WRITE);
    }
    RenderPassBeginInfo pass = {ST_RENDER_PASS_BEGIN, NULL, occlusion || sorted || v->floats ? v->earlyPass : v->renderPass, v->framebuffers[image], area, v->floats ? 3 : 2, clears};
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
        for (uint32_t level = 0; level < v->hizLevels; ++level) {
            if (level == 1) v->vkCmdBindPipeline(c, 1, v->hizNextPipeline);
            v->vkCmdPushConstants(c, v->pipelineLayout, SHADER_COMPUTE, 0, 4, &level);
            v->vkCmdDispatch(c, (v->hizTable[level][1] * v->hizTable[level][2] + 63) / 64, 1, 1);
            memoryBarrier(v, c, STAGE_COMPUTE, STAGE_COMPUTE, ACCESS_SHADER_WRITE, ACCESS_SHADER_READ | ACCESS_SHADER_WRITE);
        }
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
        postStep(v, c, POST_GATHER, 1, (uint32_t)v->sceneWidth * (uint32_t)v->sceneHeight);
        passMark(v, c, s, "depth for what follows");
        pass.renderPass = v->latePass;
        v->vkCmdBeginRenderPass(c, &pass, 0);
        v->vkCmdSetViewport(c, 0, 1, &viewport);
        v->vkCmdSetScissor(c, 0, 1, &area);
    }
    if (sorted) {
        v->vkCmdBindPipeline(c, 0, v->layerListPipeline);
        v->vkCmdBindDescriptorSets(c, 0, v->pipelineLayout, 0, 1, &s->set, 0, NULL);
        if (v->multiDraw) {
            v->vkCmdDrawIndexedIndirect(c, s->layerCommands.buffer, 0, layerCount, 20);
        } else {
            for (uint32_t i = 0; i < layerCount; ++i) v->vkCmdDrawIndexedIndirect(c, s->layerCommands.buffer, (VkDeviceSize)i * 20, 1, 20);
        }
        v->vkCmdEndRenderPass(c);
        memoryBarrier(v, c, STAGE_FRAGMENT | STAGE_EARLY_TESTS | STAGE_LATE_TESTS, STAGE_FRAGMENT, ACCESS_SHADER_WRITE | ACCESS_DEPTH_WRITE, ACCESS_SHADER_READ | ACCESS_SHADER_WRITE);
        pass.renderPass = v->samples > 1 ? v->resolvePass : v->latePass;
        if (v->samples > 1) pass.framebuffer = v->resolveFramebuffer;
        v->vkCmdBeginRenderPass(c, &pass, 0);
        v->vkCmdSetViewport(c, 0, 1, &viewport);
        v->vkCmdSetScissor(c, 0, 1, &area);
        v->vkCmdBindPipeline(c, 0, v->layerResolvePipeline);
        v->vkCmdDraw(c, 3, 1, 0, 0);
        if (v->samples > 1) {
            /* (depth was a texture there: what is left of the frame goes on as it does) */
            v->vkCmdEndRenderPass(c);
            pass.renderPass = v->latePass;
            pass.framebuffer = v->framebuffers[image];
            v->vkCmdBeginRenderPass(c, &pass, 0);
            v->vkCmdSetViewport(c, 0, 1, &viewport);
            v->vkCmdSetScissor(c, 0, 1, &area);
        }
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
    result = artfVulkanSubmitsLeft == 0 ? -4 /* VK_ERROR_DEVICE_LOST */ : v->vkQueueSubmit(v->queue, 1, &submit, s->fence);
    if (artfVulkanSubmitsLeft > 0) --artfVulkanSubmitsLeft;
    if (result != VK_OK) {
        artfLog("artful: submitting a frame failed: %d", (int)result);
        v->lost = true;
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
            v->lost = true;
            return false;
        }
    }
    v->cpuMs = artfNowMs() - start;
    return true;
}

static bool vkReadPixels(ArtfRenderer* r, uint32_t* dst) {
    Vulkan* v = (Vulkan*)r;
    if (v->lost || v->swapchain || !v->frameNumber) return false; /* (what was presented is the window system's) */
    if (v->vkDeviceWaitIdle(v->device) != VK_OK) return false;
    const size_t bytes = (size_t)v->width * v->height * 4;
    if (v->readback.size < bytes) {
        bufferFree(v, &v->readback);
        if (!bufferNewFor(v, &v->readback, bytes, BUFFER_TRANSFER_DST, BUFFER_READ_BY_CPU)) {
            bufferFree(v, &v->readback);
            return false;
        }
    }
    VkCommandBuffer c = oneShotBegin(v);
    const BufferImageCopy region = {0, 0, 0, {ASPECT_COLOR, 0, 0, 1}, {0, 0, 0}, {(uint32_t)v->width, (uint32_t)v->height, 1}};
    v->vkCmdCopyImageToBuffer(c, v->offscreen.image, LAYOUT_TRANSFER_SRC, v->readback.buffer, 1, &region);
    memoryBarrier(v, c, STAGE_TRANSFER, STAGE_HOST, ACCESS_TRANSFER_WRITE, ACCESS_HOST_READ);
    bool ok = oneShotEnd(v, c);
    if (ok) memcpy(dst, v->readback.mapped, bytes);
    return ok;
}

static void vkResize(ArtfRenderer* r, int width, int height) {
    Vulkan* v = (Vulkan*)r;
    if (width == v->width && height == v->height) return;
    v->width = width;
    v->height = height;
    v->targetsStale = true;
}

static uint32_t* vkPixels(ArtfRenderer* r) { (void)r; return NULL; }
static const char* vkDescription(const ArtfRenderer* r) { return ((const Vulkan*)r)->description; }
static void vkStats(const ArtfRenderer* r, ArtfFrameStats* out) {
    const Vulkan* v = (const Vulkan*)r;
    out->cpuMs = v->cpuMs;
    out->traceSamples = v->traceDone;
    out->gpuMs = v->gpuMs;
    out->shadowMapsDrawn = v->shadowMapsDrawn;
    out->passCount = v->passTimeCount;
    memcpy(out->passes, v->passTimes, v->passTimeCount * sizeof *out->passes);
}

/* ---- start and end ---- */

void artfVulkanWarm(ArtfRenderer* r) {
    Vulkan* v = (Vulkan*)r;
    /* (the next draw takes what was made: shaderJobEnd) */
    artfTaskJoin(v->shaderTask);
    v->shaderTask = NULL;
}

static void vkFree(ArtfRenderer* r) {
    Vulkan* v = (Vulkan*)r;
    if (v->device) {
        shaderJobEnd(v, NULL, false);
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
        for (int i = 0; i < 3; ++i)
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
        for (int i = 0; i < 16; ++i)
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
        if (v->traceShowPipeline) v->vkDestroyPipeline(v->device, v->traceShowPipeline, NULL);
        if (v->cutoutPipeline) v->vkDestroyPipeline(v->device, v->cutoutPipeline, NULL);
        if (v->layerListPipeline) v->vkDestroyPipeline(v->device, v->layerListPipeline, NULL);
        if (v->layerResolvePipeline) v->vkDestroyPipeline(v->device, v->layerResolvePipeline, NULL);
        bufferFree(v, &v->layerCounts);
        bufferFree(v, &v->layerFragments);
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
    artfVecFree(&v->copyStarts);
    artfVecFree(&v->layers);
    artfVecFree(&v->posedCopies);
    artfVecFree(&v->pastCopies);
    artfVecFree(&v->overlayDamage);
    artfFree(v);
}

static const ArtfRendererVtbl vkTable = {vkDescription, vkResize, vkSetOverlay, vkDraw, vkReadPixels, vkPixels, vkStats, vkFree};

static bool loadLibrary(Vulkan* v) {
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

static bool createSurface(Vulkan* v) {
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

static bool createInstance(Vulkan* v) {
    *(void**)&v->vkCreateInstance = v->getInstanceProcAddr(NULL, "vkCreateInstance");
    if (!v->vkCreateInstance) return false;
    const char* extensions[3];
    uint32_t count = 0;
    VkFlags flags = 0;
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
static bool chooseDevice(Vulkan* v, PhysicalDeviceProperties* chosen) {
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
        bool rayQuery = false;
        {
            ExtensionProperties* have = artfMalloc(512 * sizeof *have);
            uint32_t extensionCount = 512;
            int found = 0;
            if (v->vkEnumerateDeviceExtensionProperties && v->vkEnumerateDeviceExtensionProperties(gpus[i], NULL, &extensionCount, have) >= 0)
                for (uint32_t e = 0; e < extensionCount; ++e)
                    found |= (!strcmp(have[e].name, "VK_KHR_acceleration_structure") ? 1 : 0) | (!strcmp(have[e].name, "VK_KHR_ray_query") ? 2 : 0) | (!strcmp(have[e].name, "VK_KHR_deferred_host_operations") ? 4 : 0);
            artfFree(have);
            if (found == 7 && !getenv("ARTFUL_NO_RAY_QUERY")) {
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
static bool chooseFormat(Vulkan* v) {
    v->colorFormat = FORMAT_BGRA8;
    v->colorSpace = 0;
    if (!v->surface) return true;
    SurfaceFormat formats[64];
    uint32_t count = 64;
    if (v->vkGetPhysicalDeviceSurfaceFormatsKHR(v->gpu, v->surface, &count, formats) < 0) return false;
    int found = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (formats[i].format == FORMAT_BGRA8) { v->colorSpace = formats[i].colorSpace; return true; }
        if (formats[i].format == FORMAT_RGBA8 && !found) { found = 1; v->colorSpace = formats[i].colorSpace; }
    }
    if (found) v->colorFormat = FORMAT_RGBA8;
    return found != 0;
}

static bool createDevice(Vulkan* v) {
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
    features.features.textureCompressionETC2 = v->takesEtc;
    features.features.textureCompressionASTC_LDR = v->takesAstc;
    features.features.textureCompressionBC = v->takesBc;
    features.features.drawIndirectFirstInstance = 1;
    features.features.fragmentStoresAndAtomics = v->fragmentStores;
    features.features.sampleRateShading = v->sampleShading;
    features.features.independentBlend = v->independentBlend;
    features.features.geometryShader = v->primitiveIds;
    const char* extensions[5];
    uint32_t extensionCount = 0;
    if (v->surface) extensions[extensionCount++] = "VK_KHR_swapchain";
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
    Vulkan* v = ARTF_NEW(Vulkan);
    v->base.vt = &vkTable;
    v->base.backend = ARTF_BACKEND_VULKAN;
    v->native = o->surface;
    v->width = o->width;
    v->height = o->height;
    v->noVsync = o->noVsync;
    v->samples = o->antialias ? 4 : 1;
    v->floats = o->hdr;
    v->renderScale = o->hdr && o->renderScale > 0 && o->renderScale < 1.f ? artfMaxf(o->renderScale, .25f) : 1.f;
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
        if (!bufferNew(v, &s->extraLights, (VkDeviceSize)ARTF_MAX_LIGHTS * sizeof(ArtfLightData) + ARTF_FRAME_LIGHTS * ARTF_PROFILE_FLOATS * sizeof(float), BUFFER_STORAGE, true)) goto failed;
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
