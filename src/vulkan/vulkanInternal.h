/* artful — the Vulkan backend's own: its types, and what its parts (vulkan.c, vulkanTrace.c, vulkanPost.c) call
 * of each other. Not for the rest of the library. */
#ifndef ARTFUL_VULKAN_INTERNAL_H
#define ARTFUL_VULKAN_INTERNAL_H

#include "render/rendererInternal.h"
#include "scene/sceneInternal.h"
#include "core/threads.h"
#include "shaderir/ir.h"
#include "vulkan/frameGraph.h"
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
    FORMAT_RGBA8 = 37, FORMAT_BGRA8 = 44, FORMAT_A2B10G10R10 = 64, FORMAT_A2R10G10B10 = 58, FORMAT_RGBA16_FLOAT = 97, FORMAT_R32_UINT = 98, FORMAT_R32G32_UINT = 101, FORMAT_D32 = 126, FORMAT_RGBA16_UNORM = 91, FORMAT_RG16_SNORM = 78, FORMAT_RG16_FLOAT = 83,
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

/* What the scene's passes share, whose barriers come from what each pass says it does to them (trackUse): fog's grid,
 * the depth pyramid, tracing's sums, the layers' lists, the depth the scene draws, and what the gather writes for the
 * post passes (the depth and surfaces copied, the parts of light). */
enum { TRACK_FOG, TRACK_HIZ, TRACK_TRACE, TRACK_LAYERS, TRACK_DEPTH, TRACK_POST, TRACK_COUNT };
#define TRACK(x) (1u << (x))
#define QUERIES (ARTF_PASS_TIMES + 1) /* timestamps of a slot: where its frame starts, and where each pass ends */
#define MAX_TEXTURES 1024  /* in the one descriptor array, where the device allows; what is beyond draws white */
#define MAX_SWAPCHAIN_IMAGES 8
#define DENOISE_PIPELINES (4 + ARTF_DENOISE_PASSES) /* the filter's: light, time, variance, its passes in space, the end */

typedef struct GpuBuffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size;
    VkDeviceSize held; /* bytes of its memory (size, rounded as the driver wants) */
    void* mapped;
    uint64_t serial; /* which allocation this is: a handle can come back the same for another (see Slot's bound) */
    bool borrowed;   /* its memory is another's (an image's, bufferOver): freed with that, not with this */
} GpuBuffer;

typedef struct GpuImage {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDeviceSize held; /* bytes of its memory */
    int type;          /* of which memory type */
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
    GpuBuffer probes;                  /* light that bounced, gathered on the CPU (artfVulkanProbesOnCpu) */
    /* the GPU's probes on their way back to the scene (PROBES_KEPT_EVERY): copied in this slot's frame, read when its
     * fence says it is done, if the scene's probes are still those (its id, their serial); and how many steps they had */
    GpuBuffer probesBack;
    bool probesBackHeld;
    uint64_t probesBackScene;
    uint32_t probesBackSerial, probesBackFrames;
    VkBuffer probesBound, probesNextBound; /* what its set says bindings 62 and 70 are: its probes, or the GPU's */
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
    uint64_t meshletBoundTable; /* what its set says bindings 40 and 41 are (serials: see GpuBuffer) */
    bool meshletBound;
    uint32_t shadowCasterCount, shadowViews;
    uint64_t motionVersion;
    uint64_t motionStep;  /* (what of ArtfMotion this slot holds, after its instances) */
    size_t motionCount;
    uint64_t skinBoundData, skinBoundVertices; /* what its set says binding 34 and 36 are (serials) */
    uint64_t shadowInstances, shadowStructure, shadowMaterials, shadowScene;
    uint32_t shadowMeshes;
    uint64_t instanceVersion, structureVersion, materialVersion, lodVersion, streamStamp, streamScene;
    bool fresh; /* nothing written yet */
} Slot;

