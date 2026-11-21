/* artful-2d — the GPU, through Vulkan: compute shaders, and a swapchain of its own when there is a window. Vulkan is
 * opened at run time (no SDK), its shaders are the library's own (shaders2d.c, giShaders2d.inc).
 *
 * A frame: what the CPU made of the scene (its items, edges, tiles, lamps, shadow maps, outlines, and the textures that
 * changed) goes up through the slot's staging buffer; gi, the tiles, bloom's passes and the tone curve run; the frame
 * is copied into the window's image and shown, or back into the slot's readback buffer. Two slots: a draw waits only
 * for the frame before the last, so that the CPU makes one frame while the GPU draws the other. The buffers the shaders
 * read are one set: each frame's commands start by waiting for the frame before them on the GPU, not on the CPU.
 *
 * Textures keep their place on the GPU: one that changed is written where it was if it still fits, else at the end,
 * and the rest stay where they are. When more than half of the room is what textures left behind, or there is no room
 * left, they are all laid out again. */
#include "2d/vulkan2d.h"
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
    STAGE_TOP = 0x1, STAGE_TRANSFER = 0x1000, STAGE_COMPUTE = 0x800, STAGE_BOTTOM = 0x2000, STAGE_HOST = 0x4000,
    ACCESS_SHADER_READ = 0x20, ACCESS_SHADER_WRITE = 0x40, ACCESS_TRANSFER_READ = 0x800, ACCESS_TRANSFER_WRITE = 0x1000, ACCESS_HOST_READ = 0x2000,
    USAGE_TRANSFER_SRC = 1, USAGE_TRANSFER_DST = 2, USAGE_STORAGE = 0x20,
    MEMORY_DEVICE = 1, MEMORY_HOST = 2, MEMORY_COHERENT = 4, MEMORY_CACHED = 8,
    DESCRIPTOR_STORAGE_BUFFER = 7, SHADER_COMPUTE = 0x20, BIND_COMPUTE = 1,
    FORMAT_RGBA8 = 37, FORMAT_BGRA8 = 44, LAYOUT_UNDEFINED = 0, LAYOUT_TRANSFER_DST = 7, LAYOUT_PRESENT = 1000001002, ASPECT_COLOR = 1
};

enum {
    PIPE_TILES, PIPE_DOWN, PIPE_UP, PIPE_FINISH, PIPE_REFLECT,
    PIPE_GI_SCENE, PIPE_GI_ROWS, PIPE_GI_COLUMNS, PIPE_GI_EVEN, PIPE_GI_ODD, PIPE_GI_GATHER, PIPE_GI_SPREAD0, PIPE_GI_SPREAD1,
    PIPES
};

enum { SLOTS = 2, MAX_IMAGES = 8 };

typedef struct Buffer2D {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size;
    void* mapped;
} Buffer2D;

/* A frame on its way: its own commands, staging, readback and timestamps. */
typedef struct FrameSlot2D {
    VkCommandBuffer commands;
    VkFence fence;        /* made signaled */
    VkSemaphore acquired; /* the window's image is there */
    Buffer2D staging, readback;
    bool pending;         /* submitted, not yet waited for */
    bool readBack;        /* its frame was copied into readback */
    int width, height;
} FrameSlot2D;

/* Where a texture is in binding 6, and which of its versions is there. */
typedef struct TextureGpu {
    uint64_t version;
    uint32_t offset, capacity; /* in texels */
    bool placed;
} TextureGpu;

struct Gpu2D {
#ifdef _WIN32
    HMODULE library;
#else
    void* library;
#endif
    void* (VKAPI* getInstanceProcAddr)(VkInstance, const char*);
    VkResult(VKAPI* vkCreateInstance)(const InstanceCreateInfo*, const void*, VkInstance*);
#define X(name, result, args) result(VKAPI* name) args;
    ARTF_VK_INSTANCE_FUNCTIONS(X)
    ARTF_VK_DEVICE_FUNCTIONS(X)
#undef X
    void(VKAPI* vkCmdClearColorImage)(VkCommandBuffer, VkImage, int, const ClearValue*, uint32_t, const ImageSubresourceRange*);
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    PhysicalDeviceMemoryProperties memory;
    char name[256];
    VkCommandPool pool;
    VkDescriptorSetLayout setLayout;
    VkPipelineLayout layout;
    VkDescriptorPool descriptorPool;
    VkDescriptorSet set;
    VkPipeline pipelines[PIPES];
    Buffer2D buffers[B2_COUNT];
    bool setStale;
    FrameSlot2D slots[SLOTS];
    int slot;                 /* the next to use */
    int last;                 /* the last submitted; -1: none */
    VkCommandBuffer commands; /* (the slot's being written) */
    /* timestamps: two a slot */
    VkQueryPool timestamps;
    float timestampPeriod;
    double gpuMs;
    /* the window */
    ArtfSurface native;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkImage images[MAX_IMAGES];
    VkSemaphore rendered[MAX_IMAGES];
    uint32_t imageCount;
    int format;
    bool swapRedBlue; /* an RGBA swapchain: the shaders write red where blue was */
    bool noVsync, swapchainStale;
    int swapWidth, swapHeight, frameWidth, frameHeight;
    /* gi's grid as the frame before had it: what walls sent on is kept on the GPU from one frame to the next */
    int giWidth, giHeight, giScale;
    /* the textures, as they are on the GPU */
    uint64_t textureScene;
    ARTF_VEC(TextureGpu) textures;
    ARTF_VEC(uint32_t) levelBase;
    uint32_t texelTop, texelWaste;
    ARTF_VEC(uint8_t) upload; /* what goes up this frame, as laid out in staging */
};

/* ---- the device ---- */

static bool loadLibrary(Gpu2D* g) {
#ifdef _WIN32
    g->library = LoadLibraryA("vulkan-1.dll");
    if (!g->library) return false;
    *(void**)&g->getInstanceProcAddr = (void*)GetProcAddress(g->library, "vkGetInstanceProcAddr");
#else
#ifdef __APPLE__
    static const char* const names[] = {"libvulkan.1.dylib", "libMoltenVK.dylib"};
#else
    static const char* const names[] = {"libvulkan.so.1", "libvulkan.so"};
#endif
    for (size_t i = 0; i < ARTF_COUNT(names) && !g->library; ++i) g->library = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!g->library) return false;
    *(void**)&g->getInstanceProcAddr = dlsym(g->library, "vkGetInstanceProcAddr");
#endif
    return g->getInstanceProcAddr != NULL;
}

static bool createSurface(Gpu2D* g) {
    typedef VkResult(VKAPI * Create)(VkInstance, const void*, const void*, VkSurfaceKHR*);
    Create create;
    const ArtfSurface* n = &g->native;
    switch (n->kind) {
    case ARTF_SURFACE_WIN32: {
#ifdef _WIN32
        *(void**)&create = g->getInstanceProcAddr(g->instance, "vkCreateWin32SurfaceKHR");
        const Win32SurfaceCreateInfo info = {ST_WIN32_SURFACE, NULL, 0, (void*)GetModuleHandleA(NULL), n->a};
        return create && create(g->instance, &info, NULL, &g->surface) == VK_OK;
#else
        return false;
#endif
    }
    case ARTF_SURFACE_XLIB: {
        *(void**)&create = g->getInstanceProcAddr(g->instance, "vkCreateXlibSurfaceKHR");
        const XlibSurfaceCreateInfo info = {ST_XLIB_SURFACE, NULL, 0, n->a, (unsigned long)(uintptr_t)n->b};
        return create && create(g->instance, &info, NULL, &g->surface) == VK_OK;
    }
    case ARTF_SURFACE_WAYLAND: {
        *(void**)&create = g->getInstanceProcAddr(g->instance, "vkCreateWaylandSurfaceKHR");
        const WaylandSurfaceCreateInfo info = {ST_WAYLAND_SURFACE, NULL, 0, n->a, n->b};
        return create && create(g->instance, &info, NULL, &g->surface) == VK_OK;
    }
    case ARTF_SURFACE_ANDROID: {
        *(void**)&create = g->getInstanceProcAddr(g->instance, "vkCreateAndroidSurfaceKHR");
        const AndroidSurfaceCreateInfo info = {ST_ANDROID_SURFACE, NULL, 0, n->a};
        return create && create(g->instance, &info, NULL, &g->surface) == VK_OK;
    }
    case ARTF_SURFACE_METAL: {
        *(void**)&create = g->getInstanceProcAddr(g->instance, "vkCreateMetalSurfaceEXT");
        const MetalSurfaceCreateInfo info = {ST_METAL_SURFACE, NULL, 0, n->a};
        return create && create(g->instance, &info, NULL, &g->surface) == VK_OK;
    }
    default:
        return true;
    }
}

static bool createDevice(Gpu2D* g) {
    *(void**)&g->vkCreateInstance = g->getInstanceProcAddr(NULL, "vkCreateInstance");
    if (!g->vkCreateInstance) return false;
    const char* extensions[3];
    uint32_t extensionCount = 0;
    VkFlags flags = 0;
    if (g->native.kind != ARTF_SURFACE_NONE) {
        extensions[extensionCount++] = "VK_KHR_surface";
        switch (g->native.kind) {
        case ARTF_SURFACE_WIN32: extensions[extensionCount++] = "VK_KHR_win32_surface"; break;
        case ARTF_SURFACE_XLIB: extensions[extensionCount++] = "VK_KHR_xlib_surface"; break;
        case ARTF_SURFACE_WAYLAND: extensions[extensionCount++] = "VK_KHR_wayland_surface"; break;
        case ARTF_SURFACE_ANDROID: extensions[extensionCount++] = "VK_KHR_android_surface"; break;
        default:
            extensions[extensionCount++] = "VK_EXT_metal_surface";
            extensions[extensionCount++] = "VK_KHR_portability_enumeration";
            flags = 1;
            break;
        }
    }
#ifdef __APPLE__
    if (!flags) {
        extensions[extensionCount++] = "VK_KHR_portability_enumeration";
        flags = 1;
    }
#endif
    const ApplicationInfo app = {ST_APPLICATION_INFO, NULL, "artful-2d", 1, "artful", 1, 1u << 22 | 1u << 12};
    const InstanceCreateInfo info = {ST_INSTANCE_CREATE_INFO, NULL, flags, &app, 0, NULL, extensionCount, extensions};
    if (g->vkCreateInstance(&info, NULL, &g->instance) != VK_OK) return false;
#define X(name, result, args) *(void**)&g->name = g->getInstanceProcAddr(g->instance, #name);
    ARTF_VK_INSTANCE_FUNCTIONS(X)
#undef X
    if (!g->vkEnumeratePhysicalDevices || !createSurface(g)) return false;
    VkPhysicalDevice gpus[16];
    uint32_t count = 16;
    if (g->vkEnumeratePhysicalDevices(g->instance, &count, gpus) < 0) return false;
    int best = -1;
    PhysicalDeviceProperties chosen;
    for (uint32_t i = 0; i < count; ++i) {
        PhysicalDeviceProperties props;
        g->vkGetPhysicalDeviceProperties(gpus[i], &props);
        if (props.apiVersion < (1u << 22 | 1u << 12)) continue;
        QueueFamilyProperties families[32];
        uint32_t familyCount = 32;
        g->vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &familyCount, families);
        int family = -1;
        for (uint32_t q = 0; q < familyCount && family < 0; ++q) {
            if (!(families[q].flags & 2)) continue; /* compute */
            VkBool32 present = 1;
            if (g->surface) g->vkGetPhysicalDeviceSurfaceSupportKHR(gpus[i], q, g->surface, &present);
            if (present) family = (int)q;
        }
        if (family < 0) continue;
        const int score = props.deviceType == 2 ? 3 : props.deviceType == 1 ? 2 : 1; /* discrete, integrated, others */
        if (score > best) {
            best = score;
            g->gpu = gpus[i];
            g->family = (uint32_t)family;
            chosen = props;
            snprintf(g->name, sizeof g->name, "%s", props.deviceName);
        }
    }
    if (best < 0) return false;
    g->vkGetPhysicalDeviceMemoryProperties(g->gpu, &g->memory);
    /* (timestampComputeAndGraphics and timestampPeriod, where the limits have them) */
    uint32_t timestampsThere;
    memcpy(&timestampsThere, chosen.rest + 348, 4);
    memcpy(&g->timestampPeriod, chosen.rest + 352, 4);
    if (!timestampsThere) g->timestampPeriod = 0;
    const float priority = 1;
    const DeviceQueueCreateInfo queue = {ST_DEVICE_QUEUE_CREATE_INFO, NULL, 0, g->family, 1, &priority};
    const char* swapchain = "VK_KHR_swapchain";
    /* (ARTFUL_ROBUST, as the tests set it, and as the 3D device takes it: a read past a buffer gives 0, not a fault) */
    PhysicalDeviceFeatures features;
    memset(&features, 0, sizeof features);
    features.robustBufferAccess = getenv("ARTFUL_ROBUST") != NULL;
    const DeviceCreateInfo device = {ST_DEVICE_CREATE_INFO, NULL, 0, 1, &queue, 0, NULL, g->surface ? 1u : 0u, g->surface ? &swapchain : NULL, &features};
    if (g->vkCreateDevice(g->gpu, &device, NULL, &g->device) != VK_OK) return false;
#define X(name, result, args) *(void**)&g->name = g->vkGetDeviceProcAddr(g->device, #name);
    ARTF_VK_DEVICE_FUNCTIONS(X)
#undef X
    *(void**)&g->vkCmdClearColorImage = g->vkGetDeviceProcAddr(g->device, "vkCmdClearColorImage");
    g->vkGetDeviceQueue(g->device, g->family, 0, &g->queue);
    return true;
}

static int memoryType(const Gpu2D* g, uint32_t bits, VkFlags want, VkFlags fallback) {
    for (int pass = 0; pass < 2; ++pass) {
        const VkFlags need = pass ? fallback : want;
        for (uint32_t i = 0; i < g->memory.typeCount; ++i)
            if (bits & 1u << i && (g->memory.types[i].propertyFlags & need) == need) return (int)i;
    }
    return -1;
}

static void dropBuffer(Gpu2D* g, Buffer2D* b) {
    if (b->buffer) g->vkDestroyBuffer(g->device, b->buffer, NULL);
    if (b->memory) g->vkFreeMemory(g->device, b->memory, NULL);
    memset(b, 0, sizeof *b);
}

/* What a slot's frame took on the GPU, once it is done. */
static void slotTimes(Gpu2D* g, int i) {
    if (!g->timestamps) return;
    uint64_t ticks[2];
    if (g->vkGetQueryPoolResults(g->device, g->timestamps, (uint32_t)i * 2, 2, sizeof ticks, ticks, 8, 1 /* 64 bits */) == VK_OK)
        g->gpuMs = (double)(ticks[1] - ticks[0]) * g->timestampPeriod * 1e-6;
}

/* Waits for a slot's frame, if it is on its way. false: the device is lost. */
static bool slotWait(Gpu2D* g, int i) {
    FrameSlot2D* s = &g->slots[i];
    if (!s->pending) return true;
    if (g->vkWaitForFences(g->device, 1, &s->fence, 1, UINT64_MAX) != VK_OK) return false;
    s->pending = false;
    slotTimes(g, i);
    return true;
}

/* Every frame done: before what they read is made again. */
static bool idle(Gpu2D* g) {
    bool ok = true;
    for (int i = 0; i < SLOTS; ++i) ok = slotWait(g, i) && ok;
    return ok;
}

/* At least size bytes; on the GPU (host: where the CPU writes or reads, a slot's own). A buffer of the GPU made again
 * waits for every frame that may read it. */
static bool ensure(Gpu2D* g, Buffer2D* b, VkDeviceSize size, bool host, VkFlags usage) {
    size = size < 64 ? 64 : size;
    if (b->buffer && b->size >= size) return true;
    if (b->buffer && !host) idle(g);
    dropBuffer(g, b);
    size += size / 4; /* (room to grow) */
    const BufferCreateInfo info = {ST_BUFFER_CREATE_INFO, NULL, 0, size, usage, 0, 0, NULL};
    if (g->vkCreateBuffer(g->device, &info, NULL, &b->buffer) != VK_OK) return false;
    MemoryRequirements need;
    g->vkGetBufferMemoryRequirements(g->device, b->buffer, &need);
    const int type = host ? memoryType(g, need.typeBits, MEMORY_HOST | MEMORY_COHERENT | MEMORY_CACHED, MEMORY_HOST | MEMORY_COHERENT)
                          : memoryType(g, need.typeBits, MEMORY_DEVICE, 0);
    if (type < 0) return false;
    const MemoryAllocateInfo alloc = {ST_MEMORY_ALLOCATE_INFO, NULL, need.size, (uint32_t)type};
    if (g->vkAllocateMemory(g->device, &alloc, NULL, &b->memory) != VK_OK) return false;
    if (g->vkBindBufferMemory(g->device, b->buffer, b->memory, 0) != VK_OK) return false;
    if (host && g->vkMapMemory(g->device, b->memory, 0, size, 0, &b->mapped) != VK_OK) return false;
    b->size = size;
    if (!host) g->setStale = true;
    return true;
}