typedef struct SkinPlace { const ArtfSkinData* skin; uint32_t row; } SkinPlace;

/* refused: it did not fit in the GPU's memory, and is not drawn (ARTF_EVENT_REFUSED); tried again when a mesh goes. */
/* indexCount: what a whole draw takes (a graph's first level); indexLength: what was taken of the buffer */
typedef struct MeshRange { uint32_t firstIndex; int32_t vertexOffset; uint32_t indexCount, colorBase; uint64_t deformVersion; uint32_t vertexLength, colorLength, generation, refused, indexLength; } MeshRange;
#define MESHLET_ROWS 6 /* of the meshlet table, each: its ball, cone, range; a graph's own ball and its parent's; errors */

/* Stretches of a buffer that were given back, by where they start. */
typedef struct Span { uint32_t at, length; } Span;
typedef ARTF_VEC(Span) SpanList;

/* Room for length, in what was given back or after end; UINT32_MAX: none. */
static inline uint32_t spanTake(SpanList* list, uint32_t length, uint32_t* end, uint32_t capacity) {
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

static inline void spanGive(SpanList* list, uint32_t at, uint32_t length) {
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

/* What a compute shader that reads the scene's materials was made for (the tracing shader, reflections from rays):
 * the filter's guides, the ray tracing hardware, materials past glTF's core and lights' shapes, and the program's own
 * materials, the first graphCount of scene's. */
enum { MAKE_TRACE, MAKE_REFLECTIONS, MAKE_COMPUTE };
typedef struct ComputeKey {
    bool guides, hardware, more;
    uint32_t graphCount;
    uint64_t scene;
} ComputeKey;

enum { POST_GATHER, POST_OCCLUSION, POST_OCCLUSION_BLUR, POST_REFLECTION, POST_FROXELS, POST_TEMPORAL, POST_FOCUS, POST_BLOOM_DOWN, POST_BLOOM_UP, POST_REFLECTION_APPLY, POST_METER,
       POST_MOTION_BLUR, POST_VELOCITY, POST_TILE_MOST, POST_UPSCALE, POST_SHARPEN, POST_SAMPLED, POST_SAMPLED_SPACE, POST_CLOUD_COLUMNS, POST_CLOUD_APPLY, POST_KINDS };

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
    /* ArtfRendererOptions.output: as asked (sRGB without floats), and as made (ARTF_DISPLAY_), with its numbers
     * (artfDisplayNumbers); whether the instance has VK_EXT_swapchain_colorspace */
    int displayWanted, display;
    float displayInverse, displayPeak;
    bool colorspaces;
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
    /* ArtfRendererOptions.preRotate; how the image the frame goes into is turned (0 to 3, a quarter clockwise each), and
     * its size: the frame's, or its sides the other way round at a quarter or three */
    bool preRotate;
    int turn, imageWidth, imageHeight;
    bool lost;         /* nothing more can be drawn: it can only be freed */
    bool deviceFault;  /* VK_EXT_device_fault is on: when the device is lost, the driver is asked why */
    bool shaderDebug;  /* VK_KHR_shader_non_semantic_info is on, and the shaders carry where in C each is built */
    uint64_t deviceBytes; /* of the device's memory, what Artful holds (ArtfFrameStats.gpuBytes) */
    uint64_t shadowKeys[ARTF_SHADOW_VIEWS]; /* what each map holds */
    uint64_t shadowSeen;                    /* the scene's changes these know of */
    /* layers by each pixel's depth */
    bool fragmentStores, sampleShading;
    GpuBuffer layerCounts, layerFragments;
    VkPipeline layerListPipeline, layerResolvePipeline, layerGridPipeline, layerPrefixPipeline; /* layers by transmittance: their sums, the sums over the frame, the grid, its columns */
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
    bool depthCoverage;   /* VK_EXT_post_depth_coverage: layers by transmittance go a pixel at a time, with samples (ARTFUL_NO_POST_DEPTH: as without) */
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
    uint64_t bufferSerial; /* the last GpuBuffer's serial */
    /* Pipelines being made on a thread of their own while frames go on with these (see pipelinesFollow): the scene's
     * pixel shaders and passes, and the compute shaders that read its materials (tracePipeline, reflectionTraced, and
     * what each was made for); warmed: artfRendererWarm was called, and the next frame makes what is left before it
     * draws. And pipelines no frame takes any more, destroyed once the frames that took them are done. */
    struct ShaderJob* shaderJob;
    struct ComputeJob* computeJobs[MAKE_COMPUTE];
    ComputeKey computeKeys[MAKE_COMPUTE];
    bool warmed;
    ARTF_VEC(struct Retired) retired;

    /* the scene, as the GPU has it */
    uint64_t sceneId;
    GpuBuffer vertices, indices, meshTable, colors;
    SpanList vertexSpans, indexSpans, colorSpans;
    uint64_t meshRemovals, textureVersion;
    ARTF_VEC(uint64_t) textureMarks; /* of each texture here: its generation and resident mipmap */
    ARTF_VEC(uint8_t) textureHeld;   /* and the first mipmap its image holds (TEXTURE_NONE: none, or white) */
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
    VkPipeline denoiseLight, denoiseTime, denoiseVariance, denoiseSpace[ARTF_DENOISE_PASSES], denoiseEnd;
    bool denoiseFirst;
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
    VkPipeline postPipelines[POST_KINDS];   /* gather, occlusion, its blur, reflections, froxels, temporal, focus, bloom down, up, reflections' use, meter, motion blur,
                                     * velocity, tiles' largest, upscaling, sharpening */
    ArtfMotion motion;              /* how each instance moved: after the instances in each slot's buffer */
    bool exposureValid;             /* auto exposure has a state to go on from */
    VkPipeline fogPipeline;         /* fog that light goes through, over what is behind the scene */
    GpuBuffer postLight, postDepth, postSurface, postScratch, postHistory, postOcclusion, postFog, postBloom;
    bool postAliased; /* postOcclusion is in floatColor's memory (postBuffersMake) */
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
    GpuBuffer postBlur;             /* motion blur's velocities (binding 69): unused since they share postOcclusion */
    GpuBuffer postLocks;            /* upscaling's locks (binding 71): two floats a pixel of the frame, the halves in turn */
    GpuBuffer postGathered;         /* lights sampled, gathered (binding 72): two vec4s a pixel of the scene, the halves in turn */
    /* clouds in the scene: the field (binding 73, ArtfScene.clouds, of that generation) and the grid's cells (74) */
    GpuBuffer cloudField, cloudGrid;
    uint32_t cloudGeneration, cloudColumns, cloudRows;
    bool cloudsOn, cloudFieldHeld;
    uint32_t gatheredHalf;
    bool gatheredValid;
    bool layerSumsFresh; /* layers by transmittance: their sums were just made, and are not 0 yet */
    uint32_t lockHalf;              /* which half this frame's upscaling writes */
    struct Slot* owed;              /* a frame begun whose fence was reset, until it is submitted (frameOwed) */
    bool memoryShort;               /* this frame, the GPU's memory ran out for something (ARTF_EVENT_OUT_OF_MEMORY) */
    bool texturesShort;             /* and a texture went up with less than was asked, or white (artfSceneResidencyPress) */
    uint32_t owedImage;
    uint32_t postNeeds;             /* what effects that are on need of the post buffers (postNeedsOf) */
    /* What each post pipeline reads and writes (its IR, from its words: postAccessOf), as bits: the three parts of set 1
     * (0 to 2), then set 0's post buffers; and, while postRecord records, what the dispatches since the last barrier read
     * and wrote, by the buffers the parts are then: a barrier goes only where a dispatch would race one before it. */
    uint32_t postReads[POST_KINDS], postWrites[POST_KINDS];
    uint32_t pendingReads, pendingWrites;
    bool postTracking;
    uint32_t postBarriers, postDispatches; /* (of the last frame: how many barriers its post dispatches took) */
    /* What the frame's passes before the post ones last did to each resource they share (trackUse): the stages that
     * wrote it and how, and those that read it since; from these, and what a pass is about to do, the barrier it
     * needs. A frame starts as if each was used as the frame before uses it (trackFrameStart). */
    VkFlags trackWriteStage[TRACK_COUNT], trackWriteAccess[TRACK_COUNT], trackReadStage[TRACK_COUNT];
    VkFlags trackVisible[TRACK_COUNT]; /* the stages the last write has been made visible to */
    uint32_t trackBarriers; /* (of the last frame: how many barriers the scene's passes took from it) */
    GpuBuffer postNone;             /* what binds where an effect is off: a buffer of nothing */
    ArtfPool* pool;                 /* workers for what the CPU does for the GPU: the probes, for tests */
    /* Light that bounced, gathered here (artfShaderProbes): what the probes hold and what a step makes of them, which
     * all frames share, as each step reads what the one before made (62, 70); its pipeline and what it was made for;
     * the steps since they started over, and whose probes they are (a scene, and its artfSceneSetIndirect) */
    GpuBuffer probesNow, probesNext;
    VkPipeline probePipeline;
    uint64_t probeKey, probeScene, probeGraphScene;
    uint32_t probeGraphs; /* the scene's materials in C the probes' shader was made for */
    uint32_t probeFrames, probeSerial;
    bool probeClear;
    ArtfLightData* lightScratch;    /* lights past the frame's own, and their lists, before they go up */
    uint32_t* listScratch;
    uint32_t lightsOmitted; /* ArtfFrameStats.lightsOmitted, of the last frame */
    size_t listScratchSize; /* (in numbers) */
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

typedef enum BufferUse { BUFFER_GPU, BUFFER_WRITTEN_BY_CPU, BUFFER_READ_BY_CPU } BufferUse;

enum { NEED_OCCLUSION = 1, NEED_BLUR = 2, NEED_FOG = 4, NEED_SECOND = 8, NEED_HISTORY = 16, NEED_GATHERED = 32 /* lights sampled: what they gave, gathered */ };

/* What the scene's pixel shaders are made with, beyond floats: what vkDraw finds the scene asks for. */
typedef struct Features { bool probes, walls, froxels, coats, softShadows, shapes, viewShaders, meshletShaders, tracedShadows, tracedHardware; } Features;

/* A pipeline no frame will take from now on: destroyed when the frames before this one are done (retiredFree), so
 * that making another needs no wait for the device. So too what the ray tracing hardware no longer takes: a bottom
 * level, a buffer (of bottom levels, scratch, corners). */
typedef struct Retired { VkPipeline pipeline; VkAccelerationStructureKHR structure; GpuBuffer buffer; uint64_t frame; } Retired;

typedef struct ShaderJob {
    Vulkan* v;
    ArtfTask* task; /* NULL: made at once, or artfVulkanWarm waited for it */
    Features features;
    uint32_t variant;
    ArtfGraphData* graphs; /* a copy: the program may change the scene's while this is made */
    uint32_t graphCount;
    uint64_t scene;
    bool wide;                      /* the surface's second word */
    bool passesNew;                 /* wide is not what the scene's passes have: new ones, and what is drawn in them */
    bool lists, resolve;            /* the layers' lists' pipeline (it was there), and their resolve (new passes) */
    VkRenderPass pass, resolvePass; /* those the pipelines are made in */
    VkRenderPass passes[5];
    VkPipeline plain, cutout[4], lister[3], behind[4];
    bool ok;
} ShaderJob;

/* The tracing shader (MAKE_TRACE: with the filter's passes, the first time its guides are asked for) or reflections
 * from rays (MAKE_REFLECTIONS), for key. */
typedef struct ComputeJob {
    Vulkan* v;
    ArtfTask* task; /* as ShaderJob's */
    int kind;
    ComputeKey key;
    ArtfGraphData* graphs;
    bool denoise;
    VkPipeline made, denoised[DENOISE_PIPELINES]; /* the filter's: light, time, variance, five in space, the end */
    bool ok;
} ComputeJob;

/* (below that many meshlets, about 65,000 triangles, what culling saves is less than it costs) */
enum { MESHLET_COMMANDS = 1 << 16, MESHLET_LEAST = 512, MESHLET_HEADER = 12, MESHLET_DRAWN = 8 };

enum {
    BUFFER_DEVICE_ADDRESS = 0x20000, BUFFER_AS_INPUT = 0x80000, BUFFER_AS_STORAGE = 0x100000, STAGE_AS_BUILD = 0x02000000, ACCESS_AS_READ = 0x00200000, ACCESS_AS_WRITE = 0x00400000
};

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

/* What one bottom level does this frame. */
typedef struct RtPlan { uint32_t mesh; bool update, fresh; VkDeviceSize size, scratch; } RtPlan;



/* ---- what each part calls of the others ---- */

void trackFrameStart(Vulkan* v);
void trackUse(Vulkan* v, VkCommandBuffer c, uint32_t readMask, uint32_t writeMask, VkFlags stage, VkFlags readAccess, VkFlags writeAccess);
void trackCovered(Vulkan* v, uint32_t readMask, uint32_t writeMask, VkFlags stage, VkFlags readAccess, VkFlags writeAccess);

int memoryType(const Vulkan* v, uint32_t bits, VkFlags wanted);
bool bufferNewFor(Vulkan* v, GpuBuffer* b, VkDeviceSize size, VkFlags usage, BufferUse use);
bool bufferNew(Vulkan* v, GpuBuffer* b, VkDeviceSize size, VkFlags usage, bool hostVisible);
void bufferFree(Vulkan* v, GpuBuffer* b);
bool imageNew(Vulkan* v, GpuImage* g, int format, int width, int height, uint32_t mips, int samples, VkFlags usage, VkFlags aspect);
bool bufferOver(Vulkan* v, GpuBuffer* b, VkDeviceSize size, VkFlags usage, const GpuImage* g);
void imageFree(Vulkan* v, GpuImage* g);
void imageBarrier(Vulkan* v, VkCommandBuffer c, VkImage image, uint32_t mips, int from, int to, VkFlags srcStage, VkFlags dstStage, VkFlags srcAccess, VkFlags dstAccess);
void memoryBarrier(Vulkan* v, VkCommandBuffer c, VkFlags srcStage, VkFlags dstStage, VkFlags srcAccess, VkFlags dstAccess);
VkCommandBuffer oneShotBegin(Vulkan* v);
bool oneShotEnd(Vulkan* v, VkCommandBuffer c);
void targetsFree(Vulkan* v);
void writeOverlayDescriptor(Vulkan* v);
bool hizMake(Vulkan* v);
void postAccessOf(const uint32_t* code, size_t size, uint32_t* reads, uint32_t* writes);
uint32_t postNeedsOf(const Vulkan* v, const ArtfFrameData* f, bool clouds);
bool postBuffersMake(Vulkan* v);
bool postTargetsMake(Vulkan* v);
bool syncLut(Vulkan* v, const ArtfScene* scene);
bool targetsMake(Vulkan* v);
bool surfaceChanged(Vulkan* v);
bool targetsRemake(Vulkan* v);
VkShaderModule shaderModule(Vulkan* v, uint32_t* code, size_t size);
PipelineColorBlend blendOf(const Vulkan* v, ColorBlendAttachment* pair, ColorBlendAttachment color, bool surface);
bool renderPassMake(const Vulkan* v, VkRenderPass* out, bool load, bool store, int colorFinal, bool readDepth, bool wide);
bool scenePassesCreate(const Vulkan* v, bool wide, int finalLayout, VkRenderPass* passes);
void pipelineCacheOpen(Vulkan* v);
void pipelineCacheSave(Vulkan* v);
bool shadowPassMake(Vulkan* v);
bool shadowAtlasMake(Vulkan* v, int width, int height);
Features featuresOf(const Vulkan* v);
void featuresSet(Vulkan* v, const Features* f);
uint32_t variantOf(bool floats, const Features* f);
uint32_t shaderVariant(const Vulkan* v);
void pipelineRetire(Vulkan* v, VkPipeline pipeline);
void structureRetire(Vulkan* v, VkAccelerationStructureKHR structure);
void bufferRetire(Vulkan* v, GpuBuffer* b);
void retiredFree(Vulkan* v, bool all);
uint32_t* sceneFragmentCode(const Vulkan* v, bool cutout, const ArtfGraphData* graphs, uint32_t count, uint32_t variant, size_t* size);
bool overdrawPipelinesMake(Vulkan* v);
bool scenePipelineCreate(Vulkan* v, VkShaderModule fragment, VkRenderPass pass, VkPipeline* out);
bool cutoutPipelinesCreate(Vulkan* v, VkShaderModule fragment, VkRenderPass pass, VkPipeline* made);
void cutoutPipelinesRetire(Vulkan* v);
bool behindPipelinesCreate(Vulkan* v, VkRenderPass pass, VkPipeline* made);
bool layerListPipelinesCreate(Vulkan* v, const ArtfGraphData* graphs, uint32_t count, uint32_t variant, VkRenderPass pass, VkRenderPass resolvePass, bool list, bool resolve, VkPipeline* made);
bool computePipelineCreate(Vulkan* v, uint32_t* code, size_t size, VkPipeline* out);
ArtfGraphData* graphsCopy(const ArtfScene* scene);
void graphsFree(ArtfGraphData* graphs, uint32_t count);
bool graphsPrefix(uint32_t count, uint64_t madeFor, const ArtfScene* scene);
void shaderJobRun(void* user);
void shaderJobStart(Vulkan* v, const ArtfScene* scene, const Features* want, bool wide, bool now);
bool shaderJobEnd(Vulkan* v, const ArtfScene* scene, bool use);
ComputeKey computeKeyOf(const Vulkan* v, const ArtfScene* scene, int kind);
bool computeFlagsSame(const ComputeKey* a, const ComputeKey* b);
void computeJobRun(void* user);
void computeJobStart(Vulkan* v, const ArtfScene* scene, int kind, const ComputeKey* key, bool now);
bool computeJobEnd(Vulkan* v, const ArtfScene* scene, int kind, bool use);
bool computeFollow(Vulkan* v, const ArtfScene* scene, int kind, bool wait, bool* usable);
bool layerListsMake(Vulkan* v, const ArtfScene* scene);
bool pipelinesMake(Vulkan* v);
void writeBufferDescriptors(Vulkan* v, Slot* s);
bool flagsMake(Vulkan* v);
bool upload(Vulkan* v, GpuBuffer* to, VkDeviceSize offset, const void* data, VkDeviceSize size);
bool takesFormat(const Vulkan* v, uint32_t format);
void textureWhite(Vulkan* v, uint32_t index);
bool uploadTexture(Vulkan* v, ArtfTextureData* t, uint32_t index, bool compress, ArtfTexture handle);
bool colorsReset(Vulkan* v);
uint32_t layersAt(const Vulkan* v);
uint32_t swarmCopiesAt(const Vulkan* v);
VkDeviceSize visibleSize(const Vulkan* v);
VkDeviceSize instanceBytes(const Vulkan* v);
bool meshletsUsed(const Vulkan* v, const ArtfMeshData* m, size_t i);
bool meshletsUp(Vulkan* v, const ArtfScene* scene);
bool syncScene(Vulkan* v, ArtfScene* scene);
void syncSlot(Vulkan* v, Slot* s, const ArtfScene* scene);
void backgroundPipelineMake(Vulkan* v, const ArtfScene* scene);
bool syncBackground(Vulkan* v, const ArtfScene* scene, uint32_t bloomPixels);
uint32_t backgroundRaysPerPixel(const ArtfScene* scene);
uint64_t backgroundRays(const Vulkan* v, const ArtfScene* scene);
bool traceBackgroundApart(Vulkan* v, Slot* s, const ArtfScene* scene);
void traceBackground(Vulkan* v, VkCommandBuffer c, Slot* s, const ArtfBackgroundBlock* h, bool raysDone);
void vkSetOverlay(ArtfRenderer* r, const ArtfOverlay* o);
bool uploadOverlay(Vulkan* v, Slot* s, VkCommandBuffer c);
void drawCommands(Vulkan* v, VkCommandBuffer c, Slot* s, VkPipeline pipeline, uint32_t first, uint32_t count);
bool bonesAlone(const ArtfMeshData* m);
bool syncSkinData(Vulkan* v, const ArtfScene* scene);
bool shadowCullMake(Vulkan* v);
bool meshletPipelineMake(Vulkan* v);
bool meshletFlagsUp(Vulkan* v, const ArtfScene* scene);
uint32_t meshletHalf(const Vulkan* v);
bool stageMeshlets(Vulkan* v, Slot* s, bool on);
bool skinPipelineMake(Vulkan* v);
bool stageBones(Vulkan* v, Slot* s, const ArtfScene* scene);
void runBones(Vulkan* v, VkCommandBuffer c, Slot* s);
bool stagePoses(Vulkan* v, Slot* s, ArtfScene* scene);
void bindStorage(Vulkan* v, Slot* s, uint32_t binding, const GpuBuffer* b);
bool syncShadowList(Vulkan* v, Slot* s, const ArtfScene* scene, uint32_t views);
void shadowDraw(Vulkan* v, VkCommandBuffer c, Slot* s, VkPipeline pipeline, uint32_t first, uint32_t count);
void drawShadows(Vulkan* v, VkCommandBuffer c, Slot* s, const ArtfShadowFrame* shadows, uint32_t views, uint64_t changed);
uint32_t swarmList(Vulkan* v, const ArtfScene* scene);
bool swarmPipelineMake(Vulkan* v);
bool syncSwarms(Vulkan* v, Slot* s, const ArtfScene* scene);
void runSwarms(Vulkan* v, VkCommandBuffer c, Slot* s);
void drawSwarms(Vulkan* v, VkCommandBuffer c, Slot* s, const ArtfScene* scene);
void drawLayers(Vulkan* v, VkCommandBuffer c, const ArtfScene* scene, uint32_t count);
void lightDescriptors(Vulkan* v, Slot* s);
void probeDescriptors(Vulkan* v, Slot* s, VkBuffer now, VkBuffer next);
bool syncIndirect(Vulkan* v, Slot* s, ArtfScene* scene, const ArtfFrameData* frame, bool* gather);
void gatherProbes(Vulkan* v, VkCommandBuffer c, Slot* s, const ArtfScene* scene);
bool syncLights(Vulkan* v, Slot* s, const ArtfScene* scene, ArtfFrameData* frame);
bool pastVerticesRoom(Vulkan* v);
void pastVerticesCopy(Vulkan* v, VkCommandBuffer c, const ArtfScene* scene);
bool meshletIdsUp(Vulkan* v, const ArtfScene* scene);
bool pipelinesTake(Vulkan* v, const ArtfScene* scene);
bool shadersFollow(Vulkan* v, const ArtfScene* scene, const Features* want, bool wide, bool wait);
void frameOwed(Vulkan* v);
bool vkDraw(ArtfRenderer* r, ArtfScene* scene);
bool vkDrawAs(Vulkan* v, ArtfScene* scene);
bool vkReadPixels(ArtfRenderer* r, uint32_t* dst);
void vkResize(ArtfRenderer* r, int width, int height);
uint32_t* vkPixels(ArtfRenderer* r);
const char* vkDescription(const ArtfRenderer* r);
void vkStats(const ArtfRenderer* r, ArtfFrameStats* out);
void vkFree(ArtfRenderer* r);
bool loadLibrary(Vulkan* v);
bool createSurface(Vulkan* v);
bool createInstance(Vulkan* v);
bool chooseDevice(Vulkan* v, PhysicalDeviceProperties* chosen);
bool chooseFormat(Vulkan* v);
void lose(Vulkan* v, VkResult result);
#define ARTF_VULKAN_LOSSES_MOST 3
extern uint32_t artfVulkanDevicesLost;
bool createDevice(Vulkan* v);
bool syncEnvironment(Vulkan* v, const ArtfScene* scene);
bool rtBufferNew(Vulkan* v, GpuBuffer* b, VkDeviceSize size, VkFlags usage, bool written);
VkDeviceAddress rtAddress(Vulkan* v, const GpuBuffer* b);
bool traceBuffer(Vulkan* v, GpuBuffer* b, uint32_t binding, const void* data, VkDeviceSize size, bool built);
void rtRelease(Vulkan* v, RtBottom* b);
void rtBottomsFree(Vulkan* v, bool now);
void rtFree(Vulkan* v);
bool rtRoom(Vulkan* v, RtBottom* b, VkDeviceSize size, VkDeviceSize block);
bool rtIndicesSync(Vulkan* v, const ArtfScene* scene);
AsGeometry rtTriangles(VkDeviceAddress vertices, VkDeviceAddress corners, const ArtfTraceRange* r, uint32_t vertexCount);
float rtExtent(const ArtfTrace* t, const ArtfTraceRange* r, uint32_t vertexCount);
bool traceLayoutSame(const Vulkan* v, const ArtfScene* scene);
bool traceMovedUp(Vulkan* v, Slot* s, const ArtfScene* scene);
bool slotMeshSync(Vulkan* v, Slot* s, VkCommandBuffer c, const ArtfScene* scene, bool hardware);
bool slotTraceBuffer(Vulkan* v, Slot* s, GpuBuffer* b, uint32_t binding, const void* data, VkDeviceSize size);
bool slotTopSync(Vulkan* v, Slot* s, VkCommandBuffer c, const ArtfScene* scene, bool hardware);
bool traceBuffersSync(Vulkan* v, const ArtfScene* scene);
bool syncTraced(Vulkan* v, ArtfScene* scene);
bool syncTrace(Vulkan* v, ArtfScene* scene);
void denoise(Vulkan* v, VkCommandBuffer c, Slot* s);
/* Clouds in the scene, before a frame is recorded: their field, as the scene's, and the grid, for the scene's size. */
bool cloudsSync(Vulkan* v, const ArtfScene* scene, const ArtfFrameData* frame);
void passMark(Vulkan* v, VkCommandBuffer c, Slot* s, const char* name);
void postBarrier(Vulkan* v, VkCommandBuffer c);
void postStepWith(Vulkan* v, VkCommandBuffer c, VkPipeline pipeline, int index, uint32_t push, uint32_t count);
void postStep(Vulkan* v, VkCommandBuffer c, int pipeline, uint32_t push, uint32_t count);
uint32_t postPhysical(const Vulkan* v, uint32_t mask);
void rolesBind(Vulkan* v, VkCommandBuffer c, int point);
void postSwap(Vulkan* v, VkCommandBuffer c, bool keep);
void postRecord(Vulkan* v, VkCommandBuffer c, Slot* s, const ArtfFrameData* frame, uint32_t image);

#endif