static VkPipeline pipelineOf(Gpu2D* g, uint32_t* code, size_t size) {
    VkShaderModule module = 0;
    VkPipeline pipeline = 0;
    const ShaderModuleCreateInfo info = {ST_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    if (g->vkCreateShaderModule(g->device, &info, NULL, &module) == VK_OK) {
        const ComputePipelineCreateInfo pipe = {ST_COMPUTE_PIPELINE, NULL, 0, {ST_PIPELINE_SHADER_STAGE, NULL, 0, SHADER_COMPUTE, module, "main", NULL}, g->layout, 0, -1};
        if (g->vkCreateComputePipelines(g->device, 0, 1, &pipe, NULL, &pipeline) != VK_OK) pipeline = 0;
        g->vkDestroyShaderModule(g->device, module, NULL);
    }
    artfFree(code);
    return pipeline;
}

static bool createPipelines(Gpu2D* g) {
    DescriptorSetLayoutBinding bindings[B2_COUNT];
    for (uint32_t i = 0; i < B2_COUNT; ++i) bindings[i] = (DescriptorSetLayoutBinding){i, DESCRIPTOR_STORAGE_BUFFER, 1, SHADER_COMPUTE, NULL};
    const DescriptorSetLayoutCreateInfo setInfo = {ST_DESCRIPTOR_SET_LAYOUT, NULL, 0, B2_COUNT, bindings};
    if (g->vkCreateDescriptorSetLayout(g->device, &setInfo, NULL, &g->setLayout) != VK_OK) return false;
    const PushConstantRange push = {SHADER_COMPUTE, 0, sizeof(Push2D)};
    const PipelineLayoutCreateInfo layoutInfo = {ST_PIPELINE_LAYOUT, NULL, 0, 1, &g->setLayout, 1, &push};
    if (g->vkCreatePipelineLayout(g->device, &layoutInfo, NULL, &g->layout) != VK_OK) return false;
    const DescriptorPoolSize poolSize = {DESCRIPTOR_STORAGE_BUFFER, B2_COUNT};
    const DescriptorPoolCreateInfo poolInfo = {ST_DESCRIPTOR_POOL, NULL, 0, 1, 1, &poolSize};
    if (g->vkCreateDescriptorPool(g->device, &poolInfo, NULL, &g->descriptorPool) != VK_OK) return false;
    const DescriptorSetAllocateInfo allocInfo = {ST_DESCRIPTOR_SET_ALLOCATE, NULL, g->descriptorPool, 1, &g->setLayout};
    if (g->vkAllocateDescriptorSets(g->device, &allocInfo, &g->set) != VK_OK) return false;
    size_t size;
    uint32_t* code;
#define MAKE(pipe, call) code = call; g->pipelines[pipe] = pipelineOf(g, code, size)
    MAKE(PIPE_TILES, artf2DShaderTiles(&size));
    MAKE(PIPE_DOWN, artf2DShaderBloomDown(&size));
    MAKE(PIPE_UP, artf2DShaderBloomUp(&size));
    MAKE(PIPE_FINISH, artf2DShaderFinish(&size));
    MAKE(PIPE_REFLECT, artf2DShaderReflect(&size));
    MAKE(PIPE_GI_SCENE, artf2DShaderGiScene(&size));
    MAKE(PIPE_GI_ROWS, artf2DShaderGiDistance(&size, false));
    MAKE(PIPE_GI_COLUMNS, artf2DShaderGiDistance(&size, true));
    MAKE(PIPE_GI_EVEN, artf2DShaderGiCascade(&size, false));
    MAKE(PIPE_GI_ODD, artf2DShaderGiCascade(&size, true));
    MAKE(PIPE_GI_GATHER, artf2DShaderGiGather(&size));
    MAKE(PIPE_GI_SPREAD0, artf2DShaderGiSpread(&size, 0));
    MAKE(PIPE_GI_SPREAD1, artf2DShaderGiSpread(&size, 1));
#undef MAKE
    for (int i = 0; i < PIPES; ++i)
        if (!g->pipelines[i]) return false;
    const CommandPoolCreateInfo poolCommands = {ST_COMMAND_POOL, NULL, 2 /* reset each buffer */, g->family};
    if (g->vkCreateCommandPool(g->device, &poolCommands, NULL, &g->pool) != VK_OK) return false;
    const FenceCreateInfo fence = {ST_FENCE_CREATE_INFO, NULL, 1 /* signaled */};
    const SemaphoreCreateInfo semaphore = {ST_SEMAPHORE_CREATE_INFO, NULL, 0};
    for (int i = 0; i < SLOTS; ++i) {
        FrameSlot2D* s = &g->slots[i];
        const CommandBufferAllocateInfo commands = {ST_COMMAND_BUFFER_ALLOCATE, NULL, g->pool, 0, 1};
        if (g->vkAllocateCommandBuffers(g->device, &commands, &s->commands) != VK_OK) return false;
        if (g->vkCreateFence(g->device, &fence, NULL, &s->fence) != VK_OK) return false;
        if (g->vkCreateSemaphore(g->device, &semaphore, NULL, &s->acquired) != VK_OK) return false;
    }
    for (int i = 0; i < MAX_IMAGES; ++i)
        if (g->surface && g->vkCreateSemaphore(g->device, &semaphore, NULL, &g->rendered[i]) != VK_OK) return false;
    if (g->timestampPeriod > 0) {
        const QueryPoolCreateInfo queries = {11 /* ST_QUERY_POOL */, NULL, 0, 2 /* timestamps */, SLOTS * 2, 0};
        if (g->vkCreateQueryPool(g->device, &queries, NULL, &g->timestamps) != VK_OK) g->timestamps = 0;
    }
    return true;
}

/* ---- the window ---- */

static void dropSwapchain(Gpu2D* g) {
    if (g->swapchain) g->vkDestroySwapchainKHR(g->device, g->swapchain, NULL);
    g->swapchain = 0;
    g->imageCount = 0;
}

/* The window's images, of the frame's size where the window lets it choose; copied into, then shown. */
static bool makeSwapchain(Gpu2D* g) {
    idle(g);
    SurfaceCapabilities caps;
    if (g->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g->gpu, g->surface, &caps) != VK_OK) return false;
    if (!(caps.supportedUsage & USAGE_TRANSFER_DST)) {
        artfLog("artful-2d: this window's images cannot be copied into");
        return false;
    }
    uint32_t width = (uint32_t)g->frameWidth, height = (uint32_t)g->frameHeight;
    if (caps.current.width != 0xFFFFFFFFu) width = caps.current.width, height = caps.current.height;
    width = width < caps.minExtent.width ? caps.minExtent.width : width > caps.maxExtent.width ? caps.maxExtent.width : width;
    height = height < caps.minExtent.height ? caps.minExtent.height : height > caps.maxExtent.height ? caps.maxExtent.height : height;
    g->swapWidth = (int)width;
    g->swapHeight = (int)height;
    g->swapchainStale = false;
    if (!width || !height) {
        dropSwapchain(g); /* (minimized: nothing to show into) */
        return true;
    }
    /* BGRA bytes, as the frame's; RGBA, with red and blue swapped by the shaders */
    SurfaceFormat formats[64];
    uint32_t formatCount = 64;
    g->vkGetPhysicalDeviceSurfaceFormatsKHR(g->gpu, g->surface, &formatCount, formats);
    int format = 0, space = 0;
    for (uint32_t i = 0; i < formatCount; ++i) {
        if (formats[i].format == FORMAT_BGRA8) { format = FORMAT_BGRA8; space = formats[i].colorSpace; break; }
        if (formats[i].format == FORMAT_RGBA8 && !format) format = FORMAT_RGBA8, space = formats[i].colorSpace;
    }
    if (!format) {
        artfLog("artful-2d: this window takes neither BGRA nor RGBA bytes");
        return false;
    }
    g->format = format;
    g->swapRedBlue = format == FORMAT_RGBA8;
    int modes[16], mode = 2; /* FIFO: always there */
    uint32_t modeCount = 16;
    g->vkGetPhysicalDeviceSurfacePresentModesKHR(g->gpu, g->surface, &modeCount, modes);
    if (g->noVsync)
        for (uint32_t i = 0; i < modeCount; ++i) {
            if (modes[i] == 0) { mode = 0; break; } /* immediate */
            if (modes[i] == 1) mode = 1;             /* mailbox */
        }
    uint32_t images = caps.minImageCount + 1;
    if (caps.maxImageCount && images > caps.maxImageCount) images = caps.maxImageCount;
    if (images > MAX_IMAGES) images = MAX_IMAGES;
    const VkFlags alpha = caps.supportedAlpha & 1 ? 1 : caps.supportedAlpha & (~caps.supportedAlpha + 1);
    VkSwapchainKHR old = g->swapchain;
    const SwapchainCreateInfo info = {ST_SWAPCHAIN_CREATE_INFO, NULL, 0, g->surface, images, format, space, {width, height}, 1, USAGE_TRANSFER_DST, 0, 0, NULL,
                                      caps.currentTransform, alpha, mode, 1, old};
    VkSwapchainKHR made = 0;
    const VkResult result = g->vkCreateSwapchainKHR(g->device, &info, NULL, &made);
    if (old) g->vkDestroySwapchainKHR(g->device, old, NULL);
    g->swapchain = made;
    if (result != VK_OK) {
        artfLog("artful-2d: the driver refused a swapchain: %d", (int)result);
        g->swapchain = 0;
        return false;
    }
    g->imageCount = MAX_IMAGES;
    g->vkGetSwapchainImagesKHR(g->device, g->swapchain, &g->imageCount, g->images);
    return true;
}

Gpu2D* artf2DVulkanNew(const ArtfSurface* surface, bool noVsync, int width, int height) {
    Gpu2D* g = ARTF_NEW(Gpu2D);
    if (surface) g->native = *surface;
    g->noVsync = noVsync;
    g->frameWidth = width;
    g->frameHeight = height;
    g->last = -1;
    if (!loadLibrary(g) || !createDevice(g) || !createPipelines(g) || (g->surface && !makeSwapchain(g))) {
        artfLog("artful-2d: no Vulkan here%s%s", g->name[0] ? ": " : "", g->name);
        artf2DVulkanFree(g);
        return NULL;
    }
    return g;
}

void artf2DVulkanFree(Gpu2D* g) {
    if (!g) return;
    if (g->device) {
        g->vkDeviceWaitIdle(g->device);
        for (int i = 0; i < B2_COUNT; ++i) dropBuffer(g, &g->buffers[i]);
        for (int i = 0; i < SLOTS; ++i) {
            FrameSlot2D* s = &g->slots[i];
            dropBuffer(g, &s->staging);
            dropBuffer(g, &s->readback);
            if (s->fence) g->vkDestroyFence(g->device, s->fence, NULL);
            if (s->acquired) g->vkDestroySemaphore(g->device, s->acquired, NULL);
        }
        for (int i = 0; i < MAX_IMAGES; ++i)
            if (g->rendered[i]) g->vkDestroySemaphore(g->device, g->rendered[i], NULL);
        dropSwapchain(g);
        for (int i = 0; i < PIPES; ++i)
            if (g->pipelines[i]) g->vkDestroyPipeline(g->device, g->pipelines[i], NULL);
        if (g->timestamps) g->vkDestroyQueryPool(g->device, g->timestamps, NULL);
        if (g->pool) g->vkDestroyCommandPool(g->device, g->pool, NULL);
        if (g->descriptorPool) g->vkDestroyDescriptorPool(g->device, g->descriptorPool, NULL);
        if (g->layout) g->vkDestroyPipelineLayout(g->device, g->layout, NULL);
        if (g->setLayout) g->vkDestroyDescriptorSetLayout(g->device, g->setLayout, NULL);
        g->vkDestroyDevice(g->device, NULL);
    }
    /* (the window goes back with the surface: what comes next, the CPU, could not reach it otherwise) */
    if (g->surface) g->vkDestroySurfaceKHR(g->instance, g->surface, NULL);
    if (g->instance) g->vkDestroyInstance(g->instance, NULL);
#ifdef _WIN32
    if (g->library) FreeLibrary(g->library);
#else
    if (g->library) dlclose(g->library);
#endif
    artfVecFree(&g->textures);
    artfVecFree(&g->levelBase);
    artfVecFree(&g->upload);
    artfFree(g);
}

const char* artf2DVulkanName(const Gpu2D* g) { return g->name; }
bool artf2DVulkanShows(const Gpu2D* g) { return g->surface != 0; }
double artf2DVulkanGpuMs(const Gpu2D* g) { return g->gpuMs; }

void artf2DVulkanResize(Gpu2D* g, int width, int height) {
    g->frameWidth = width;
    g->frameHeight = height;
    g->swapchainStale = true;
}

/* ---- what goes up ---- */

typedef struct Part2D { size_t at, size; } Part2D;

/* Room for size bytes in this frame's upload, 16 aligned. */
static Part2D reserve(Gpu2D* g, size_t size) {
    Part2D p = {(g->upload.len + 15) & ~(size_t)15, size ? size : 16};
    artfVecReserve(&g->upload, p.at + p.size);
    memset(g->upload.items + g->upload.len, 0, p.at + p.size - g->upload.len);
    g->upload.len = p.at + p.size;
    return p;
}
static void* partOf(Gpu2D* g, Part2D p) { return g->upload.items + p.at; }

static float bitsF(uint32_t u) {
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* A copy of this frame's: from staging, into a binding at a place. */
typedef struct Copy2D { int binding; Part2D part; VkDeviceSize to; } Copy2D;
typedef ARTF_VEC(Copy2D) Copies2D;

/* ---- textures: only what changed goes up ---- */

uint64_t artf2DVulkanTexelsSent;

static uint32_t texelsOf(const Texture2D* t) {
    uint32_t n = 0;
    for (int l = 0; l < t->levels; ++l) n += (uint32_t)(t->width[l] * t->height[l]);
    return n;
}

/* Places the scene's textures in binding 6, and adds what must go up to copies. false: no room could be made. */
static bool texturesUp(Gpu2D* g, const ArtfScene2D* s, Copies2D* copies, Part2D* levels) {
    const uint64_t scene = s->textureVersion >> 32;
    bool fresh = g->textureScene != scene;
    for (int pass = 0; pass < 2; ++pass) {
        if (fresh) {
            /* (all of them, from the start) */
            g->textureScene = scene;
            g->textures.len = 0;
            g->texelTop = g->texelWaste = 0;
        }
        artfVecReserve(&g->textures, s->textures.len);
        while (g->textures.len < s->textures.len) {
            const TextureGpu none = {0};
            artfVecPush(&g->textures, none);
        }
        bool changed = fresh, repack = false;
        uint32_t top = g->texelTop, waste = g->texelWaste;
        /* first only where each goes, so that a layout that does not fit can start again */
        for (size_t i = 0; i < s->textures.len; ++i) {
            const Texture2D* t = &s->textures.items[i];
            TextureGpu* e = &g->textures.items[i];
            if (!t->slot.alive) {
                if (e->placed) waste += e->capacity, changed = true;
                continue;
            }
            if (e->placed && e->version == t->version) continue;
            const uint32_t need = texelsOf(t);
            if (!e->placed || need > e->capacity) {
                if (e->placed) waste += e->capacity;
                top += need;
            }
            changed = true;
        }
        if (!fresh && (waste > top / 2 && waste > (1u << 20))) repack = true;
        const VkDeviceSize room = g->buffers[B2_TEXELS].buffer ? g->buffers[B2_TEXELS].size / 4 : 0;
        if (!fresh && top > room) repack = true; /* (a larger buffer has nothing in it) */
        if (repack) {
            fresh = true;
            continue;
        }
        if (!ensure(g, &g->buffers[B2_TEXELS], (VkDeviceSize)top * 4, false, USAGE_STORAGE | USAGE_TRANSFER_DST)) return false;
        /* now in earnest */
        for (size_t i = 0; i < s->textures.len; ++i) {
            const Texture2D* t = &s->textures.items[i];
            TextureGpu* e = &g->textures.items[i];
            if (!t->slot.alive) {
                if (e->placed) g->texelWaste += e->capacity;
                e->placed = false;
                continue;
            }
            if (e->placed && e->version == t->version) continue;
            const uint32_t need = texelsOf(t);
            if (!e->placed || need > e->capacity) {
                if (e->placed) g->texelWaste += e->capacity;
                e->offset = g->texelTop;
                e->capacity = need;
                g->texelTop += need;
            }
            e->placed = true;
            e->version = t->version;
            artf2DVulkanTexelsSent += need;
            Part2D part = reserve(g, (size_t)need * 4);
            uint32_t* to = partOf(g, part);
            for (int l = 0; l < t->levels; ++l) {
                const size_t n = (size_t)t->width[l] * t->height[l];
                memcpy(to, t->texels[l], n * 4);
                to += n;
            }
            const Copy2D copy = {B2_TEXELS, part, (VkDeviceSize)e->offset * 4};
            artfVecPush(copies, copy);
        }
        /* the table of levels, when anything moved: where each level of each texture starts */
        if (changed || !g->buffers[B2_LEVELS].buffer) {
            size_t levelCount = 0;
            for (size_t i = 0; i < s->textures.len; ++i)
                if (s->textures.items[i].slot.alive) levelCount += (size_t)s->textures.items[i].levels;
            *levels = reserve(g, (levelCount + 1) * 16);
            if (!ensure(g, &g->buffers[B2_LEVELS], levels->size, false, USAGE_STORAGE | USAGE_TRANSFER_DST)) return false;
            uint32_t* lv = partOf(g, *levels);
            g->levelBase.len = 0;
            uint32_t level = 0;
            for (size_t i = 0; i < s->textures.len; ++i) {
                const Texture2D* t = &s->textures.items[i];
                artfVecPush(&g->levelBase, level);
                if (!t->slot.alive) continue;
                uint32_t texel = g->textures.items[i].offset;
                for (int l = 0; l < t->levels; ++l) {
                    lv[level * 4] = texel, lv[level * 4 + 1] = (uint32_t)t->width[l], lv[level * 4 + 2] = (uint32_t)t->height[l], lv[level * 4 + 3] = t->normalMap;
                    texel += (uint32_t)(t->width[l] * t->height[l]);
                    ++level;
                }
            }
            if (!levelCount) lv[1] = lv[2] = 1; /* (a texel to read where there is no texture) */
            const Copy2D copy = {B2_LEVELS, *levels, 0};
            artfVecPush(copies, copy);
        }
        return true;
    }
    return true;
}

/* ---- a frame ---- */

static void barrier(Gpu2D* g, VkFlags srcStage, VkFlags dstStage, VkFlags srcAccess, VkFlags dstAccess) {
    const GlobalBarrier b = {ST_MEMORY_BARRIER, NULL, srcAccess, dstAccess};
    g->vkCmdPipelineBarrier(g->commands, srcStage, dstStage, 0, 1, &b, 0, NULL, 0, NULL);
}

static void imageBarrier(Gpu2D* g, VkImage image, int from, int to, VkFlags srcStage, VkFlags dstStage, VkFlags srcAccess, VkFlags dstAccess) {
    const ImageMemoryBarrier b = {ST_IMAGE_MEMORY_BARRIER, NULL, srcAccess, dstAccess, from, to, ~0u, ~0u, image, {ASPECT_COLOR, 0, 1, 0, 1}};
    g->vkCmdPipelineBarrier(g->commands, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ARTFUL_2D_STEP: each dispatch on its own, waited for and named on stderr (to tell which shader a driver falls over in) */
static void stepped(Gpu2D* g, int pipe) {
    static int on = -1;
    if (on < 0) on = getenv("ARTFUL_2D_STEP") != NULL;
    if (!on) return;
    static const char* names[PIPES] = {"tiles", "down", "up", "finish", "reflect", "giScene", "giRows", "giColumns", "giEven", "giOdd", "giGather", "giSpread0", "giSpread1"};
    fprintf(stderr, "2d step: %s ", names[pipe]);
    fflush(stderr);
    FrameSlot2D* s = &g->slots[g->slot];
    g->vkEndCommandBuffer(g->commands);
    const SubmitInfo submit = {ST_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &g->commands, 0, NULL};
    g->vkResetFences(g->device, 1, &s->fence);
    g->vkQueueSubmit(g->queue, 1, &submit, s->fence);
    g->vkWaitForFences(g->device, 1, &s->fence, 1, UINT64_MAX);
    fprintf(stderr, "done\n");
    fflush(stderr);
    g->vkResetCommandBuffer(g->commands, 0);
    const CommandBufferBeginInfo begin = {ST_COMMAND_BUFFER_BEGIN, NULL, 1, NULL};
    g->vkBeginCommandBuffer(g->commands, &begin);
    g->vkCmdBindDescriptorSets(g->commands, BIND_COMPUTE, g->layout, 0, 1, &g->set, 0, NULL);
}

static void dispatch(Gpu2D* g, int pipe, const Push2D* push, uint32_t width, uint32_t height) {
    g->vkCmdBindPipeline(g->commands, BIND_COMPUTE, g->pipelines[pipe]);
    if (push) g->vkCmdPushConstants(g->commands, g->layout, SHADER_COMPUTE, 0, sizeof *push, push);
    g->vkCmdDispatch(g->commands, (width + 7) / 8, (height + 7) / 8, 1);
    stepped(g, pipe);
}

/* A 1D dispatch: groups of 64. */
static void dispatchLine(Gpu2D* g, int pipe, const Push2D* push, uint32_t count) {
    g->vkCmdBindPipeline(g->commands, BIND_COMPUTE, g->pipelines[pipe]);
    if (push) g->vkCmdPushConstants(g->commands, g->layout, SHADER_COMPUTE, 0, sizeof *push, push);
    g->vkCmdDispatch(g->commands, (count + 63) / 64, 1, 1);
    stepped(g, pipe);
}

static void computeBarrier(Gpu2D* g) { barrier(g, STAGE_COMPUTE, STAGE_COMPUTE, ACCESS_SHADER_WRITE, ACCESS_SHADER_READ | ACCESS_SHADER_WRITE); }

/* gi's cascades, as gi2d.c counts them */
static int giSpacing(int c) { return 1 << c; }
static uint32_t giRays(int c) { return 4u << (2 * c); }
static float giStart(int c) { return (float)((1 << (2 * c)) - 1) / 3.f; }

bool artf2DVulkanDraw(Gpu2D* g, const Frame2D* f, const ArtfGI2DDesc* giDesc, bool readBack) {
    const ArtfScene2D* s = f->scene;
    const int width = f->width, height = f->height, tiles = f->tilesX * f->tilesY;
    g->frameWidth = width;
    g->frameHeight = height;
    /* the slot: what was in it before must be done */
    const int slotIndex = g->slot;
    FrameSlot2D* slot = &g->slots[slotIndex];
    if (!slotWait(g, slotIndex)) {
        artfLog("artful-2d: the Vulkan device was lost");
        return false;
    }
    g->commands = slot->commands;
    /* the window's image */
    uint32_t image = 0;
    bool shows = false;
    if (g->surface) {
        if (g->swapchainStale || !g->swapchain) {
            if (!makeSwapchain(g)) return false;
        }
        if (g->swapchain) {
            VkResult result = g->vkAcquireNextImageKHR(g->device, g->swapchain, UINT64_MAX, slot->acquired, 0, &image);
            if (result == VK_ERR_OUT_OF_DATE) {
                if (!makeSwapchain(g)) return false;
                if (g->swapchain) result = g->vkAcquireNextImageKHR(g->device, g->swapchain, UINT64_MAX, slot->acquired, 0, &image);
            }
            if (result == VK_SUBOPTIMAL) g->swapchainStale = true;
            shows = g->swapchain && (result == VK_OK || result == VK_SUBOPTIMAL);
            if (!shows && result != VK_ERR_OUT_OF_DATE && result != VK_OK) {
                artfLog("artful-2d: the window's image could not be had: %d", (int)result);
                if (result == VK_ERR_DEVICE_LOST) return false;
            }
        }
        if (!shows && !readBack) return true; /* (minimized: nothing to draw) */
    }
    g->upload.len = 0;
    Copies2D copies = {0};

    /* textures: only those that changed */
    Part2D levels = {0, 0};
    if (!texturesUp(g, s, &copies, &levels)) {
        artfVecFree(&copies);
        artfLog("artful-2d: Vulkan could not make room for the textures");
        return false;
    }

    /* bloom's levels, as artf2DBloom makes them */
    const bool bloom = s->post.bloom > 0;
    int bloomLevels = 0, lw[9] = {width}, lh[9] = {height};
    size_t lat[9] = {0};
    size_t chain = (size_t)width * height;
    if (bloom) {
        const float radius = s->post.bloomRadius > 0 ? s->post.bloomRadius : .05f;
        const float reach = artfMaxf(radius, .001f) * (float)height;
        int want = 1;
        while (want < 8 && (float)(2 << want) < reach * 2.f) ++want;
        for (int l = 1; l <= want; ++l) {
            lw[l] = artfMaxi((lw[l - 1] + 1) / 2, 1);
            lh[l] = artfMaxi((lh[l - 1] + 1) / 2, 1);
            lat[l] = chain;
            chain += (size_t)lw[l] * lh[l];
            bloomLevels = l;
            if (lw[l] == 1 && lh[l] == 1) break;
        }
    }
    const float amount = artfClampf(s->post.bloom, 0, 1);

    /* gi's grid and cascades, as gi2d.c */
    const bool gi = giDesc && giDesc->enabled;
    const int giScale = gi ? giDesc->scale : 1;
    const int giW = gi ? (width + giScale - 1) / giScale : 1, giH = gi ? (height + giScale - 1) / giScale : 1;
    int cascades = 0;
    size_t mostRays = 1;
    if (gi) {
        const float across = sqrtf((float)giW * giW + (float)giH * giH);
        cascades = 1;
        while (cascades < 10 && giStart(cascades) < across) ++cascades;
        for (int c = 0; c < cascades; ++c) {
            const int sp = giSpacing(c);
            const size_t rays = (size_t)((giW + sp - 1) / sp) * ((giH + sp - 1) / sp) * giRays(c);
            if (rays > mostRays) mostRays = rays;
        }
    }

    /* outlines, their points, and what glows (for gi) */
    size_t emitterCount = 0;
    for (size_t i = 0; gi && i < f->items.len; ++i) emitterCount += f->items.items[i].emits;
    Part2D outlines = reserve(g, gi ? f->outlines.len * 32 : 0), points = reserve(g, gi ? f->outlinePoints.len * 16 : 0), emitters = reserve(g, emitterCount * 4);
    if (gi) {
        float* o = partOf(g, outlines);
        for (size_t i = 0; i < f->outlines.len; ++i, o += 8) {
            const Outline2D* ol = &f->outlines.items[i];
            o[0] = bitsF((uint32_t)ol->start), o[1] = bitsF((uint32_t)ol->count), o[2] = bitsF(ol->closed ? 1u : 0u);
            o[4] = ol->x0, o[5] = ol->y0, o[6] = ol->x1, o[7] = ol->y1;
        }
        float* p = partOf(g, points);
        for (size_t i = 0; i < f->outlinePoints.len; ++i) p[i * 4] = f->outlinePoints.items[i].x, p[i * 4 + 1] = f->outlinePoints.items[i].y;
        uint32_t* e = partOf(g, emitters);
        for (size_t i = 0, n = 0; i < f->items.len; ++i)
            if (f->items.items[i].emits) e[n++] = (uint32_t)i;
    }

    /* globals */
    Part2D globals = reserve(g, G_ROWS * 16);
    {
        float* v = partOf(g, globals);
        const uint32_t size[4] = {(uint32_t)width, (uint32_t)height, (uint32_t)f->tilesX, (uint32_t)giScale};
        for (int i = 0; i < 4; ++i) v[G_SIZE * 4 + i] = bitsF(size[i]);
        v[G_AMBIENT * 4] = f->ambient[0], v[G_AMBIENT * 4 + 1] = f->ambient[1], v[G_AMBIENT * 4 + 2] = f->ambient[2], v[G_AMBIENT * 4 + 3] = f->exposure;
        for (int i = 0; i < 4; ++i) v[G_BACK * 4 + i] = f->background[i];
        const uint32_t mode[4] = {(uint32_t)f->tone, (uint32_t)giW, (uint32_t)giH, gi ? 1u : 0u};
        for (int i = 0; i < 4; ++i) v[G_MODE * 4 + i] = bitsF(mode[i]);
        v[G_FLAGS * 4] = bitsF(bloom ? 1u : 0u), v[G_FLAGS * 4 + 1] = bitsF(gi ? 1u : 0u);
        v[G_FLAGS * 4 + 2] = 1.f - amount, v[G_FLAGS * 4 + 3] = bloomLevels ? amount / (float)bloomLevels : 0;
        v[G_GI * 4] = gi ? giDesc->intensity : 0, v[G_GI * 4 + 1] = gi ? giDesc->bounce : 0;
        v[G_GI * 4 + 2] = bitsF(gi ? (uint32_t)f->outlines.len : 0u), v[G_GI * 4 + 3] = bitsF((uint32_t)emitterCount);
        v[G_OUT * 4] = bitsF(shows && g->swapRedBlue ? 1u : 0u);
    }

    /* items */
    Part2D items = reserve(g, f->items.len * ITEM_ROWS * 16);
    {
        float* v = partOf(g, items);
        for (size_t i = 0; i < f->items.len; ++i, v += ITEM_ROWS * 4) {
            const Item2D* it = &f->items.items[i];
            const ArtfSprite2DDesc* d = &it->sp->desc;
            for (int k = 0; k < 6; ++k) v[k] = it->toLocal.m[k];
            v[I_MAP1 * 4 + 2] = it->zoom, v[I_MAP1 * 4 + 3] = bitsF((uint32_t)d->shape);
            v[I_SHAPE * 4] = it->hw, v[I_SHAPE * 4 + 1] = it->hh, v[I_SHAPE * 4 + 2] = d->cornerRadius, v[I_SHAPE * 4 + 3] = d->thickness * .5f;
            if (d->shape == ARTF_SHAPE_LINE && d->pointCount >= 2) {
                v[I_LINE * 4] = it->sp->points[0].x, v[I_LINE * 4 + 1] = it->sp->points[0].y;
                v[I_LINE * 4 + 2] = it->sp->points[1].x, v[I_LINE * 4 + 3] = it->sp->points[1].y;
            }
            for (int k = 0; k < 4; ++k) v[I_UV * 4 + k] = it->uv[k];
            v[I_COLOR * 4] = d->color.x, v[I_COLOR * 4 + 1] = d->color.y, v[I_COLOR * 4 + 2] = d->color.z, v[I_COLOR * 4 + 3] = d->opacity;
            v[I_EMIT * 4] = d->emissive.x, v[I_EMIT * 4 + 1] = d->emissive.y, v[I_EMIT * 4 + 2] = d->emissive.z, v[I_EMIT * 4 + 3] = d->specular;
            v[I_TURN * 4] = d->roughness, v[I_TURN * 4 + 1] = it->turnCos, v[I_TURN * 4 + 2] = it->turnSin, v[I_TURN * 4 + 3] = bitsF(d->lightMask);
            const uint32_t flags = (it->lit ? I2_LIT : 0) | (d->nearest ? I2_NEAREST : 0) | (it->polygon ? I2_POLYGON : 0) | (it->emits ? I2_EMITS : 0);
            v[I_FLAGS * 4] = bitsF(flags), v[I_FLAGS * 4 + 1] = bitsF(it->owner);
            v[I_FLAGS * 4 + 2] = bitsF((uint32_t)it->edgeStart), v[I_FLAGS * 4 + 3] = bitsF((uint32_t)it->edgeCount);
            for (int k = 0; k < 3; ++k) {
                uint32_t level = 0xFFFFFFFFu;
                if (it->tex[k]) {
                    const size_t index = (size_t)(it->tex[k] - s->textures.items);
                    if (index < g->levelBase.len) level = g->levelBase.items[index] + (uint32_t)artfMini(it->mip[k], it->tex[k]->levels - 1);
                }
                v[I_TEX * 4 + k] = bitsF(level);
            }
            const uint32_t box[4] = {(uint32_t)it->x0, (uint32_t)it->y0, (uint32_t)it->x1, (uint32_t)it->y1};
            for (int k = 0; k < 4; ++k) v[I_BOX * 4 + k] = bitsF(box[k]);
        }
    }
    Part2D edges = reserve(g, f->edges.len * 16);
    if (f->edges.len) memcpy(partOf(g, edges), f->edges.items, f->edges.len * 16);

    /* tiles: their lists */
    size_t listed = 0;
    for (int t = 0; t < tiles; ++t) listed += f->tileItemCount[t] + f->tileLampCount[t];
    Part2D tileList = reserve(g, ((size_t)tiles * 4 + listed) * 4);
    {
        uint32_t* v = partOf(g, tileList);
        uint32_t at = (uint32_t)tiles * 4;
        for (int t = 0; t < tiles; ++t) {
            v[t * 4] = at, v[t * 4 + 1] = f->tileItemCount[t];
            memcpy(v + at, f->tileItems[t], f->tileItemCount[t] * 4);
            at += f->tileItemCount[t];
            v[t * 4 + 2] = at, v[t * 4 + 3] = f->tileLampCount[t];
            for (uint32_t k = 0; k < f->tileLampCount[t]; ++k) v[at + k] = f->tileLamps[t][k];
            at += f->tileLampCount[t];
        }
    }

    /* lamps, and their maps */
    size_t mapWords = 0;
    for (size_t i = 0; i < f->lamps.len; ++i)
        if (f->lamps.items[i].shadow) mapWords += (size_t)f->lamps.items[i].shadow->bins * 3;
    Part2D lamps = reserve(g, f->lamps.len * LAMP_ROWS * 16);
    Part2D maps = reserve(g, mapWords * 4);
    {
        float* v = partOf(g, lamps);
        uint32_t* m = partOf(g, maps);
        uint32_t offset = 0;
        for (size_t i = 0; i < f->lamps.len; ++i, v += LAMP_ROWS * 4) {
            const Lamp2D* l = &f->lamps.items[i];
            v[0] = bitsF((uint32_t)l->kind), v[1] = l->x, v[2] = l->y, v[3] = bitsF(l->mask);
            v[4] = l->color[0], v[5] = l->color[1], v[6] = l->color[2], v[7] = l->range2;
            v[8] = l->inverseRange2, v[9] = l->radius, v[10] = l->height, v[11] = bitsF(l->shadow ? 1u : 0u);
            v[12] = l->dirX, v[13] = l->dirY, v[14] = l->dirZ, v[15] = l->cosInner;
            v[16] = l->cosOuter;
            if (l->shadow) {
                const ShadowMap2D* sm = l->shadow;
                v[17] = sm->uMin, v[18] = sm->binsPerUnit, v[19] = bitsF((uint32_t)sm->bins), v[20] = bitsF(offset);
                memcpy(m + offset, sm->near_, (size_t)sm->bins * 4);
                memcpy(m + offset + sm->bins, sm->owner, (size_t)sm->bins * 4);
                memcpy(m + offset + 2 * sm->bins, sm->past, (size_t)sm->bins * 4);
                offset += (uint32_t)sm->bins * 3;
            }
        }
    }

    /* the buffers, large enough; this slot's staging and what comes back */
    const size_t pixelBytes = (size_t)width * height * 4;
    const size_t texelsOfGi = (size_t)giW * giH;
    const size_t scratch = gi ? (size_t)artfMaxi(giH * (3 * giW + 2), giW * (3 * giH + 2)) : 1;
    const VkFlags up = USAGE_STORAGE | USAGE_TRANSFER_DST, down = USAGE_STORAGE | USAGE_TRANSFER_SRC, both = up | USAGE_TRANSFER_SRC;
    bool ok = ensure(g, &g->buffers[B2_GLOBALS], globals.size, false, up) && ensure(g, &g->buffers[B2_ITEMS], items.size, false, up) &&
              ensure(g, &g->buffers[B2_EDGES], edges.size, false, up) && ensure(g, &g->buffers[B2_TILES], tileList.size, false, up) &&
              ensure(g, &g->buffers[B2_LAMPS], lamps.size, false, up) && ensure(g, &g->buffers[B2_SHADOWS], maps.size, false, up) &&
              ensure(g, &g->buffers[B2_CHAIN], bloom ? chain * 16 : 16, false, USAGE_STORAGE) && ensure(g, &g->buffers[B2_PIXELS], pixelBytes, false, down) &&
              ensure(g, &g->buffers[B2_REFLECT], gi ? (size_t)width * height * 16 : 16, false, USAGE_STORAGE) &&
              ensure(g, &g->buffers[B2_GI], texelsOfGi * 12, false, USAGE_STORAGE) && ensure(g, &g->buffers[B2_GI_TMP], texelsOfGi * 12, false, USAGE_STORAGE) &&
              ensure(g, &g->buffers[B2_CELLS], texelsOfGi * 16, false, both) && ensure(g, &g->buffers[B2_CELLS_PREV], texelsOfGi * 16, false, both) &&
              ensure(g, &g->buffers[B2_OUTLINES], outlines.size, false, up) && ensure(g, &g->buffers[B2_POINTS], points.size, false, up) &&
              ensure(g, &g->buffers[B2_EMITTERS], emitters.size, false, up) && ensure(g, &g->buffers[B2_SOLID], texelsOfGi * 4, false, USAGE_STORAGE) &&
              ensure(g, &g->buffers[B2_SOURCE], texelsOfGi * 16, false, USAGE_STORAGE) && ensure(g, &g->buffers[B2_DIST], texelsOfGi * 4, false, USAGE_STORAGE) &&
              ensure(g, &g->buffers[B2_SCRATCH], scratch * 4, false, USAGE_STORAGE) && ensure(g, &g->buffers[B2_RAYS_EVEN], mostRays * 16, false, USAGE_STORAGE) &&
              ensure(g, &g->buffers[B2_RAYS_ODD], mostRays * 16, false, USAGE_STORAGE) && ensure(g, &g->buffers[B2_TEXELS], 16, false, up) &&
              ensure(g, &g->buffers[B2_LEVELS], 16, false, up) && ensure(g, &slot->staging, g->upload.len, true, USAGE_TRANSFER_SRC) &&
              (!readBack || ensure(g, &slot->readback, pixelBytes, true, USAGE_TRANSFER_DST));
    if (!ok) {
        artfVecFree(&copies);
        artfLog("artful-2d: Vulkan could not make its buffers");
        return false;
    }
    if (g->setStale) {
        DescriptorBufferInfo infos[B2_COUNT];
        WriteDescriptorSet writes[B2_COUNT];
        for (uint32_t i = 0; i < B2_COUNT; ++i) {
            infos[i] = (DescriptorBufferInfo){g->buffers[i].buffer, 0, g->buffers[i].size};
            writes[i] = (WriteDescriptorSet){ST_WRITE_DESCRIPTOR_SET, NULL, g->set, i, 0, 1, DESCRIPTOR_STORAGE_BUFFER, NULL, &infos[i], NULL};
        }
        g->vkUpdateDescriptorSets(g->device, B2_COUNT, writes, 0, NULL);
        g->setStale = false;
    }
    memcpy(slot->staging.mapped, g->upload.items, g->upload.len);

    /* the commands */
    VkCommandBuffer cb = g->commands;
    g->vkResetCommandBuffer(cb, 0);
    const CommandBufferBeginInfo begin = {ST_COMMAND_BUFFER_BEGIN, NULL, 1, NULL};
    g->vkBeginCommandBuffer(cb, &begin);
    if (g->timestamps) {
        g->vkCmdResetQueryPool(cb, g->timestamps, (uint32_t)slotIndex * 2, 2);
        g->vkCmdWriteTimestamp(cb, STAGE_TOP, g->timestamps, (uint32_t)slotIndex * 2);
    }
    /* (what this frame sends up is written after the frame before read it and wrote what comes back) */
    barrier(g, STAGE_COMPUTE | STAGE_TRANSFER, STAGE_TRANSFER, ACCESS_SHADER_WRITE | ACCESS_TRANSFER_WRITE, ACCESS_TRANSFER_READ | ACCESS_TRANSFER_WRITE);
    const Copy2D each[] = {{B2_GLOBALS, globals, 0}, {B2_ITEMS, items, 0}, {B2_EDGES, edges, 0}, {B2_TILES, tileList, 0}, {B2_LAMPS, lamps, 0},
                           {B2_SHADOWS, maps, 0}, {B2_OUTLINES, outlines, 0}, {B2_POINTS, points, 0}, {B2_EMITTERS, emitters, 0}};
    for (size_t i = 0; i < ARTF_COUNT(each); ++i) artfVecPush(&copies, each[i]);
    for (size_t i = 0; i < copies.len; ++i) {
        const Copy2D* c = &copies.items[i];
        if (!c->part.size) continue;
        const BufferCopy region = {c->part.at, c->to, c->part.size};
        g->vkCmdCopyBuffer(cb, slot->staging.buffer, g->buffers[c->binding].buffer, 1, &region);
    }
    artfVecFree(&copies);
    if (gi) {
        /* what walls sent on last frame becomes the frame before's; a new grid starts with none */
        const VkDeviceSize cells = (VkDeviceSize)texelsOfGi * 16;
        if (giW != g->giWidth || giH != g->giHeight || giScale != g->giScale) {
            g->vkCmdFillBuffer(cb, g->buffers[B2_CELLS_PREV].buffer, 0, cells, 0);
            g->vkCmdFillBuffer(cb, g->buffers[B2_CELLS].buffer, 0, cells, 0);
        } else {
            const BufferCopy region = {0, 0, cells};
            g->vkCmdCopyBuffer(cb, g->buffers[B2_CELLS].buffer, g->buffers[B2_CELLS_PREV].buffer, 1, &region);
        }
    }
    barrier(g, STAGE_TRANSFER, STAGE_COMPUTE, ACCESS_TRANSFER_WRITE, ACCESS_SHADER_READ | ACCESS_SHADER_WRITE);
    g->vkCmdBindDescriptorSets(cb, BIND_COMPUTE, g->layout, 0, 1, &g->set, 0, NULL);
    if (gi) {
        dispatch(g, PIPE_GI_SCENE, NULL, (uint32_t)giW, (uint32_t)giH);
        computeBarrier(g);
        dispatchLine(g, PIPE_GI_ROWS, NULL, (uint32_t)giH);
        computeBarrier(g);
        dispatchLine(g, PIPE_GI_COLUMNS, NULL, (uint32_t)giW);
        for (int c = cascades - 1; c >= 0; --c) {
            computeBarrier(g);
            const uint32_t sp = (uint32_t)giSpacing(c), n = giRays(c), pw = ((uint32_t)giW + sp - 1) / sp, ph = ((uint32_t)giH + sp - 1) / sp;
            const float t0 = giStart(c), t1 = giStart(c + 1);
            uint32_t t0Bits, t1Bits;
            memcpy(&t0Bits, &t0, 4);
            memcpy(&t1Bits, &t1, 4);
            const Push2D p = {sp, n, pw, ph, c == cascades - 1, t0Bits, t1Bits, 0};
            const uint64_t total = (uint64_t)pw * ph * n;
            g->vkCmdBindPipeline(cb, BIND_COMPUTE, g->pipelines[c & 1 ? PIPE_GI_ODD : PIPE_GI_EVEN]);
            g->vkCmdPushConstants(cb, g->layout, SHADER_COMPUTE, 0, sizeof p, &p);
            const uint32_t rows = (uint32_t)((total + GI_ROW - 1) / GI_ROW);
            g->vkCmdDispatch(cb, rows > 1 ? GI_ROW / 64 : (uint32_t)((total + 63) / 64), rows, 1);
            stepped(g, c & 1 ? PIPE_GI_ODD : PIPE_GI_EVEN);
        }
        computeBarrier(g);
        dispatch(g, PIPE_GI_GATHER, NULL, (uint32_t)giW, (uint32_t)giH);
        computeBarrier(g);
        dispatch(g, PIPE_GI_SPREAD0, NULL, (uint32_t)giW, (uint32_t)giH);
        computeBarrier(g);
        dispatch(g, PIPE_GI_SPREAD1, NULL, (uint32_t)giW, (uint32_t)giH);
        computeBarrier(g);
        g->giWidth = giW, g->giHeight = giH, g->giScale = giScale;
    }
    dispatch(g, PIPE_TILES, NULL, (uint32_t)width, (uint32_t)height);
    if (bloom) {
        for (int l = 1; l <= bloomLevels; ++l) {
            computeBarrier(g);
            const Push2D p = {(uint32_t)lat[l - 1], (uint32_t)lw[l - 1], (uint32_t)lh[l - 1], (uint32_t)lat[l], (uint32_t)lw[l], (uint32_t)lh[l], 0, 0};
            dispatch(g, PIPE_DOWN, &p, (uint32_t)lw[l], (uint32_t)lh[l]);
        }
        for (int l = bloomLevels - 1; l >= 1; --l) {
            computeBarrier(g);
            const Push2D p = {(uint32_t)lat[l + 1], (uint32_t)lw[l + 1], (uint32_t)lh[l + 1], (uint32_t)lat[l], (uint32_t)lw[l], (uint32_t)lh[l], 0, 0};
            dispatch(g, PIPE_UP, &p, (uint32_t)lw[l], (uint32_t)lh[l]);
        }
        computeBarrier(g);
        const Push2D p = {(uint32_t)lat[1], (uint32_t)lw[1], (uint32_t)lh[1], 0, (uint32_t)width, (uint32_t)height, 0, 0};
        dispatch(g, PIPE_FINISH, &p, (uint32_t)width, (uint32_t)height);
    }
    if (gi) {
        /* what walls send on, for the next frame's gi: it stays on the GPU */
        computeBarrier(g);
        const Push2D p = {0, (uint32_t)width, (uint32_t)height, 0, (uint32_t)giW, (uint32_t)giH, (uint32_t)giScale, 0};
        dispatch(g, PIPE_REFLECT, &p, (uint32_t)giW, (uint32_t)giH);
    }
    barrier(g, STAGE_COMPUTE, STAGE_TRANSFER, ACCESS_SHADER_WRITE, ACCESS_TRANSFER_READ | ACCESS_TRANSFER_WRITE);
    if (shows) {
        /* into the window's image: as much of the frame as it holds, and black around it */
        const VkImage target = g->images[image];
        imageBarrier(g, target, LAYOUT_UNDEFINED, LAYOUT_TRANSFER_DST, STAGE_TRANSFER, STAGE_TRANSFER, 0, ACCESS_TRANSFER_WRITE);
        const uint32_t cw = (uint32_t)artfMini(width, g->swapWidth), ch = (uint32_t)artfMini(height, g->swapHeight);
        if ((cw < (uint32_t)g->swapWidth || ch < (uint32_t)g->swapHeight) && g->vkCmdClearColorImage) {
            const ClearValue black = {{0, 0, 0, 1}};
            const ImageSubresourceRange range = {ASPECT_COLOR, 0, 1, 0, 1};
            g->vkCmdClearColorImage(cb, target, LAYOUT_TRANSFER_DST, &black, 1, &range);
            barrier(g, STAGE_TRANSFER, STAGE_TRANSFER, ACCESS_TRANSFER_WRITE, ACCESS_TRANSFER_WRITE);
        }
        const BufferImageCopy region = {0, (uint32_t)width, (uint32_t)height, {ASPECT_COLOR, 0, 0, 1}, {0, 0, 0}, {cw, ch, 1}};
        g->vkCmdCopyBufferToImage(cb, g->buffers[B2_PIXELS].buffer, target, LAYOUT_TRANSFER_DST, 1, &region);
        imageBarrier(g, target, LAYOUT_TRANSFER_DST, LAYOUT_PRESENT, STAGE_TRANSFER, STAGE_BOTTOM, ACCESS_TRANSFER_WRITE, 0);
    }
    if (readBack) {
        const BufferCopy pixelCopy = {0, 0, pixelBytes};
        g->vkCmdCopyBuffer(cb, g->buffers[B2_PIXELS].buffer, slot->readback.buffer, 1, &pixelCopy);
        barrier(g, STAGE_TRANSFER, STAGE_HOST, ACCESS_TRANSFER_WRITE, ACCESS_HOST_READ);
    }
    if (g->timestamps) g->vkCmdWriteTimestamp(cb, STAGE_BOTTOM, g->timestamps, (uint32_t)slotIndex * 2 + 1);
    g->vkEndCommandBuffer(cb);

    const VkFlags waitStage = STAGE_TRANSFER;
    const SubmitInfo submit = {ST_SUBMIT_INFO, NULL, shows ? 1u : 0u, shows ? &slot->acquired : NULL, shows ? &waitStage : NULL, 1, &cb,
                               shows ? 1u : 0u, shows ? &g->rendered[image] : NULL};
    g->vkResetFences(g->device, 1, &slot->fence);
    if (g->vkQueueSubmit(g->queue, 1, &submit, slot->fence) != VK_OK) {
        artfLog("artful-2d: the Vulkan device was lost");
        return false;
    }
    slot->pending = true;
    slot->readBack = readBack;
    slot->width = width;
    slot->height = height;
    g->last = slotIndex;
    g->slot = (slotIndex + 1) % SLOTS;
    if (shows) {
        const PresentInfo present = {ST_PRESENT_INFO, NULL, 1, &g->rendered[image], 1, &g->swapchain, &image, NULL};
        const VkResult result = g->vkQueuePresentKHR(g->queue, &present);
        if (result == VK_ERR_OUT_OF_DATE || result == VK_SUBOPTIMAL) g->swapchainStale = true;
        else if (result == VK_ERR_DEVICE_LOST) {
            artfLog("artful-2d: the Vulkan device was lost");
            return false;
        }
    }
    return true;
}

bool artf2DVulkanPixels(Gpu2D* g, uint32_t* dst, int width, int height) {
    if (g->last < 0) return false;
    FrameSlot2D* s = &g->slots[g->last];
    if (s->width != width || s->height != height) return false;
    if (!slotWait(g, g->last)) return false;
    if (s->readBack) {
        memcpy(dst, s->readback.mapped, (size_t)width * height * 4);
        return true;
    }
    /* (shown, not read back: copied from the GPU now, every frame done first) */
    if (!idle(g) || !ensure(g, &s->readback, (VkDeviceSize)width * height * 4, true, USAGE_TRANSFER_DST)) return false;
    g->commands = s->commands;
    g->vkResetCommandBuffer(s->commands, 0);
    const CommandBufferBeginInfo begin = {ST_COMMAND_BUFFER_BEGIN, NULL, 1, NULL};
    g->vkBeginCommandBuffer(s->commands, &begin);
    barrier(g, STAGE_COMPUTE | STAGE_TRANSFER, STAGE_TRANSFER, ACCESS_SHADER_WRITE | ACCESS_TRANSFER_WRITE, ACCESS_TRANSFER_READ);
    const BufferCopy copy = {0, 0, (VkDeviceSize)width * height * 4};
    g->vkCmdCopyBuffer(s->commands, g->buffers[B2_PIXELS].buffer, s->readback.buffer, 1, &copy);
    barrier(g, STAGE_TRANSFER, STAGE_HOST, ACCESS_TRANSFER_WRITE, ACCESS_HOST_READ);
    g->vkEndCommandBuffer(s->commands);
    const SubmitInfo submit = {ST_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &s->commands, 0, NULL};
    g->vkResetFences(g->device, 1, &s->fence);
    if (g->vkQueueSubmit(g->queue, 1, &submit, s->fence) != VK_OK || g->vkWaitForFences(g->device, 1, &s->fence, 1, UINT64_MAX) != VK_OK) return false;
    /* (the timestamps this slot had are its frame's still: no query was written) */
    const uint32_t* from = s->readback.mapped;
    const size_t n = (size_t)width * height;
    if (!g->swapRedBlue) memcpy(dst, from, n * 4);
    else
        for (size_t i = 0; i < n; ++i) {
            const uint32_t p = from[i];
            dst[i] = (p & 0xFF00FF00u) | (p >> 16 & 255) | (p & 255) << 16;
        }
    return true;
}
