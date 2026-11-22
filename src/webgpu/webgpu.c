/* artful — the WebGPU backend: a browser's GPU (through Emscripten) or a native WebGPU (wgpu-native, Dawn), on
 * webgpu.h. Built with ARTFUL_WEBGPU; chosen only by name (ARTF_BACKEND_WEBGPU).
 *
 * Its shaders are Vulkan's, built here as SPIR-V by shaders.c, read back into the IR (irFromSpirv) and written as WGSL
 * (irWgsl): the same arithmetic as on Vulkan, and as on the CPU. Textures are read by constant indices
 * (ARTF_TEXTURES_UNIFORM), as WGSL has no arrays of them, as many as the adapter lets a stage have. Each pipeline's
 * bind group holds what its shaders say they bind, from what Vulkan binds at the same numbers.
 *
 * Its frame is Metal's (metal.c), the plain one: the scene's buffers go up as they change; each instance in view is
 * found by the cull shader, which fills each mesh's indirect draw; shadows from maps; then the scene, what has holes,
 * and the overlay, into a texture of its own, copied to the surface where there is one and read back where asked.
 * Materials of the program's own are branches of the cutout pipeline's pixel shader, made again when the scene's
 * change. What it leaves out, it says in the log: a frame in floats and what follows it, light from all around and
 * from probes, layers of transparency, tracing and views. */
#include "render/rendererInternal.h"
#include "scene/sceneInternal.h"
#include "shaderir/ir.h"
#include "shaderir/wgsl.h"
#include "vulkan/shaders.h"

#include <webgpu/webgpu.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <webgpu/wgpu.h>
#endif

#include <stdio.h>
#include <string.h>

#define BLANK_BYTES 65536
#define PUSH_STRIDE 256 /* (a dynamic uniform offset's alignment) */

typedef struct Gpu { WGPUBuffer buffer; uint64_t size; } Gpu;

/* A pipeline, and the bind group of what its shaders bind. */
typedef struct Program {
    WGPURenderPipeline render;
    WGPUComputePipeline compute;
    WGPUBindGroupLayout layout;
    WGPUPipelineLayout pipelineLayout;
    IrWgslBindings bindings;
    WGPUBindGroup group;
    uint64_t stamp; /* what its group was made for */
    bool push;      /* push constants: a dynamic offset */
    /* group 1, Vulkan's set 1: the light, the second and the history, as their roles now fall (a group for each way) */
    WGPUBindGroupLayout layout1;
    WGPUBindGroup roleGroups[27];
    uint64_t roleStamp;
} Program;

typedef struct MeshRange { uint32_t firstIndex; int32_t vertexOffset; uint32_t indexCount, colorBase; uint64_t deformVersion; } MeshRange;

typedef struct WebGpu {
    ArtfRenderer base;
    WGPUInstance instance;
    WGPUAdapter adapter;
    WGPUDevice device;
    WGPUQueue queue;
    WGPUSurface surface;
    WGPUTextureFormat surfaceFormat;
    bool surfaceReady;
    char description[300];
    int width, height, samples;
    uint32_t maxTextures;
    bool targetsStale;
    WGPUTexture frameTexture, samplesTexture, depthTexture, overlay, atlas, white;
    WGPUTextureView frameView, samplesView, depthView, overlayView, atlasView, whiteView;
    int shadowWidth, shadowHeight;
    WGPUSampler linear, nearest;
    Program cull, scene, cutout, overlayDraw, shadow, shadowHoles, blit;
    Program layerDepth, layer, particle; /* (of the cutout's shader: a layer's depth alone, then it at that depth, blended) */
    /* A frame in floats (ArtfRendererOptions.hdr), as Vulkan's: the scene's light and surface words, then the post
     * passes over storage buffers (Vulkan's bindings 50 to 57 and 69 to 74), the final pass and the overlay. */
    bool floats, wide;               /* (wide: the surface's other words, for motion and light under the surface) */
    uint32_t madeVariant;            /* what the scene's pipelines were made for: variant bits, and wide */
    bool madeWide, programsMade;
    WGPUTexture floatColor, floatSamples, floatSurface;
    WGPUTextureView floatColorView, floatSamplesView, floatSurfaceView;
    Gpu post[8], postLocks, postGathered, cloudField, cloudGrid; /* (post: 50 light, depth, surface, scratch, history, occlusion, fog, 57 bloom) */
    uint32_t postNeeds, bloomSizes[6][3];
    Program posts[22], final, overlayFloats;
    uint8_t role[3];
    bool historyValid, exposureValid, hasPrevious, gatheredValid;
    uint32_t postFrames, grainFrames, gatheredHalf, lutRoom, lockHalf;
    uint64_t lutScene, lutVersion, cloudGeneration;
    float previous[16];
    ArtfMotion motion;
    uint32_t* pushes;                /* (this frame's post passes' push constants, each at its stride) */
    uint32_t pushCount, maxGroups;
    bool warnedWide, warnedLarge;
    bool froxels;                    /* fog that light goes through: its grid, read by what is drawn */
    int sceneWidth, sceneHeight;     /* (in floats with renderScale: the scene's size, the frame's after upscaling) */
    float scale;
    Program fogBehind;               /* the fog over what is behind (the background) */
    uint32_t variantBits;            /* the scene's pixel shaders' variant past floats and fog: coats, soft shadows, shapes */
    Gpu environment;                 /* light from all around (binding 49), and what draws it behind the scene */
    uint64_t environmentVersion, environmentScene;
    Program environmentBehind;
    /* tracing (ArtfTraceDesc): the trees (17 to 21), what is gathered (22), the frame's rays (23), the filter's buffers
     * (27 to 33, 48) and how objects moved (37) */
    Gpu traceNodes, traceTriangles, traceVertices, traceTop, traceInstances, traceSum, traceFrameUbo, denoiseBuffers[7], denoiseShift, traceMotion;
    Program trace, denoisePasses[9], traceShow;
    uint64_t traceScene, traceMeshVersion, traceTopVersion, traceKey, traceProgramScene;
    uint32_t traceDone, traceFrames, traceProgramKey;
    bool denoiseFirst, warnedTraceHardware;
    /* light that bounced: the probes, a step a frame on the GPU through the trees (vulkan.c's syncIndirect) */
    Gpu probesNow, probesNext;
    Program probeStep;
    uint64_t probeKey, probeScene, probeSerial, probeGraphScene;
    uint32_t probeFrames, probeGraphs;
    /* rays while drawing: shadows (the pixel shaders' TRACED variant) and reflections (a post pass in place of the
     * screen's), through the trees */
    Program reflectionTraced;
    uint64_t reflectionScene;
    uint32_t reflectionGraphs;
    float tracePrevious[16];
    uint64_t stamp; /* the resources' version: bind groups are made again when it moves */
    double cpuMs;
    uint64_t frameNumber;
    bool warnedTrace, warnedFloats, warnedView;
    ARTF_VEC(ArtfLayer) layers; /* (scratch: what shows what is behind it, in view, far to near) */
    uint64_t graphScene;  /* the scene, and how many of its materials of its own, the cutout pipeline was made for */
    uint32_t graphCount;

    Gpu empty, emptyWritable, frame, shadowData, push, instances, visible, indirect, materials, meshTable, vertices, indices, colors;
    Gpu shadowList, shadowCommands, meshletMeshes, meshletState, extraLights, tileLights, decals;
    uint32_t* templateCpu; /* the indirect commands each frame starts from */
    size_t templateBytes;

    uint64_t sceneId, meshRemovals, textureVersion, meshTableVersion, lodVersion, structureVersion, instanceVersion, materialVersion;
    bool fresh;
    size_t meshesUp;
    ARTF_VEC(MeshRange) meshes;
    ARTF_VEC(WGPUTexture) textures;
    ARTF_VEC(WGPUTextureView) textureViews;
    ARTF_VEC(uint64_t) textureMarks;
    ARTF_VEC(uint32_t) copyStarts;
    uint32_t instanceCapacity, materialCapacity, meshCapacity;
    uint64_t shadowInstances, shadowStructure, shadowMaterials, shadowScene;
    uint32_t shadowMeshes;
    ArtfLightData* lightScratch;
    uint32_t* listScratch;
    size_t listScratchSize;
    ArtfDecalData* decalScratch;

    const uint32_t* overlayPixels;
    int overlayWidth, overlayHeight, overlayStride;
    bool overlayPending, overlayUsable;
} WebGpu;

/* ---- waiting ---- */

static bool adapterGot, deviceGot;
static void gotAdapter(WGPURequestAdapterStatus status, WGPUAdapter a, WGPUStringView message, void* user, void* unused) {
    (void)unused;
    adapterGot = true;
    if (status == WGPURequestAdapterStatus_Success) ((WebGpu*)user)->adapter = a;
    else artfLog("artful: no WebGPU adapter: %.*s", (int)message.length, message.data ? message.data : "");
}
static void gotDevice(WGPURequestDeviceStatus status, WGPUDevice d, WGPUStringView message, void* user, void* unused) {
    (void)unused;
    deviceGot = true;
    if (status == WGPURequestDeviceStatus_Success) ((WebGpu*)user)->device = d;
    else artfLog("artful: no WebGPU device: %.*s", (int)message.length, message.data ? message.data : "");
}
static void gpuError(WGPUDevice const* device, WGPUErrorType type, WGPUStringView message, void* user, void* unused) {
    (void)device; (void)user; (void)unused;
    artfLog("artful: WebGPU error %d: %.*s", (int)type, (int)message.length, message.data ? message.data : "");
}

/* Until *done: what WebGPU has to say, said. (In a browser, only where it may wait: what it cannot, it does not.) */
static bool waitFor(WebGpu* v, volatile bool* done) {
    for (int i = 0; i < 100000 && !*done; ++i) {
#ifdef __EMSCRIPTEN__
        (void)v;
        emscripten_sleep(1);
#else
        if (v->device) wgpuDevicePoll(v->device, true, NULL);
        wgpuInstanceProcessEvents(v->instance);
#endif
    }
    return *done;
}

/* ---- memory ---- */

static void bufferFree(Gpu* b) {
    if (b->buffer) wgpuBufferRelease(b->buffer);
    memset(b, 0, sizeof *b);
}

static bool bufferNew(WebGpu* v, Gpu* b, uint64_t size, WGPUBufferUsage usage) {
    bufferFree(b);
    size = (size ? size : 16) + 15 & ~(uint64_t)15;
    WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
    d.size = size;
    d.usage = usage | WGPUBufferUsage_CopyDst;
    b->buffer = wgpuDeviceCreateBuffer(v->device, &d);
    if (!b->buffer) {
        artfLog("artful: no memory for a WebGPU buffer of %llu bytes", (unsigned long long)size);
        return false;
    }
    b->size = size;
    ++v->stamp;
    return true;
}

/* Room for at least size (what it held goes). */
static bool bufferRoom(WebGpu* v, Gpu* b, uint64_t size, WGPUBufferUsage usage) { return b->buffer && size <= b->size ? true : bufferNew(v, b, size * 2, usage); }

/* (WebGPU writes whole words: what is not, goes up padded) */
static void bufferPut(WebGpu* v, Gpu* b, uint64_t offset, const void* data, size_t size) {
    if (!size) return;
    if (size % 4) {
        const size_t whole = (size + 3) & ~(size_t)3;
        char* padded = artfCalloc(whole, 1);
        memcpy(padded, data, size);
        wgpuQueueWriteBuffer(v->queue, b->buffer, offset, padded, whole);
        artfFree(padded);
        return;
    }
    wgpuQueueWriteBuffer(v->queue, b->buffer, offset, data, size);
}

static const WGPUBufferUsage STORAGE = WGPUBufferUsage_Storage, UNIFORM = WGPUBufferUsage_Uniform;

static WGPUTexture textureNew(WebGpu* v, WGPUTextureFormat format, int width, int height, uint32_t mips, int samples, WGPUTextureUsage usage) {
    WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;
    d.size.width = (uint32_t)width;
    d.size.height = (uint32_t)height;
    d.size.depthOrArrayLayers = 1;
    d.format = format;
    d.mipLevelCount = mips;
    d.sampleCount = (uint32_t)samples;
    d.usage = usage;
    return wgpuDeviceCreateTexture(v->device, &d);
}
static WGPUTextureView viewOf(WGPUTexture t) { return t ? wgpuTextureCreateView(t, NULL) : NULL; }

static void texturePut(WebGpu* v, WGPUTexture t, uint32_t level, int x, int y, int w, int h, const void* pixels, int stride) {
    WGPUTexelCopyTextureInfo to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    to.texture = t;
    to.mipLevel = level;
    to.origin.x = (uint32_t)x;
    to.origin.y = (uint32_t)y;
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = (uint32_t)stride * 4;
    layout.rowsPerImage = (uint32_t)h;
    const WGPUExtent3D size = {(uint32_t)w, (uint32_t)h, 1};
    wgpuQueueWriteTexture(v->queue, &to, pixels, (size_t)stride * 4 * (size_t)(h - 1) + (size_t)w * 4, &layout, &size);
}

/* ---- shaders ---- */

/* A shader of shaders.c's, as WGSL, and what it binds. */
static char* wgslOf(uint32_t* code, size_t bytes, bool flipY, IrWgslBindings* bindings) {
    if (!code) return NULL;
    IrModule m;
    irInit(&m);
    const bool read = irFromSpirv(&m, code, bytes / 4);
    artfFree(code);
    char why[256] = "not SPIR-V";
    const IrWgslOptions options = {flipY};
    char* text = read ? irWgsl(&m, &options, bindings, why, sizeof why) : NULL;
    if (!text) artfLog("artful: a shader could not be written for WebGPU: %s", why);
    irFree(&m);
    return text;
}

static WGPUShaderModule moduleOf(WebGpu* v, const char* text) {
    WGPUShaderSourceWGSL source = WGPU_SHADER_SOURCE_WGSL_INIT;
    source.code.data = text;
    source.code.length = strlen(text);
    WGPUShaderModuleDescriptor d = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    d.nextInChain = &source.chain;
    return wgpuDeviceCreateShaderModule(v->device, &d);
}

static void bindingsJoin(IrWgslBindings* into, const IrWgslBindings* more) {
    for (int k = 0; k < more->count; ++k) {
        bool there = false;
        for (int j = 0; j < into->count && !there; ++j) there = into->items[j].binding == more->items[k].binding && into->items[j].group == more->items[k].group;
        if (!there && into->count < IR_WGSL_BINDINGS) into->items[into->count++] = more->items[k];
    }
}

/* What Vulkan's binding is here: a buffer (and how much of it), a texture's view or a sampler. */
static Gpu* bufferAt(WebGpu* v, uint32_t source, bool writable) {
    switch (source) {
    case 0: return &v->frame;
    case 1: return &v->instances;
    case 2: return &v->visible;
    case 3: return &v->meshTable;
    case 4: return &v->materials;
    case 5: return &v->indirect;
    case 16: return &v->colors;
    case 24: return &v->shadowData;
    case 26: return &v->shadowList;
    case 41: return &v->meshletMeshes;
    case 43: return &v->meshletState;
    case 60: return &v->extraLights;
    case 61: return &v->tileLights;
    case 63: return &v->decals;
    case 17: return &v->traceNodes;
    case 18: return &v->traceTriangles;
    case 19: return &v->traceVertices;
    case 20: return &v->traceTop;
    case 21: return &v->traceInstances;
    case 22: return &v->traceSum;
    case 23: return &v->traceFrameUbo;
    case 27: case 28: case 29: case 30: case 31: case 32: case 33: return &v->denoiseBuffers[source - 27];
    case 37: return &v->traceMotion;
    case 48: return &v->denoiseShift;
    case 62: return &v->probesNow;
    case 70: return &v->probesNext;
    case 49: return &v->environment;
    case 50: case 51: case 52: case 53: case 54: case 55: case 56: case 57: return &v->post[source - 50];
    case 69: return v->postNeeds & 2 /* NEED_BLUR */ ? &v->post[5] : &v->empty;
    case 71: return v->postLocks.buffer ? &v->postLocks : &v->emptyWritable;
    case 72: return &v->postGathered;
    case 73: return &v->cloudField;
    case 74: return &v->cloudGrid;
    case IR_WGSL_PUSH_AT: return &v->push;
    default: return writable ? &v->emptyWritable : &v->empty;
    }
}
/* (and where it is not there yet, nothing: as long as the largest block a shader reads) */
static Gpu* bufferOrEmpty(WebGpu* v, uint32_t source, bool writable) {
    Gpu* g = bufferAt(v, source, writable);
    return g->buffer ? g : writable ? &v->emptyWritable : &v->empty;
}
static WGPUTextureView textureAt(WebGpu* v, uint32_t source, uint32_t element) {
    switch (source) {
    case 6: return element < v->textureViews.len && v->textureViews.items[element] ? v->textureViews.items[element] : v->whiteView;
    case 7: return v->overlayView ? v->overlayView : v->whiteView;
    case 8: return v->depthView;
    case 25: return v->atlasView;
    case 58: return v->floatColorView ? v->floatColorView : v->whiteView;
    case 59: return v->floatSurfaceView;
    default: return v->whiteView;
    }
}

/* The layout, from what the shaders bind (compute: their stage; else the vertex's and the pixel's). */
/* (what is read as it is, never filtered: depth, the shadow atlas, the surface words) */
static bool unfiltered(uint32_t source) { return source == 8 || source == 25 || source == 59; }

static bool layoutMake(WebGpu* v, Program* p, bool compute) {
    WGPUBindGroupLayoutEntry entries[IR_WGSL_BINDINGS], entries1[4];
    int count = 0, count1 = 0;
    for (int k = 0; k < p->bindings.count; ++k) {
        const IrWgslBinding* b = &p->bindings.items[k];
        WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        e.binding = b->binding;
        e.visibility = compute ? WGPUShaderStage_Compute : WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
        switch (b->kind) {
        case IR_WGSL_UNIFORM:
            e.buffer.type = WGPUBufferBindingType_Uniform;
            if (b->source == IR_WGSL_PUSH_AT) {
                e.buffer.hasDynamicOffset = true;
                p->push = true;
            }
            break;
        case IR_WGSL_STORAGE_READ: e.buffer.type = WGPUBufferBindingType_ReadOnlyStorage; break;
        case IR_WGSL_STORAGE:
            e.buffer.type = WGPUBufferBindingType_Storage;
            if (!compute) e.visibility = WGPUShaderStage_Fragment; /* (a vertex shader writes nothing) */
            break;
        case IR_WGSL_SAMPLER: e.sampler.type = unfiltered(b->source) ? WGPUSamplerBindingType_NonFiltering : WGPUSamplerBindingType_Filtering; break;
        default:
            e.texture.sampleType = b->kind == IR_WGSL_TEXTURE_UINT ? WGPUTextureSampleType_Uint : unfiltered(b->source) ? WGPUTextureSampleType_UnfilterableFloat : WGPUTextureSampleType_Float;
            e.texture.viewDimension = WGPUTextureViewDimension_2D;
            e.texture.multisampled = b->kind == IR_WGSL_TEXTURE_MULTISAMPLED;
            break;
        }
        if (b->group == 1 && count1 < 4) entries1[count1++] = e;
        else entries[count++] = e;
    }
    WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    d.entryCount = (size_t)count;
    d.entries = entries;
    p->layout = wgpuDeviceCreateBindGroupLayout(v->device, &d);
    WGPUBindGroupLayout layouts[2] = {p->layout, NULL};
    if (count1) {
        WGPUBindGroupLayoutDescriptor d1 = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        d1.entryCount = (size_t)count1;
        d1.entries = entries1;
        layouts[1] = p->layout1 = wgpuDeviceCreateBindGroupLayout(v->device, &d1);
    }
    WGPUPipelineLayoutDescriptor pd = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pd.bindGroupLayoutCount = count1 ? 2 : 1;
    pd.bindGroupLayouts = layouts;
    p->pipelineLayout = wgpuDeviceCreatePipelineLayout(v->device, &pd);
    return p->layout && p->pipelineLayout && (!count1 || p->layout1);
}

/* Its bind group, as the resources now are. */
static WGPUBindGroup groupOf(WebGpu* v, Program* p) {
    if (p->group && p->stamp == v->stamp) return p->group;
    if (p->group) wgpuBindGroupRelease(p->group);
    WGPUBindGroupEntry entries[IR_WGSL_BINDINGS];
    int count = 0;
    for (int k = 0; k < p->bindings.count; ++k) {
        const IrWgslBinding* b = &p->bindings.items[k];
        if (b->group == 1) continue;
        WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
        e.binding = b->binding;
        switch (b->kind) {
        case IR_WGSL_UNIFORM: case IR_WGSL_STORAGE_READ: case IR_WGSL_STORAGE: {
            const Gpu* g = bufferOrEmpty(v, b->source, b->kind == IR_WGSL_STORAGE);
            e.buffer = g->buffer;
            e.size = b->source == IR_WGSL_PUSH_AT ? 16 : g->size;
            break;
        }
        case IR_WGSL_SAMPLER: e.sampler = unfiltered(b->source) || b->source == 58 ? v->nearest : v->linear; break;
        default: e.textureView = textureAt(v, b->source, b->element); break;
        }
        entries[count++] = e;
    }
    WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    d.layout = p->layout;
    d.entryCount = (size_t)count;
    d.entries = entries;
    p->group = wgpuDeviceCreateBindGroup(v->device, &d);
    p->stamp = v->stamp;
    return p->group;
}

/* Its group 1, as the light buffers' roles now are (vulkan.c's roleSets): binding 0 the light, 1 the second, 2 the
 * history, each one of the three buffers (the light's own, the scratch, the history). */
static WGPUBindGroup rolesGroupOf(WebGpu* v, Program* p) {
    if (!p->layout1) return NULL;
    if (p->roleStamp != v->stamp) {
        for (int k = 0; k < 27; ++k)
            if (p->roleGroups[k]) wgpuBindGroupRelease(p->roleGroups[k]), p->roleGroups[k] = NULL;
        p->roleStamp = v->stamp;
    }
    const int key = v->role[0] * 9 + v->role[1] * 3 + v->role[2];
    if (p->roleGroups[key]) return p->roleGroups[key];
    Gpu* const three[3] = {&v->post[0], v->post[3].buffer ? &v->post[3] : &v->emptyWritable, v->post[4].buffer ? &v->post[4] : &v->emptyWritable};
    WGPUBindGroupEntry entries[4];
    int count = 0;
    for (int k = 0; k < p->bindings.count; ++k) {
        const IrWgslBinding* b = &p->bindings.items[k];
        if (b->group != 1 || b->binding > 2 || count == 4) continue;
        WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
        e.binding = b->binding;
        const Gpu* g = three[v->role[b->binding]];
        e.buffer = g->buffer;
        e.size = g->size;
        entries[count++] = e;
    }
    WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    d.layout = p->layout1;
    d.entryCount = (size_t)count;
    d.entries = entries;
    return p->roleGroups[key] = wgpuDeviceCreateBindGroup(v->device, &d);
}

static bool computeMake(WebGpu* v, Program* p, char* text) {
    if (!text) return false;
    WGPUShaderModule module = moduleOf(v, text);
    artfFree(text);
    if (!module || !layoutMake(v, p, true)) return false;
    WGPUComputePipelineDescriptor d = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    d.layout = p->pipelineLayout;
    d.compute.module = module;
    d.compute.entryPoint.data = "main";
    d.compute.entryPoint.length = 4;
    p->compute = wgpuDeviceCreateComputePipeline(v->device, &d);
    wgpuShaderModuleRelease(module);
    return p->compute != NULL;
}

typedef struct RenderState {
    WGPUCompareFunction depth;
    bool depthWrite, blend, color, vertices, shadow;
    WGPUCullMode cull;
    WGPUTextureFormat target; /* (0: the frame's) */
    WGPUTextureFormat surface; /* (in floats: the surface words' target, written by what is solid; 0: none) */
    bool surfaceWrite;
    bool fog; /* (blended as fog over what is behind: light * what is left + added * what shows of it, alpha kept) */
} RenderState;

static bool renderMake(WebGpu* v, Program* p, char* vertexText, char* fragmentText, const RenderState* st) {
    if (!vertexText) { artfFree(fragmentText); return false; }
    WGPUShaderModule vertex = moduleOf(v, vertexText), fragment = fragmentText ? moduleOf(v, fragmentText) : NULL;
    artfFree(vertexText);
    artfFree(fragmentText);
    if (!vertex || !layoutMake(v, p, false)) return false;
    WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    d.layout = p->pipelineLayout;
    d.vertex.module = vertex;
    d.vertex.entryPoint.data = "main";
    d.vertex.entryPoint.length = 4;
    /* the vertices, as ArtfPackedVertex packs them (as Metal's: metal.c) */
    WGPUVertexAttribute attributes[3] = {WGPU_VERTEX_ATTRIBUTE_INIT, WGPU_VERTEX_ATTRIBUTE_INIT, WGPU_VERTEX_ATTRIBUTE_INIT};
    attributes[0].format = WGPUVertexFormat_Unorm16x4;
    attributes[0].offset = 0;
    attributes[0].shaderLocation = 0;
    attributes[1].format = WGPUVertexFormat_Snorm16x2;
    attributes[1].offset = 8;
    attributes[1].shaderLocation = 1;
    attributes[2].format = WGPUVertexFormat_Float16x2;
    attributes[2].offset = 12;
    attributes[2].shaderLocation = 2;
    WGPUVertexBufferLayout layout = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    layout.stepMode = WGPUVertexStepMode_Vertex;
    layout.arrayStride = sizeof(ArtfPackedVertex);
    layout.attributeCount = 3;
    layout.attributes = attributes;
    if (st->vertices) {
        d.vertex.bufferCount = 1;
        d.vertex.buffers = &layout;
    }
    d.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    d.primitive.frontFace = WGPUFrontFace_CCW;
    d.primitive.cullMode = st->cull;
    WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
    if (st->target == 0 || st->shadow || st->surface) {
        depth.format = WGPUTextureFormat_Depth32Float;
        depth.depthCompare = st->depth;
        depth.depthWriteEnabled = st->depthWrite ? WGPUOptionalBool_True : WGPUOptionalBool_False;
        d.depthStencil = &depth;
    }
    d.multisample.count = st->shadow || (st->target && !st->surface) ? 1u : (uint32_t)v->samples;
    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    blend.color.srcFactor = blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.color.dstFactor = blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.color.operation = blend.alpha.operation = WGPUBlendOperation_Add;
    if (st->fog) {
        blend.color.srcFactor = WGPUBlendFactor_DstAlpha;
        blend.color.dstFactor = WGPUBlendFactor_SrcAlpha;
        blend.alpha.srcFactor = WGPUBlendFactor_Zero;
        blend.alpha.dstFactor = WGPUBlendFactor_One;
    }
    WGPUColorTargetState targets[2] = {WGPU_COLOR_TARGET_STATE_INIT, WGPU_COLOR_TARGET_STATE_INIT};
    WGPUColorTargetState* const target = &targets[0];
    target->format = st->target ? st->target : WGPUTextureFormat_BGRA8Unorm;
    target->writeMask = st->color ? WGPUColorWriteMask_All : WGPUColorWriteMask_None;
    if (st->blend) target->blend = &blend;
    targets[1].format = st->surface;
    targets[1].writeMask = st->surfaceWrite ? WGPUColorWriteMask_All : WGPUColorWriteMask_None;
    WGPUFragmentState fragmentState = WGPU_FRAGMENT_STATE_INIT;
    if (fragment || !st->shadow) {
        fragmentState.module = fragment;
        fragmentState.entryPoint.data = "main";
        fragmentState.entryPoint.length = 4;
        if (!st->shadow) {
            fragmentState.targetCount = st->surface ? 2 : 1;
            fragmentState.targets = targets;
        }
        if (fragment) d.fragment = &fragmentState;
    }
    p->render = wgpuDeviceCreateRenderPipeline(v->device, &d);
    wgpuShaderModuleRelease(vertex);
    if (fragment) wgpuShaderModuleRelease(fragment);
    return p->render != NULL;
}

/* The frame, as it is, onto the surface (whose format may not be the frame's): one triangle over it. */
static const char blitSource[] =
    "@group(0) @binding(0) var frame: texture_2d<f32>;\n"
    "@vertex fn vertexMain(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {\n"
    "    let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));\n"
    "    return vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n"
    "@fragment fn main(@builtin(position) at: vec4<f32>) -> @location(0) vec4<f32> {\n"
    "    return textureLoad(frame, vec2<i32>(at.xy), 0);\n"
    "}\n";

static bool blitMake(WebGpu* v) {
    char* text = artfMalloc(sizeof blitSource);
    memcpy(text, blitSource, sizeof blitSource);
    WGPUShaderModule module = moduleOf(v, text);
    artfFree(text);
    if (!module) return false;
    Program* p = &v->blit;
    p->bindings.count = 1;
    const IrWgslBinding frame = {0, 0, IR_WGSL_TEXTURE, 9999, 0};
    p->bindings.items[0] = frame;
    WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    e.binding = 0;
    e.visibility = WGPUShaderStage_Fragment;
    e.texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    e.texture.viewDimension = WGPUTextureViewDimension_2D;
    WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    ld.entryCount = 1;
    ld.entries = &e;
    p->layout = wgpuDeviceCreateBindGroupLayout(v->device, &ld);
    WGPUPipelineLayoutDescriptor pd = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pd.bindGroupLayoutCount = 1;
    pd.bindGroupLayouts = &p->layout;
    p->pipelineLayout = wgpuDeviceCreatePipelineLayout(v->device, &pd);
    WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    d.layout = p->pipelineLayout;
    d.vertex.module = module;
    d.vertex.entryPoint.data = "vertexMain";
    d.vertex.entryPoint.length = 10;
    d.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = v->surfaceFormat;
    WGPUFragmentState f = WGPU_FRAGMENT_STATE_INIT;
    f.module = module;
    f.entryPoint.data = "main";
    f.entryPoint.length = 4;
    f.targetCount = 1;
    f.targets = &target;
    d.fragment = &f;
    p->render = wgpuDeviceCreateRenderPipeline(v->device, &d);
    wgpuShaderModuleRelease(module);
    return p->render != NULL;
}

/* The pipeline of what has holes, and of materials of the program's own (their graphs, one branch each). */
static void programFree(Program* p);

/* The scene's pipelines, as Vulkan's: what is solid; and of the cutout shader (holes, and materials of the program's
 * own, one branch each), what has holes, a layer's depth alone, the layer at that depth blended, and particles. In
 * floats, each also writes the surface words (what is solid only), and with wide, the vertices' motion. */
static bool scenePrograms(WebGpu* v, const ArtfGraphData* graphs, uint32_t count) {
    const uint32_t variant = (v->floats ? ARTF_SHADER_FLOATS : 0u) | (v->froxels ? ARTF_SHADER_FROXELS : 0u) | v->variantBits, textures = v->maxTextures | ARTF_TEXTURES_UNIFORM;
    const WGPUTextureFormat light = v->floats ? WGPUTextureFormat_RGBA16Float : 0;
    const WGPUTextureFormat surface = !v->floats ? 0 : v->wide ? WGPUTextureFormat_RGBA32Uint : WGPUTextureFormat_R32Uint;
    size_t size = 0;
    IrWgslBindings a, b;
    char* text[3] = {NULL, NULL, NULL}; /* (the vertex shader; the plain pixel shader; the cutout's) */
    IrWgslBindings plainBindings, cutoutBindings;
    uint32_t* code = artfShaderSceneVertex(&size, v->floats); /* (in floats the pixel shader reads where each vertex was) */
    text[0] = wgslOf(code, size, false, &a);
    b.count = 0;
    code = artfShaderSceneFragment(&size, textures, false, NULL, 0, 0, variant);
    text[1] = wgslOf(code, size, false, &b);
    plainBindings = a;
    bindingsJoin(&plainBindings, &b);
    b.count = 0;
    code = artfShaderSceneFragment(&size, textures, true, graphs, count, 0, variant);
    text[2] = wgslOf(code, size, false, &b);
    cutoutBindings = a;
    bindingsJoin(&cutoutBindings, &b);
    bool ok = text[0] && text[1] && text[2];
    Program* const programs[5] = {&v->scene, &v->cutout, &v->layerDepth, &v->layer, &v->particle};
    const RenderState states[5] = {{WGPUCompareFunction_Greater, true, false, true, true, false, WGPUCullMode_Back, light, surface, true},
                                   {WGPUCompareFunction_Greater, true, false, true, true, false, WGPUCullMode_None, light, surface, true},
                                   {WGPUCompareFunction_Greater, true, false, false, true, false, WGPUCullMode_None, light, surface, false},
                                   {WGPUCompareFunction_Equal, false, true, true, true, false, WGPUCullMode_None, light, surface, false},
                                   {WGPUCompareFunction_Greater, true, true, true, true, false, WGPUCullMode_None, light, surface, false}};
    for (int k = 0; k < 5 && ok; ++k) {
        programFree(programs[k]);
        programs[k]->bindings = k ? cutoutBindings : plainBindings;
        const char* fragment = text[k ? 2 : 1];
        const size_t vl = strlen(text[0]) + 1, fl = strlen(fragment) + 1;
        char* vc = artfMalloc(vl);
        char* fc = artfMalloc(fl);
        memcpy(vc, text[0], vl);
        memcpy(fc, fragment, fl);
        ok = renderMake(v, programs[k], vc, fc, &states[k]);
    }
    for (int k = 0; k < 3; ++k) artfFree(text[k]);
    if (ok && v->floats && !v->fogBehind.render) {
        /* (the fog over what is behind: in the scene's pass, a triangle over it, no depth written) */
        code = artfShaderOverlayVertex(&size);
        char* vt = wgslOf(code, size, false, &a);
        code = artfShaderFogBackdrop(&size);
        b.count = 0;
        char* ft = wgslOf(code, size, false, &b);
        v->fogBehind.bindings = a;
        bindingsJoin(&v->fogBehind.bindings, &b);
        const RenderState st = {WGPUCompareFunction_Always, false, true, true, false, false, WGPUCullMode_None, light, surface, false, true};
        ok = vt && ft && renderMake(v, &v->fogBehind, vt, ft, &st);
    }
    return ok;
}

static bool postProgramsMake(WebGpu* v);

static bool pipelinesMake(WebGpu* v) {
    WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sd.addressModeU = sd.addressModeV = WGPUAddressMode_Repeat;
    sd.magFilter = sd.minFilter = WGPUFilterMode_Linear;
    sd.mipmapFilter = WGPUMipmapFilterMode_Linear;
    v->linear = wgpuDeviceCreateSampler(v->device, &sd);
    WGPUSamplerDescriptor nd = WGPU_SAMPLER_DESCRIPTOR_INIT;
    v->nearest = wgpuDeviceCreateSampler(v->device, &nd);

    const uint32_t textures = v->maxTextures | ARTF_TEXTURES_UNIFORM;
    size_t size = 0;
    IrWgslBindings a, b;
    uint32_t* code = artfShaderCull(&size, ARTF_CULL_ALL); /* (made before its size is read) */
    char* cull = wgslOf(code, size, false, &v->cull.bindings);
    if (!computeMake(v, &v->cull, cull)) return false;
#define RENDER(program, vertexCall, vertexFlip, fragmentCall, ...) do { \
        code = vertexCall; \
        char* vt = wgslOf(code, size, vertexFlip, &a); \
        b.count = 0; \
        char* ft = NULL; \
        code = fragmentCall; \
        if (code) { ft = wgslOf(code, size, false, &b); if (!ft) { artfFree(vt); return false; } } \
        (program)->bindings = a; \
        bindingsJoin(&(program)->bindings, &b); \
        const RenderState st = __VA_ARGS__; \
        if (!renderMake(v, program, vt, ft, &st)) return false; \
    } while (0)
    RENDER(&v->overlayDraw, artfShaderOverlayVertex(&size), false, artfShaderOverlayFragment(&size, false), {WGPUCompareFunction_Always, false, true, true, false, false, WGPUCullMode_None, 0});
    RENDER(&v->shadow, artfShaderShadowVertex(&size), true, (uint32_t*)NULL, {WGPUCompareFunction_Less, true, false, false, true, true, WGPUCullMode_None, 0});
    RENDER(&v->shadowHoles, artfShaderShadowVertex(&size), true, artfShaderShadowFragment(&size, textures), {WGPUCompareFunction_Less, true, false, false, true, true, WGPUCullMode_None, 0});
#undef RENDER
    if (v->floats && !postProgramsMake(v)) return false;
    return !v->surface || blitMake(v);
}

/* ---- what is drawn into ---- */

static void releaseTexture(WGPUTexture* t, WGPUTextureView* view) {
    if (*view) wgpuTextureViewRelease(*view);
    if (*t) wgpuTextureRelease(*t);
    *t = NULL;
    *view = NULL;
}

static void targetsFree(WebGpu* v) {
    releaseTexture(&v->frameTexture, &v->frameView);
    releaseTexture(&v->samplesTexture, &v->samplesView);
    releaseTexture(&v->depthTexture, &v->depthView);
    releaseTexture(&v->overlay, &v->overlayView);
    v->overlayUsable = false;
    ++v->stamp;
    releaseTexture(&v->floatColor, &v->floatColorView);
    releaseTexture(&v->floatSamples, &v->floatSamplesView);
    releaseTexture(&v->floatSurface, &v->floatSurfaceView);
}

static bool postBuffersMake(WebGpu* v);

static bool targetsMake(WebGpu* v) {
    targetsFree(v);
    v->targetsStale = false;
    if (v->width <= 0 || v->height <= 0) return true;
    v->frameTexture = textureNew(v, WGPUTextureFormat_BGRA8Unorm, v->width, v->height, 1, 1, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc | WGPUTextureUsage_TextureBinding);
    /* (with renderScale, in floats, the scene is drawn smaller, and made whole at the frame's size after it) */
    v->sceneWidth = v->floats ? artfMaxi((int)((float)v->width * v->scale + .5f), 1) : v->width;
    v->sceneHeight = v->floats ? artfMaxi((int)((float)v->height * v->scale + .5f), 1) : v->height;
    v->depthTexture = textureNew(v, WGPUTextureFormat_Depth32Float, v->sceneWidth, v->sceneHeight, 1, v->samples, WGPUTextureUsage_RenderAttachment | (v->floats ? WGPUTextureUsage_TextureBinding : 0));
    if (v->floats) {
        /* the light (resolved, where there are samples, into one that the gather reads) and the surface words */
        v->floatColor = textureNew(v, WGPUTextureFormat_RGBA16Float, v->sceneWidth, v->sceneHeight, 1, 1, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding);
        if (v->samples > 1) v->floatSamples = textureNew(v, WGPUTextureFormat_RGBA16Float, v->sceneWidth, v->sceneHeight, 1, v->samples, WGPUTextureUsage_RenderAttachment);
        v->floatSurface = textureNew(v, v->wide ? WGPUTextureFormat_RGBA32Uint : WGPUTextureFormat_R32Uint, v->sceneWidth, v->sceneHeight, 1, v->samples, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding);
        v->floatColorView = viewOf(v->floatColor);
        v->floatSamplesView = viewOf(v->floatSamples);
        v->floatSurfaceView = viewOf(v->floatSurface);
        if (!v->floatColor || !v->floatSurface || !postBuffersMake(v)) return false;
    }
    if (v->samples > 1) v->samplesTexture = textureNew(v, WGPUTextureFormat_BGRA8Unorm, v->width, v->height, 1, v->samples, WGPUTextureUsage_RenderAttachment);
    v->overlay = textureNew(v, WGPUTextureFormat_BGRA8Unorm, v->width, v->height, 1, 1, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst);
    v->frameView = viewOf(v->frameTexture);
    v->depthView = viewOf(v->depthTexture);
    v->samplesView = viewOf(v->samplesTexture);
    v->overlayView = viewOf(v->overlay);
    if (v->surface) {
        WGPUSurfaceConfiguration c = WGPU_SURFACE_CONFIGURATION_INIT;
        c.device = v->device;
        c.format = v->surfaceFormat;
        c.usage = WGPUTextureUsage_RenderAttachment;
        c.width = (uint32_t)v->width;
        c.height = (uint32_t)v->height;
        c.presentMode = WGPUPresentMode_Fifo;
        c.alphaMode = WGPUCompositeAlphaMode_Opaque;
        wgpuSurfaceConfigure(v->surface, &c);
        v->surfaceReady = true;
    }
    ++v->stamp;
    return v->frameTexture && v->depthTexture && v->overlay;
}

/* ---- the scene ---- */

static bool uploadTexture(WebGpu* v, ArtfTextureData* t, uint32_t index) {
    if (t->format) artfTextureDecode(t);
    const int first = artfMini(t->residentMip, t->mipCount - 1);
    int width = t->width, height = t->height;
    for (int level = 0; level < first; ++level) {
        width = artfMaxi(width / 2, 1);
        height = artfMaxi(height / 2, 1);
    }
    WGPUTexture image = textureNew(v, WGPUTextureFormat_BGRA8Unorm, width, height, (uint32_t)(t->mipCount - first), 1, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst);
    if (!image) return false;
    for (int level = first, w = width, h = height; level < t->mipCount; ++level) {
        texturePut(v, image, (uint32_t)(level - first), 0, 0, w, h, t->pixels + t->offsets[level], w);
        w = artfMaxi(w / 2, 1);
        h = artfMaxi(h / 2, 1);
    }
    const uint64_t mark = (uint64_t)t->generation << 8 | (uint64_t)first;
    while (v->textures.len <= index) {
        artfVecPush(&v->textures, (WGPUTexture)NULL);
        artfVecPush(&v->textureViews, (WGPUTextureView)NULL);
        artfVecPush(&v->textureMarks, (uint64_t)~0ull);
    }
    releaseTexture(&v->textures.items[index], &v->textureViews.items[index]);
    v->textures.items[index] = image;
    v->textureViews.items[index] = viewOf(image);
    v->textureMarks.items[index] = mark;
    ++v->stamp;
    return true;
}

/* All meshes go up together when one comes or goes, as Metal's do. */
static bool syncMeshes(WebGpu* v, ArtfScene* scene) {
    size_t vertices = 0, indices = 0, colors = 1;
    for (size_t i = 0; i < scene->meshes.len; ++i) {
        const ArtfMeshData* m = &scene->meshes.items[i];
        if (m->removed) continue;
        vertices += m->vertexCount;
        indices += m->indexCount;
        if (m->colors) colors += m->vertexCount;
    }
    if (!bufferRoom(v, &v->vertices, vertices * sizeof(ArtfPackedVertex), WGPUBufferUsage_Vertex)) return false;
    if (!bufferRoom(v, &v->indices, indices * 4, WGPUBufferUsage_Index)) return false;
    if (!bufferRoom(v, &v->colors, colors * 4, STORAGE)) return false;
    v->meshes.len = 0;
    uint32_t atVertex = 0, atIndex = 0, atColor = 1;
    const uint32_t white = ~(uint32_t)0; /* (what a mesh without colors reads) */
    bufferPut(v, &v->colors, 0, &white, 4);
    for (size_t i = 0; i < scene->meshes.len; ++i) {
        const ArtfMeshData* m = &scene->meshes.items[i];
        MeshRange range = {0, 0, 0, 0, m->skin ? m->pose.applied : m->deformVersion};
        if (m->vertexCount && !m->removed && !m->evicted) {
            bufferPut(v, &v->vertices, (uint64_t)atVertex * sizeof(ArtfPackedVertex), m->vertices, (size_t)m->vertexCount * sizeof(ArtfPackedVertex));
            bufferPut(v, &v->indices, (uint64_t)atIndex * 4, m->indices, (size_t)m->indexCount * 4);
            range.firstIndex = atIndex;
            range.vertexOffset = (int32_t)atVertex;
            range.indexCount = m->indexCount;
            if (m->colors) {
                bufferPut(v, &v->colors, (uint64_t)atColor * 4, m->colors, (size_t)m->vertexCount * 4);
                range.colorBase = atColor;
                atColor += m->vertexCount;
            }
            atVertex += m->vertexCount;
            atIndex += m->indexCount;
        }
        artfVecPush(&v->meshes, range);
    }
    v->meshesUp = scene->meshes.len;
    v->meshRemovals = scene->meshRemovals;
    v->meshTableVersion = ~(uint64_t)0;
    v->structureVersion = ~(uint64_t)0;
    if (v->meshes.len > v->meshCapacity) {
        v->meshCapacity = (uint32_t)v->meshes.len * 2;
        if (!bufferNew(v, &v->meshTable, (uint64_t)v->meshCapacity * 80, STORAGE)) return false;
        if (!bufferNew(v, &v->indirect, (uint64_t)v->meshCapacity * 80, STORAGE | WGPUBufferUsage_Indirect)) return false;
        artfFree(v->templateCpu);
        v->templateBytes = (size_t)v->meshCapacity * 80;
        v->templateCpu = artfCalloc(v->templateBytes, 1);
        /* (the cull shader asks of meshlets, and is told none) */
        if (!bufferNew(v, &v->meshletMeshes, (uint64_t)v->meshCapacity * 16, STORAGE)) return false;
        if (!bufferNew(v, &v->meshletState, 64, STORAGE)) return false;
        uint8_t* zeros = artfCalloc((size_t)v->meshCapacity * 16, 1);
        bufferPut(v, &v->meshletMeshes, 0, zeros, (size_t)v->meshCapacity * 16);
        bufferPut(v, &v->meshletState, 0, zeros, 64);
        artfFree(zeros);
    }
    return true;
}

static uint32_t layersAt(const WebGpu* v) { return v->instanceCapacity * (ARTF_MAX_LODS + 1) * 4; }

static bool syncScene(WebGpu* v, ArtfScene* scene) {
    if (v->sceneId != scene->id) {
        v->sceneId = scene->id;
        for (size_t i = 0; i < v->textures.len; ++i) releaseTexture(&v->textures.items[i], &v->textureViews.items[i]);
        v->textures.len = v->textureViews.len = v->textureMarks.len = 0;
        v->meshes.len = 0;
        v->meshesUp = ~(size_t)0;
        v->textureVersion = ~scene->textureVersion;
        v->fresh = true;
        ++v->stamp;
    }
    if (v->meshesUp != scene->meshes.len || v->meshRemovals != scene->meshRemovals) {
        if (!syncMeshes(v, scene)) return false;
        v->fresh = true;
    }
    if (v->meshTableVersion != scene->meshes.len + scene->lodVersion) {
        v->meshTableVersion = scene->meshes.len + scene->lodVersion;
        float* table = artfCalloc(scene->meshes.len + 1, 80);
        float* out = table;
        for (size_t i = 0; i < scene->meshes.len; ++i, out += 20) {
            const ArtfMeshData* m = &scene->meshes.items[i];
            float row[20] = {m->center[0], m->center[1], m->center[2], m->radius, m->scale[0], m->scale[1], m->scale[2], 0, m->offset[0], m->offset[1], m->offset[2], 0};
            memcpy(&row[7], &v->meshes.items[i].colorBase, 4);
            memcpy(&row[11], &v->meshes.items[i].vertexOffset, 4);
            for (uint32_t k = 0; k < ARTF_MAX_LODS; ++k) {
                const uint32_t mesh = k < m->lodCount ? m->lods[k] : 0;
                memcpy(&row[12 + k], &mesh, 4);
                row[16 + k] = k < m->lodCount ? artfMeshLevelError(m, k) : -1.f;
            }
            row[19] = m->dagBounds ? 1.f : 0.f;
            memcpy(out, row, sizeof row);
        }
        bufferPut(v, &v->meshTable, 0, table, scene->meshes.len * 80);
        artfFree(table);
        v->fresh = true;
    }
    const size_t textures = scene->textures.len < v->maxTextures ? scene->textures.len : v->maxTextures;
    if (v->textureVersion != scene->textureVersion || textures > v->textures.len) {
        v->textureVersion = scene->textureVersion;
        for (size_t i = 0; i < textures; ++i) {
            ArtfTextureData* t = &scene->textures.items[i];
            const uint64_t mark = (uint64_t)t->generation << 8 | (uint64_t)artfMini(t->residentMip, artfMaxi(t->mipCount - 1, 0));
            if (i < v->textures.len && v->textureMarks.items[i] == mark) continue;
            if (t->removed) {
                if (i < v->textures.len) {
                    releaseTexture(&v->textures.items[i], &v->textureViews.items[i]);
                    v->textureMarks.items[i] = mark;
                    ++v->stamp;
                }
                continue;
            }
            if (!uploadTexture(v, t, (uint32_t)i)) return false;
        }
    }
    if (scene->instances.len > v->instanceCapacity) {
        v->instanceCapacity = (uint32_t)scene->instances.len * 2;
        if (!bufferNew(v, &v->instances, (uint64_t)v->instanceCapacity * (sizeof(ArtfInstanceData) + 64), STORAGE)) return false;
        if (!bufferNew(v, &v->visible, ((uint64_t)layersAt(v) + v->instanceCapacity) * 8, STORAGE)) return false;
        v->fresh = true;
    }
    if (scene->materials.len > v->materialCapacity) {
        v->materialCapacity = (uint32_t)scene->materials.len * 2;
        if (!bufferNew(v, &v->materials, (uint64_t)v->materialCapacity * (sizeof(ArtfMaterialData) + sizeof(ArtfMaterialMore)), STORAGE)) return false;
        v->fresh = true;
    }
    if (v->fresh || v->materialVersion != scene->materialVersion) {
        /* (a coat and sheen after them all, as Vulkan's: ArtfFrameData.materials says where) */
        bufferPut(v, &v->materials, 0, scene->materials.items, scene->materials.len * sizeof(ArtfMaterialData));
        bufferPut(v, &v->materials, scene->materials.len * sizeof(ArtfMaterialData), scene->materialsMore.items, scene->materials.len * sizeof(ArtfMaterialMore));
        v->materialVersion = scene->materialVersion;
    }
    if (v->fresh || v->instanceVersion != scene->instanceVersion) {
        bufferPut(v, &v->instances, 0, scene->instances.items, scene->instances.len * sizeof(ArtfInstanceData));
        v->instanceVersion = scene->instanceVersion;
    }
    if (v->fresh || v->structureVersion != scene->structureVersion || v->lodVersion != scene->lodVersion) {
        v->lodVersion = scene->lodVersion;
        artfVecReserve(&v->copyStarts, v->meshes.len + 1);
        uint32_t* starts = v->copyStarts.items;
        memset(starts, 0, (v->meshes.len + 1) * 4);
        for (size_t i = 0; i < scene->instances.len; ++i) {
            const uint32_t mesh = scene->instances.items[i].mesh;
            const ArtfMeshData* m = &scene->meshes.items[mesh];
            ++starts[mesh];
            for (uint32_t k = 0; k < m->lodCount; ++k) ++starts[m->lods[k]];
        }
        uint32_t* out = v->templateCpu;
        uint32_t at = 0;
        for (size_t i = 0; i < v->meshes.len; ++i, out += 5) {
            const MeshRange* m = &v->meshes.items[i];
            const uint32_t command[5] = {m->indexCount, 0, m->firstIndex, (uint32_t)m->vertexOffset, at};
            memcpy(out, command, sizeof command);
            at += starts[i];
        }
        for (uint32_t part = 1; part < 4; ++part, out += v->meshes.len * 5) {
            memcpy(out, v->templateCpu, v->meshes.len * 20);
            for (size_t i = 0; i < v->meshes.len; ++i) out[i * 5 + 4] += at * part;
        }
        v->structureVersion = scene->structureVersion;
    }
    v->fresh = false;
    /* what a pose moved */
    artfScenePosesApply(scene);
    artfSceneFaceParticles(scene);
    for (size_t i = 0; i < scene->deforming.len; ++i) {
        const uint32_t index = scene->deforming.items[i];
        const ArtfMeshData* m = &scene->meshes.items[index];
        MeshRange* range = &v->meshes.items[index];
        if (range->deformVersion == m->deformVersion) continue;
        bufferPut(v, &v->vertices, (uint64_t)range->vertexOffset * sizeof(ArtfPackedVertex), m->vertices, (size_t)m->vertexCount * sizeof(ArtfPackedVertex));
        range->deformVersion = m->deformVersion;
    }
    return true;
}

/* Every caster in every view, as Metal's list: its instance and the mesh it is drawn with, by kind and mesh. */
static bool syncShadowList(WebGpu* v, const ArtfScene* scene) {
    const uint32_t meshes = (uint32_t)v->meshes.len;
    if (v->shadowInstances == scene->instanceVersion && v->shadowStructure == scene->structureVersion && v->shadowMaterials == scene->materialVersion && v->shadowScene == scene->id && v->shadowMeshes == meshes) return true;
    if (!bufferRoom(v, &v->shadowList, ((uint64_t)scene->instances.len + 1) * 8, STORAGE)) return false;
    if (!bufferRoom(v, &v->shadowCommands, (uint64_t)meshes * 40 + 40, WGPUBufferUsage_Indirect)) return false;
    artfVecReserve(&v->copyStarts, (size_t)meshes * 2 + 1);
    uint32_t* counts = v->copyStarts.items;
    memset(counts, 0, ((size_t)meshes * 2 + 1) * 4);
#define KIND(d) (scene->materials.items[(d)->material].flags & ARTF_MATERIAL_MASK ? meshes : 0)
    for (size_t i = 0; i < scene->instances.len; ++i) {
        const ArtfInstanceData* d = &scene->instances.items[i];
        if (!d->translucent) ++counts[KIND(d) + artfMeshHeld(scene, d->mesh)];
    }
    uint32_t* commands = artfCalloc((size_t)meshes * 2 + 1, 20);
    uint32_t at = 0;
    for (uint32_t i = 0; i < meshes * 2; ++i) {
        const MeshRange* m = &v->meshes.items[i % meshes];
        const uint32_t command[5] = {m->indexCount, counts[i], m->firstIndex, (uint32_t)m->vertexOffset, at};
        memcpy(commands + i * 5, command, sizeof command);
        const uint32_t count = counts[i];
        counts[i] = at;
        at += count;
    }
    uint32_t* list = artfCalloc(scene->instances.len + 1, 8);
    for (size_t i = 0; i < scene->instances.len; ++i) {
        const ArtfInstanceData* d = &scene->instances.items[i];
        if (d->translucent) continue;
        const uint32_t held = artfMeshHeld(scene, d->mesh);
        const uint32_t slot = counts[KIND(d) + held]++;
        list[slot * 2] = (uint32_t)i;
        list[slot * 2 + 1] = held;
    }
#undef KIND
    bufferPut(v, &v->shadowCommands, 0, commands, (size_t)meshes * 40);
    bufferPut(v, &v->shadowList, 0, list, scene->instances.len * 8);
    artfFree(commands);
    artfFree(list);
    v->shadowInstances = scene->instanceVersion;
    v->shadowStructure = scene->structureVersion;
    v->shadowMaterials = scene->materialVersion;
    v->shadowScene = scene->id;
    v->shadowMeshes = meshes;
    return true;
}

/* Lights past the frame's own, as Vulkan's (syncLights): the lights, and the lists by tile. */
static bool syncLights(WebGpu* v, const ArtfScene* scene, ArtfFrameData* frame) {
    if (frame->eye[2][3] <= 0) return true;
    if (!v->lightScratch) v->lightScratch = artfMalloc(ARTF_MAX_LIGHTS * sizeof(ArtfLightData));
    const uint32_t count = artfSceneExtraLights(scene, frame, v->lightScratch);
    artfSceneLightSlices(frame, v->lightScratch, count);
    uint32_t omitted = 0;
    const size_t need = artfSceneLightTiles(frame, v->width, v->height, v->lightScratch, count, NULL, &v->listScratch, &v->listScratchSize, &omitted) * 4;
    if (!bufferRoom(v, &v->tileLights, need, STORAGE)) return false;
    bufferPut(v, &v->extraLights, 0, v->lightScratch, (size_t)count * sizeof(ArtfLightData));
    bufferPut(v, &v->tileLights, 0, v->listScratch, need);
    return true;
}

/* ---- the overlay ---- */

static void webgpuSetOverlay(ArtfRenderer* r, const ArtfOverlay* o) {
    WebGpu* v = (WebGpu*)r;
    if (!o || !o->pixels) {
        v->overlayPixels = NULL;
        v->overlayPending = v->overlayUsable = false;
        return;
    }
    v->overlayPixels = o->pixels;
    v->overlayWidth = o->width;
    v->overlayHeight = o->height;
    v->overlayStride = o->stride ? o->stride : o->width;
    if (!o->partial || o->damageCount) v->overlayPending = true;
}

static void uploadOverlay(WebGpu* v) {
    if (!v->overlayPending) return;
    const int ow = artfMini(v->overlayWidth, v->width), oh = artfMini(v->overlayHeight, v->height);
    /* (all of it: what changed is a part, but a write of the whole is one call) */
    if (ow < v->width || oh < v->height) {
        uint32_t* clear = artfCalloc((size_t)v->width * v->height, 4);
        texturePut(v, v->overlay, 0, 0, 0, v->width, v->height, clear, v->width);
        artfFree(clear);
    }
    if (ow > 0 && oh > 0) texturePut(v, v->overlay, 0, 0, 0, ow, oh, v->overlayPixels, v->overlayStride);
    v->overlayUsable = true;
    v->overlayPending = false;
}

/* ---- a frame ---- */

static void drawCommands(WebGpu* v, WGPURenderPassEncoder e, WGPUBuffer commands, uint32_t first, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i)
        if (v->meshes.items[i % v->meshes.len].indexCount) wgpuRenderPassEncoderDrawIndexedIndirect(e, commands, (uint64_t)(first + i) * 20);
}

static void use(WebGpu* v, WGPURenderPassEncoder e, Program* p, uint32_t push) {
    wgpuRenderPassEncoderSetPipeline(e, p->render);
    const uint32_t offset = push * PUSH_STRIDE;
    wgpuRenderPassEncoderSetBindGroup(e, 0, groupOf(v, p), p->push ? 1 : 0, p->push ? &offset : NULL);
}

static void drawShadows(WebGpu* v, WGPUCommandEncoder c, const ArtfShadowFrame* shadows, uint32_t views) {
    WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    depth.view = v->atlasView;
    depth.depthLoadOp = WGPULoadOp_Clear;
    depth.depthStoreOp = WGPUStoreOp_Store;
    depth.depthClearValue = 1.f;
    WGPURenderPassDescriptor d = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    d.depthStencilAttachment = &depth;
    WGPURenderPassEncoder e = wgpuCommandEncoderBeginRenderPass(c, &d);
    wgpuRenderPassEncoderSetVertexBuffer(e, 0, v->vertices.buffer, 0, v->vertices.size);
    wgpuRenderPassEncoderSetIndexBuffer(e, v->indices.buffer, WGPUIndexFormat_Uint32, 0, v->indices.size);
    const uint32_t meshes = (uint32_t)v->meshes.len;
    for (uint32_t i = 0; i < views; ++i) {
        const float* info = shadows->views[i].info;
        const float side = shadows->views[i].size[0];
        wgpuRenderPassEncoderSetViewport(e, info[0], info[1], side, side, 0, 1);
        wgpuRenderPassEncoderSetScissorRect(e, (uint32_t)info[0], (uint32_t)info[1], (uint32_t)side, (uint32_t)side);
        use(v, e, &v->shadow, i);
        drawCommands(v, e, v->shadowCommands.buffer, 0, meshes);
        use(v, e, &v->shadowHoles, i);
        drawCommands(v, e, v->shadowCommands.buffer, meshes, meshes);
    }
    wgpuRenderPassEncoderEnd(e);
    wgpuRenderPassEncoderRelease(e);
}

/* Light from all around: the scene's, up when it changes (vulkanTrace.c's syncEnvironment); and what draws it behind
 * the scene, in the scene's pass, made when first asked. */
static bool syncEnvironment(WebGpu* v, const ArtfScene* scene) {
    const uint64_t full = ARTF_ENV_FLOATS * sizeof(float);
    if (!v->environment.buffer && !bufferNew(v, &v->environment, full, STORAGE)) return false;
    if (scene->environment && (v->environmentVersion != scene->environmentVersion || v->environmentScene != scene->id)) {
        bufferPut(v, &v->environment, 0, scene->environment, (size_t)full);
        v->environmentVersion = scene->environmentVersion;
        v->environmentScene = scene->id;
    }
    return true;
}

static bool traceShowMake(WebGpu* v);

static bool environmentBehindMake(WebGpu* v) {
    size_t size = 0;
    IrWgslBindings a, b;
    uint32_t* code = artfShaderOverlayVertex(&size);
    char* vt = wgslOf(code, size, false, &a);
    code = artfShaderEnvironmentBackdrop(&size, v->floats);
    b.count = 0;
    char* ft = wgslOf(code, size, false, &b);
    v->environmentBehind.bindings = a;
    bindingsJoin(&v->environmentBehind.bindings, &b);
    const WGPUTextureFormat light = v->floats ? WGPUTextureFormat_RGBA16Float : 0;
    const WGPUTextureFormat surface = !v->floats ? 0 : v->wide ? WGPUTextureFormat_RGBA32Uint : WGPUTextureFormat_R32Uint;
    const RenderState st = {WGPUCompareFunction_Always, false, false, true, false, false, WGPUCullMode_None, light, surface, false, false};
    return vt && ft && renderMake(v, &v->environmentBehind, vt, ft, &st);
}

/* The scene's pipelines made again when its materials of their own, or what the frame is (wide), are not those they
 * were made for. */
static bool graphsSync(WebGpu* v, const ArtfScene* scene) {
    const uint32_t count = (uint32_t)scene->graphs.len;
    const uint32_t variant = (v->froxels ? 1u : 0u) | v->variantBits << 1;
    if (v->programsMade && count == v->graphCount && (!count || v->graphScene == scene->id) && v->madeWide == v->wide && v->madeVariant == variant) return true;
    if (!scenePrograms(v, scene->graphs.items, count)) return false;
    v->graphCount = count;
    v->graphScene = scene->id;
    programFree(&v->environmentBehind);
    programFree(&v->traceShow);
    if (!environmentBehindMake(v) || !traceShowMake(v)) return false;
    v->madeWide = v->wide;
    v->madeVariant = variant;
    v->programsMade = true;
    return true;
}

/* ---- a frame in floats: what comes after the scene (vulkan.c's postBuffersMake and syncLut, vulkanPost.c's
 * postRecord), over the same shaders ---- */

enum { NEED_OCCLUSION = 1, NEED_BLUR = 2, NEED_FOG = 4, NEED_SECOND = 8, NEED_HISTORY = 16, NEED_GATHERED = 32 };
enum { POST_GATHER, POST_OCCLUSION, POST_OCCLUSION_BLUR, POST_REFLECTION, POST_FROXELS, POST_TEMPORAL, POST_FOCUS, POST_BLOOM_DOWN, POST_BLOOM_UP, POST_REFLECTION_APPLY, POST_METER,
       POST_MOTION_BLUR, POST_VELOCITY, POST_TILE_MOST, POST_UPSCALE, POST_SHARPEN, POST_SAMPLED, POST_SAMPLED_SPACE, POST_CLOUD_COLUMNS, POST_CLOUD_APPLY, POST_UNDER_ACROSS, POST_UNDER_DOWN, POST_KINDS };
#define STORAGE_ALL (WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc)
#define PUSH_POST_AT ARTF_SHADOW_VIEWS /* (the post passes' push constants come after the shadow views') */
#define PUSH_POST_MOST 64

static uint32_t postNeedsOf(const ArtfFrameData* f, bool second, bool scaled) {
    const bool temporal = f->post[1][3] > 0;
    uint32_t n = 0;
    if (f->post[0][2] > 0) n |= NEED_OCCLUSION;
    if (f->motionBlur > 0) n |= NEED_BLUR | NEED_SECOND;
    if (f->post[1][1] > 0 && f->look[1] > 0) n |= NEED_FOG;
    if (scaled || temporal) n |= NEED_SECOND | NEED_HISTORY;
    if (f->post[2][1] > 0 && f->post[2][0] > 0) n |= NEED_SECOND;
    if (f->more[3] > 0 && temporal) n |= NEED_GATHERED;
    if (second) n |= NEED_SECOND;
    return n;
}

/* The post passes' buffers, as the effects that are on need them (postBuffersMake's sizes). */
static bool postBuffersMake(WebGpu* v) {
    const uint64_t pixels = (uint64_t)v->sceneWidth * v->sceneHeight, framePixels = (uint64_t)v->width * v->height;
    const uint64_t fog = (uint64_t)((v->sceneWidth + ARTF_FROXEL_TILE - 1) / ARTF_FROXEL_TILE) * ((v->sceneHeight + ARTF_FROXEL_TILE - 1) / ARTF_FROXEL_TILE) * ARTF_FROXEL_SLICES;
    uint64_t bloom = 0;
    uint32_t w = (uint32_t)v->width, h = (uint32_t)v->height;
    for (int l = 0; l < 6; ++l) {
        w = (w + 1) / 2 ? (w + 1) / 2 : 1;
        h = (h + 1) / 2 ? (h + 1) / 2 : 1;
        v->bloomSizes[l][0] = (uint32_t)bloom;
        v->bloomSizes[l][1] = w;
        v->bloomSizes[l][2] = h;
        bloom += (uint64_t)w * h;
    }
    for (int i = 0; i < 8; ++i) bufferFree(&v->post[i]);
    bufferFree(&v->postGathered);
    bufferFree(&v->postLocks);
    const uint64_t tiles = (uint64_t)((v->width + 15) / 16) * ((v->height + 15) / 16);
    const uint64_t occlusion = v->postNeeds & NEED_OCCLUSION ? pixels * 4 : 0, blur = v->postNeeds & NEED_BLUR ? (framePixels + tiles * 2) * 8 : 0;
    const uint64_t sizes[8] = {framePixels * 8, pixels * 4, pixels * 12, v->postNeeds & NEED_SECOND ? framePixels * 8 : 0, v->postNeeds & NEED_HISTORY ? framePixels * 8 : 0,
                               occlusion > blur ? occlusion : blur, v->postNeeds & NEED_FOG ? fog * 16 : 0, (bloom + 1 + v->lutRoom) * 16};
    for (int i = 0; i < 8; ++i)
        if (sizes[i] && !bufferNew(v, &v->post[i], sizes[i], STORAGE_ALL)) return false;
    if (v->postNeeds & NEED_GATHERED && !bufferNew(v, &v->postGathered, pixels * 64, STORAGE_ALL)) return false;
    /* (upscaling's locks, with a history and the scene drawn smaller) */
    if (v->postNeeds & NEED_HISTORY && pixels != framePixels && !bufferNew(v, &v->postLocks, framePixels * 8, STORAGE_ALL)) return false;
    v->role[0] = 0, v->role[1] = 1, v->role[2] = 2;
    v->historyValid = v->exposureValid = v->hasPrevious = v->gatheredValid = false;
    v->lutVersion = 0;
    ++v->stamp;
    return true;
}

/* The scene's look-up table, after bloom's levels and auto exposure's state (syncLut). */
static bool syncLut(WebGpu* v, const ArtfScene* scene) {
    if (!scene->lut || (v->lutScene == scene->id && v->lutVersion == scene->lutVersion)) return true;
    const uint32_t entries = (uint32_t)(scene->lutFloats / 3);
    const uint64_t bloom = (uint64_t)v->bloomSizes[5][0] + (uint64_t)v->bloomSizes[5][1] * v->bloomSizes[5][2];
    if (entries > v->lutRoom) {
        v->lutRoom = entries;
        bufferFree(&v->post[7]);
        if (!bufferNew(v, &v->post[7], (bloom + 1 + v->lutRoom) * 16, STORAGE_ALL)) return false;
        v->exposureValid = false;
        ++v->stamp;
    }
    float* rows = artfMalloc((size_t)entries * 16);
    for (uint32_t i = 0; i < entries; ++i) {
        memcpy(rows + i * 4, scene->lut + i * 3, 12);
        rows[i * 4 + 3] = 0;
    }
    bufferPut(v, &v->post[7], (bloom + 1) * 16, rows, (size_t)entries * 16);
    artfFree(rows);
    v->lutScene = scene->id;
    v->lutVersion = scene->lutVersion;
    return true;
}

/* Clouds in the scene: their field and grid (cloudsSync). */
static bool cloudsSync(WebGpu* v, const ArtfScene* scene, uint64_t* generation, uint32_t* columns, uint32_t* rows) {
    *columns = ((uint32_t)v->sceneWidth + ARTF_CLOUD_TILE - 1) / ARTF_CLOUD_TILE;
    *rows = ((uint32_t)v->sceneHeight + ARTF_CLOUD_TILE - 1) / ARTF_CLOUD_TILE;
    const uint64_t grid = (uint64_t)*columns * *rows * ARTF_CLOUD_CELL * 4;
    if (!v->cloudField.buffer && !bufferNew(v, &v->cloudField, (uint64_t)ARTF_CLOUD_FLOATS * 4, STORAGE_ALL)) return false;
    if (v->cloudGrid.size != grid) {
        bufferFree(&v->cloudGrid);
        if (!bufferNew(v, &v->cloudGrid, grid, STORAGE_ALL)) return false;
        ++v->stamp;
        *generation = ~(uint64_t)0;
    }
    if (*generation != scene->cloudsGeneration) {
        bufferPut(v, &v->cloudField, 0, scene->clouds, (size_t)ARTF_CLOUD_FLOATS * 4);
        *generation = scene->cloudsGeneration;
        ++v->stamp;
    }
    return true;
}

static bool postProgramsMake(WebGpu* v) {
    uint32_t* (*const makers[POST_KINDS])(size_t*) = {NULL, artfShaderPostOcclusion, artfShaderPostOcclusionBlur, artfShaderPostReflection, artfShaderPostFroxels,
                                                      artfShaderPostTemporal, artfShaderPostFocus, artfShaderPostBloomDown, artfShaderPostBloomUp, artfShaderPostReflectionApply,
                                                      artfShaderPostMeter, artfShaderPostMotionBlur, artfShaderPostVelocity, artfShaderPostTileMost, artfShaderPostUpscale,
                                                      artfShaderPostSharpen, artfShaderPostSampled, artfShaderPostSampledSpace, artfShaderPostCloudColumns, artfShaderPostCloudApply,
                                                      artfShaderPostUnderAcross, artfShaderPostUnderDown};
    for (int k = 0; k < POST_KINDS; ++k) {
        size_t size = 0;
        uint32_t* code = k == POST_GATHER ? artfShaderPostGather(&size, v->samples) : makers[k](&size);
        char* text = wgslOf(code, size, false, &v->posts[k].bindings);
        if (!computeMake(v, &v->posts[k], text)) {
            artfLog("artful: WebGPU's post pass %d could not be made", k);
            return false;
        }
    }
    /* the final pass and the overlay over it: the frame's size, one sample, no depth */
    size_t size = 0;
    IrWgslBindings a, b;
    uint32_t* code = artfShaderOverlayVertex(&size);
    char* vt = wgslOf(code, size, false, &a);
    code = artfShaderPostFinalFor(&size, false, NULL);
    b.count = 0;
    char* ft = wgslOf(code, size, false, &b);
    v->final.bindings = a;
    bindingsJoin(&v->final.bindings, &b);
    const RenderState replace = {WGPUCompareFunction_Always, false, false, true, false, false, WGPUCullMode_None, WGPUTextureFormat_BGRA8Unorm, 0, false};
    if (!vt || !ft || !renderMake(v, &v->final, vt, ft, &replace)) return false;
    code = artfShaderOverlayVertex(&size);
    vt = wgslOf(code, size, false, &a);
    code = artfShaderOverlayFragment(&size, false);
    b.count = 0;
    ft = wgslOf(code, size, false, &b);
    v->overlayFloats.bindings = a;
    bindingsJoin(&v->overlayFloats.bindings, &b);
    const RenderState over = {WGPUCompareFunction_Always, false, true, true, false, false, WGPUCullMode_None, WGPUTextureFormat_BGRA8Unorm, 0, false};
    return vt && ft && renderMake(v, &v->overlayFloats, vt, ft, &over);
}

/* A post pass's dispatch, its push constant at its own offset (postStep). */
static void postStep(WebGpu* v, WGPUComputePassEncoder e, int kind, uint32_t push, uint32_t count) {
    Program* p = &v->posts[kind];
    if (v->pushCount >= PUSH_POST_MOST) return;
    const uint32_t at = v->pushCount++;
    v->pushes[at * (PUSH_STRIDE / 4)] = push;
    const uint32_t offset = (PUSH_POST_AT + at) * PUSH_STRIDE;
    uint32_t groups = (count + 63) / 64;
    if (groups > v->maxGroups) {
        if (!v->warnedLarge) {
            v->warnedLarge = true;
            artfLog("artful: WebGPU's post passes go up to %u pixels: the frame is larger", v->maxGroups * 64);
        }
        groups = v->maxGroups;
    }
    wgpuComputePassEncoderSetPipeline(e, p->compute);
    wgpuComputePassEncoderSetBindGroup(e, 0, groupOf(v, p), p->push ? 1 : 0, p->push ? &offset : NULL);
    if (p->layout1) wgpuComputePassEncoderSetBindGroup(e, 1, rolesGroupOf(v, p), 0, NULL);
    wgpuComputePassEncoderDispatchWorkgroups(e, groups, 1, 1);
}

/* What a pass wrote into the second is the light from now on (postSwap). */
static void postSwap(WebGpu* v, bool keep) {
    const uint8_t light = v->role[1], was = v->role[0];
    if (keep) v->role[2] = light;
    v->role[0] = light;
    v->role[1] = was != v->role[2] && was != light ? was : (uint8_t)(3 - light - v->role[2]);
}

/* The post passes, in post.c's order (postRecord, at the frame's own size: no upscaling here). */
static void postChain(WebGpu* v, WGPUCommandEncoder c, const ArtfFrameData* frame, bool under, bool clouds, uint32_t cloudColumns, uint32_t cloudRows) {
    const uint32_t pixels = (uint32_t)v->sceneWidth * (uint32_t)v->sceneHeight, framePixels = (uint32_t)v->width * (uint32_t)v->height;
    const bool scaled = pixels != framePixels;
    WGPUComputePassEncoder e = wgpuCommandEncoderBeginComputePass(c, NULL);
    v->role[0] = v->role[2] ? 0 : 1;
    v->role[1] = (uint8_t)(3 - v->role[0] - v->role[2]);
    postStep(v, e, POST_GATHER, 0, pixels);
    if (under) {
        postStep(v, e, POST_UNDER_ACROSS, 0, pixels);
        postStep(v, e, POST_UNDER_DOWN, 0, pixels);
    }
    if (frame->post[0][2] > 0) {
        postStep(v, e, POST_OCCLUSION, 0, pixels);
        postStep(v, e, POST_OCCLUSION_BLUR, 0, pixels);
    }
    if (frame->post[1][0] > 0) {
        if (frame->traced & ARTF_TRACED_REFLECTIONS && v->reflectionTraced.compute) {
            /* (from rays: a pass of its own, with no push constant) */
            Program* p = &v->reflectionTraced;
            wgpuComputePassEncoderSetPipeline(e, p->compute);
            wgpuComputePassEncoderSetBindGroup(e, 0, groupOf(v, p), 0, NULL);
            if (p->layout1) wgpuComputePassEncoderSetBindGroup(e, 1, rolesGroupOf(v, p), 0, NULL);
            wgpuComputePassEncoderDispatchWorkgroups(e, (v->bloomSizes[0][1] * v->bloomSizes[0][2] + 63) / 64, 1, 1);
        } else
        postStep(v, e, POST_REFLECTION, 0, v->bloomSizes[0][1] * v->bloomSizes[0][2]);
        postStep(v, e, POST_REFLECTION_APPLY, 0, pixels);
    }
    const bool temporal = frame->post[1][3] > 0;
    if (frame->more[3] > 0 && temporal && v->postGathered.buffer) {
        postStep(v, e, POST_SAMPLED, (v->gatheredValid ? 1u : 0u) | v->gatheredHalf << 1, pixels);
        v->gatheredHalf ^= 1;
        v->gatheredValid = true;
        postSwap(v, false);
        postStep(v, e, POST_SAMPLED_SPACE, 0, pixels);
        postSwap(v, false);
    } else v->gatheredValid = false;
    if (clouds) {
        postStep(v, e, POST_CLOUD_COLUMNS, cloudColumns, cloudColumns * cloudRows);
        postStep(v, e, POST_CLOUD_APPLY, cloudColumns, pixels);
        postSwap(v, false);
    }
    if (scaled) {
        /* (from here on, the frame's size) */
        postStep(v, e, POST_UPSCALE, (temporal && v->historyValid ? 1u : 0u) | v->lockHalf << 1, framePixels);
        v->lockHalf ^= 1;
        postSwap(v, temporal);
    } else if (temporal) {
        postStep(v, e, POST_TEMPORAL, v->historyValid ? 1u : 0u, pixels);
        postSwap(v, true);
    }
    v->historyValid = temporal;
    if (frame->output[2] > 0 && (scaled || temporal)) {
        postStep(v, e, POST_SHARPEN, 0, framePixels);
        postSwap(v, false);
    }
    if (frame->post[2][1] > 0 && frame->post[2][0] > 0) {
        postStep(v, e, POST_FOCUS, 0, framePixels);
        postSwap(v, false);
    }
    if (frame->motionBlur > 0) {
        const uint32_t tiles = (((uint32_t)v->width + 15) / 16) * (((uint32_t)v->height + 15) / 16);
        postStep(v, e, POST_VELOCITY, 0, framePixels);
        postStep(v, e, POST_TILE_MOST, 0, tiles);
        postStep(v, e, POST_TILE_MOST, 1, tiles);
        postStep(v, e, POST_MOTION_BLUR, 0, framePixels);
        postSwap(v, false);
    }
    if (frame->grade[0][1] > 0) {
        postStep(v, e, POST_METER, v->exposureValid ? 1u : 0u, 1);
        v->exposureValid = true;
    } else v->exposureValid = false;
    if (frame->post[0][0] > 0) {
        for (uint32_t l = 0; l < 6; ++l) postStep(v, e, POST_BLOOM_DOWN, l, v->bloomSizes[l][1] * v->bloomSizes[l][2]);
        for (int l = 4; l >= 0; --l) postStep(v, e, POST_BLOOM_UP, (uint32_t)l, v->bloomSizes[l][1] * v->bloomSizes[l][2]);
    }
    wgpuComputePassEncoderEnd(e);
    wgpuComputePassEncoderRelease(e);
}

/* ---- tracing: the scene's trees on the GPU, walked by the trace shader (no ray tracing hardware), as Vulkan's ---- */

/* A buffer as large as what goes into it, made again (and every group with it) when it is not. */
static bool bufferHold(WebGpu* v, Gpu* g, const void* data, size_t bytes, WGPUBufferUsage usage) {
    if (!g->buffer || g->size < bytes) {
        bufferFree(g);
        if (!bufferNew(v, g, bytes > 32 ? (uint64_t)bytes + bytes / 2 : 64, usage)) return false;
        ++v->stamp;
    }
    if (data && bytes) bufferPut(v, g, 0, data, bytes);
    return true;
}

/* What rays walk and what tracing gathers into, as the scene's trees are now (vulkanTrace.c's syncTrace). */
static bool treesSync(WebGpu* v, ArtfScene* scene) {
    artfSceneTraceSync(scene);
    const ArtfTrace* t = &scene->trace;
    if (v->traceScene != scene->id || v->traceMeshVersion != t->meshVersion || scene->deforming.len) {
        /* (what bones bend moves the trees' nodes and vertices: those go up again each frame) */
        if (!bufferHold(v, &v->traceNodes, t->wide.items, t->wide.len * sizeof *t->wide.items, STORAGE) ||
            !bufferHold(v, &v->traceTriangles, t->triangles.items, t->triangles.len * sizeof *t->triangles.items, STORAGE) ||
            !bufferHold(v, &v->traceVertices, t->vertices.items, t->vertices.len * sizeof *t->vertices.items, STORAGE))
            return false;
        v->traceMeshVersion = t->meshVersion;
    }
    if (v->traceScene != scene->id || v->traceTopVersion != t->topVersion) {
        if (!bufferHold(v, &v->traceTop, t->top.items, t->top.len * sizeof *t->top.items, STORAGE) ||
            !bufferHold(v, &v->traceInstances, t->instances.items, (t->instances.len + 1) * sizeof *t->instances.items, STORAGE))
            return false;
        v->traceTopVersion = t->topVersion;
    }
    v->traceScene = scene->id;
    return true;
}

static bool traceSync(WebGpu* v, ArtfScene* scene, bool guides) {
    if (!treesSync(v, scene)) return false;
    const ArtfTrace* t = &scene->trace;
    const uint64_t pixels = (uint64_t)v->sceneWidth * v->sceneHeight;
    if (v->traceSum.size < pixels * 16) {
        bufferFree(&v->traceSum);
        if (!bufferNew(v, &v->traceSum, pixels * 16, STORAGE_ALL)) return false;
        v->traceDone = 0;
        ++v->stamp;
    }
    if (!v->traceFrameUbo.buffer && !bufferNew(v, &v->traceFrameUbo, sizeof(ArtfTraceFrame), UNIFORM)) return false;
    if (guides && v->denoiseBuffers[0].size < pixels * 16) {
        for (uint32_t i = 0; i < 7; ++i) {
            bufferFree(&v->denoiseBuffers[i]);
            if (!bufferNew(v, &v->denoiseBuffers[i], pixels * (i == 3 || i == 4 ? 32 : 16), STORAGE_ALL)) return false;
        }
        bufferFree(&v->denoiseShift);
        if (!bufferNew(v, &v->denoiseShift, pixels * 16, STORAGE_ALL)) return false;
        v->denoiseFirst = true;
        ++v->stamp;
    }
    if (guides && !bufferHold(v, &v->traceMotion, t->motion.items, (t->motion.len + 12) * 4, STORAGE)) return false;
    return true;
}

/* The trace shader for the scene's materials of their own, the filter's guides and what it reads past glTF's core;
 * the filter's passes, when first asked. */
static bool tracePrograms(WebGpu* v, const ArtfScene* scene, bool guides, bool more) {
    const uint32_t count = (uint32_t)scene->graphs.len;
    const uint32_t key = count << 2 | (guides ? 1u : 0u) | (more ? 2u : 0u);
    if (!v->trace.compute || key != v->traceProgramKey || (count && v->traceProgramScene != scene->id)) {
        programFree(&v->trace);
        size_t size = 0;
        uint32_t* code = artfShaderTrace(&size, v->maxTextures | ARTF_TEXTURES_UNIFORM, scene->graphs.items, count, guides, false, more);
        char* text = wgslOf(code, size, false, &v->trace.bindings);
        if (!computeMake(v, &v->trace, text)) {
            artfLog("artful: WebGPU's trace shader could not be made");
            return false;
        }
        v->traceProgramKey = key;
        v->traceProgramScene = scene->id;
    }
    if (guides && !v->denoisePasses[0].compute) {
        for (int i = 0; i < 9; ++i) {
            size_t size = 0;
            uint32_t* code = i == 0 ? artfShaderDenoiseLight(&size) : i == 1 ? artfShaderDenoiseTime(&size) : i == 2 ? artfShaderDenoiseVariance(&size)
                             : i < 8 ? artfShaderDenoiseSpace(&size, i - 3) : artfShaderDenoiseEnd(&size);
            char* text = wgslOf(code, size, false, &v->denoisePasses[i].bindings);
            if (!computeMake(v, &v->denoisePasses[i], text)) return false;
        }
    }
    return true;
}

/* The trace's dispatch, and the filter's passes after it (vulkanTrace.c's denoise). */
static void traceRecord(WebGpu* v, WGPUCommandEncoder c, bool clear, bool adding, bool denoise) {
    const uint64_t pixels = (uint64_t)v->sceneWidth * v->sceneHeight;
    if (clear) wgpuCommandEncoderClearBuffer(c, v->traceSum.buffer, 0, pixels * 16);
    if (!adding) return;
    const uint32_t gx = ((uint32_t)v->sceneWidth + 7) / 8, gy = ((uint32_t)v->sceneHeight + 7) / 8;
    WGPUComputePassEncoder e = wgpuCommandEncoderBeginComputePass(c, NULL);
    wgpuComputePassEncoderSetPipeline(e, v->trace.compute);
    wgpuComputePassEncoderSetBindGroup(e, 0, groupOf(v, &v->trace), 0, NULL);
    wgpuComputePassEncoderDispatchWorkgroups(e, gx, gy, 1);
    for (int i = 0; denoise && i < 9; ++i) {
        wgpuComputePassEncoderSetPipeline(e, v->denoisePasses[i].compute);
        wgpuComputePassEncoderSetBindGroup(e, 0, groupOf(v, &v->denoisePasses[i]), 0, NULL);
        wgpuComputePassEncoderDispatchWorkgroups(e, gx, gy, 1);
    }
    wgpuComputePassEncoderEnd(e);
    wgpuComputePassEncoderRelease(e);
    if (denoise) {
        /* guide to the frame before's, what is gathered to what was */
        wgpuCommandEncoderCopyBufferToBuffer(c, v->denoiseBuffers[0].buffer, 0, v->denoiseBuffers[2].buffer, 0, pixels * 16);
        wgpuCommandEncoderCopyBufferToBuffer(c, v->denoiseBuffers[4].buffer, 0, v->denoiseBuffers[3].buffer, 0, pixels * 32);
    }
}

/* Light that bounced: the probes' buffers, the step's shader made for what they are, and from what the scene kept of
 * them when they are new here (vulkan.c's syncIndirect). false: none to step. */
static bool probesSync(WebGpu* v, ArtfScene* scene, bool* step) {
    *step = false;
    const ArtfIndirect* g = &scene->indirect;
    const uint64_t need = (uint64_t)artfIndirectCount(g) * ARTF_PROBE_FLOATS * 4;
    if (!treesSync(v, scene)) return false;
    if (v->probesNow.size < need) {
        bufferFree(&v->probesNow);
        bufferFree(&v->probesNext);
        if (!bufferNew(v, &v->probesNow, need, STORAGE_ALL) || !bufferNew(v, &v->probesNext, need, STORAGE_ALL)) return false;
        v->probeScene = 0;
        ++v->stamp;
    }
    const int rays = g->desc.rays > 0 ? g->desc.rays : 64;
    const float keep = g->desc.hysteresis > 0 ? artfClampf(g->desc.hysteresis, 0, .999f) : .9f;
    const bool walls = !g->desc.throughWalls, more = scene->moreMaterials > 0;
    uint32_t keepBits;
    memcpy(&keepBits, &keep, 4);
    const uint64_t key = (uint64_t)keepBits << 32 | (uint64_t)rays << 3 | (walls ? 1u : 0u) | (more ? 4u : 0u);
    if (!v->probeStep.compute || v->probeKey != key || v->probeGraphs != scene->graphs.len || v->probeGraphScene != scene->id) {
        programFree(&v->probeStep);
        size_t size = 0;
        uint32_t* code = artfShaderProbes(&size, v->maxTextures | ARTF_TEXTURES_UNIFORM, scene->graphs.items, (uint32_t)scene->graphs.len, false, more, walls, rays, keep);
        char* text = wgslOf(code, size, false, &v->probeStep.bindings);
        if (!computeMake(v, &v->probeStep, text)) {
            artfLog("artful: WebGPU's probes' shader could not be made");
            return false;
        }
        v->probeKey = key;
        v->probeGraphs = (uint32_t)scene->graphs.len;
        v->probeGraphScene = scene->id;
    }
    if (v->probeScene != scene->id || v->probeSerial != g->serial) {
        /* (new here: from what the scene kept of them, or from nothing) */
        v->probeScene = scene->id;
        v->probeSerial = g->serial;
        v->probeFrames = 0;
        if (g->frames && g->probes) {
            bufferPut(v, &v->probesNow, 0, g->probes, (size_t)need);
            v->probeFrames = g->frames;
        } else {
            float* zeros = artfCalloc((size_t)need, 1);
            bufferPut(v, &v->probesNow, 0, zeros, (size_t)need);
            artfFree(zeros);
        }
    }
    *step = true;
    return true;
}

/* A step of the probes: from what they held into the next, then that into what this frame's pixels read. */
static void probesRecord(WebGpu* v, WGPUCommandEncoder c, const ArtfScene* scene) {
    const ArtfIndirect* g = &scene->indirect;
    WGPUComputePassEncoder e = wgpuCommandEncoderBeginComputePass(c, NULL);
    Program* p = &v->probeStep;
    if (v->pushCount < PUSH_POST_MOST) {
        const uint32_t at = v->pushCount++;
        v->pushes[at * (PUSH_STRIDE / 4)] = v->probeFrames;
        const uint32_t offset = (PUSH_POST_AT + at) * PUSH_STRIDE;
        wgpuComputePassEncoderSetPipeline(e, p->compute);
        wgpuComputePassEncoderSetBindGroup(e, 0, groupOf(v, p), p->push ? 1 : 0, p->push ? &offset : NULL);
        wgpuComputePassEncoderDispatchWorkgroups(e, (uint32_t)(g->dims[0] * g->dims[1]), (uint32_t)(g->dims[2] * (g->cascades > 0 ? g->cascades : 1)), 1);
    }
    wgpuComputePassEncoderEnd(e);
    wgpuComputePassEncoderRelease(e);
    wgpuCommandEncoderCopyBufferToBuffer(c, v->probesNext.buffer, 0, v->probesNow.buffer, 0, (uint64_t)artfIndirectCount(g) * ARTF_PROBE_FLOATS * 4);
    ++v->probeFrames;
}

/* What shows what tracing gathered, in the scene's pass (as the scene's own: its targets are theirs). */
static bool traceShowMake(WebGpu* v) {
    size_t size = 0;
    IrWgslBindings a, b;
    uint32_t* code = artfShaderOverlayVertex(&size);
    char* vt = wgslOf(code, size, false, &a);
    code = artfShaderTraceShow(&size, v->floats);
    b.count = 0;
    char* ft = wgslOf(code, size, false, &b);
    v->traceShow.bindings = a;
    bindingsJoin(&v->traceShow.bindings, &b);
    const WGPUTextureFormat light = v->floats ? WGPUTextureFormat_RGBA16Float : 0;
    const WGPUTextureFormat surface = !v->floats ? 0 : v->wide ? WGPUTextureFormat_RGBA32Uint : WGPUTextureFormat_R32Uint;
    const RenderState st = {WGPUCompareFunction_Always, false, false, true, false, false, WGPUCullMode_None, light, surface, false, false};
    return vt && ft && renderMake(v, &v->traceShow, vt, ft, &st);
}

/* The gather's depth alone, between what is solid and the layers (their depth is not what comes after reads). */
static void gatherDepth(WebGpu* v, WGPUCommandEncoder c) {
    WGPUComputePassEncoder e = wgpuCommandEncoderBeginComputePass(c, NULL);
    postStep(v, e, POST_GATHER, 1, (uint32_t)v->sceneWidth * (uint32_t)v->sceneHeight);
    wgpuComputePassEncoderEnd(e);
    wgpuComputePassEncoderRelease(e);
}

/* The final pass, and the overlay over it, into the frame. */
static void finalPass(WebGpu* v, WGPUCommandEncoder c) {
    WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = v->frameView;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor pass = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    pass.colorAttachmentCount = 1;
    pass.colorAttachments = &color;
    WGPURenderPassEncoder e = wgpuCommandEncoderBeginRenderPass(c, &pass);
    use(v, e, &v->final, 0);
    if (v->final.layout1) wgpuRenderPassEncoderSetBindGroup(e, 1, rolesGroupOf(v, &v->final), 0, NULL);
    wgpuRenderPassEncoderDraw(e, 3, 1, 0, 0);
    if (v->overlayPixels && v->overlayUsable) {
        use(v, e, &v->overlayFloats, 0);
        wgpuRenderPassEncoderDraw(e, 3, 1, 0, 0);
    }
    wgpuRenderPassEncoderEnd(e);
    wgpuRenderPassEncoderRelease(e);
}


static bool frameDraw(WebGpu* v, ArtfScene* scene) {
    /* (the surface's other words: where motion is followed or light goes under the surface; not with samples, as
     * WebGPU's RGBA32Uint has none) */
    const bool wants = v->floats && (scene->post.screen.temporal || scene->post.lens.motionBlur > 0 || artfSceneHasUnder(scene));
    if (wants && v->samples > 1 && !v->warnedWide) {
        v->warnedWide = true;
        artfLog("artful: WebGPU with four samples has no temporal antialiasing, motion blur or light under the surface");
    }
    if (wants && v->samples == 1 && scene->deforming.len && !v->warnedWide) {
        v->warnedWide = true;
        artfLog("artful: WebGPU follows the motion of instances, not of what bones bend");
    }
    const bool wide = wants && v->samples == 1;
    if (wide != v->wide) {
        v->wide = wide;
        v->targetsStale = true;
    }
    if (v->floats && v->base.renderScale != v->scale) {
        /* (pacing: the scene's targets at another size) */
        v->scale = v->base.renderScale;
        v->targetsStale = true;
    }
    if (v->targetsStale && !targetsMake(v)) return false;
    if (!v->frameTexture) return true; /* (a window with no size) */
#define ONCE(flag, ...) do { if (!v->flag) { v->flag = true; artfLog(__VA_ARGS__); } } while (0)
    const bool tracing = scene->trace.desc.mode != ARTF_TRACE_NONE && !v->base.view;
    if (tracing && scene->trace.desc.hardware) ONCE(warnedTraceHardware, "artful: WebGPU traces through the scene's trees: it has no ray tracing hardware");
    if (v->base.view) ONCE(warnedView, "artful: WebGPU shows no views but the frame");
#undef ONCE
    v->froxels = v->floats && scene->post.screen.volumetric > 0 && scene->fogDensity > 0;
    /* (what the scene's materials and lights ask of the pixel shaders: a coat and more past glTF's core, soft shadows,
     * lights with a shape or a profile) */
    v->variantBits = (scene->layeredMaterials > 0 ? (uint32_t)ARTF_SHADER_COAT : 0u) | (scene->shadows.soft && scene->shadows.resolution > 0 ? (uint32_t)ARTF_SHADER_SOFT : 0u) |
                     (artfSceneShapedLights(scene) ? (uint32_t)ARTF_SHADER_SHAPES : 0u);
    if (scene->shadows.traced && !tracing) v->variantBits |= (uint32_t)ARTF_SHADER_TRACED;
    const bool probes = scene->indirect.on && scene->indirect.probes;
    if (probes) v->variantBits |= (uint32_t)ARTF_SHADER_PROBES | (!scene->indirect.desc.throughWalls ? (uint32_t)ARTF_SHADER_WALLS : 0u);
    if (!syncEnvironment(v, scene)) return false;
    if (!syncScene(v, scene) || !graphsSync(v, scene)) return false;
    if (v->floats && !v->posts[0].compute) return false;
    ArtfFrameData frame;
    artfSceneFrame(scene, v->sceneWidth, v->sceneHeight, &frame);
    frame.output[0] = (float)v->width;
    frame.output[1] = (float)v->height;
    /* rays while drawing: shadows where the pixel shaders have them, reflections in floats; none when all is traced */
    if (tracing || !v->floats) frame.traced &= ~(uint32_t)ARTF_TRACED_REFLECTIONS;
    if (tracing) frame.traced = frame.tracedCasts = 0;
    if ((frame.traced || frame.tracedCasts) && !treesSync(v, scene)) return false;
    if (tracing) frame.post[1][1] = 0; /* (fog that light goes through is drawn, not traced) */
    v->pushCount = 0;
    bool clouds = false, under = false;
    uint32_t cloudColumns = 0, cloudRows = 0;
    if (!v->floats) {
        memset(frame.post, 0, sizeof frame.post);
    } else {
        if (!v->wide) {
            frame.post[1][3] = 0; /* (temporal antialiasing and motion blur follow motion: wide) */
            frame.motionBlur = 0;
        }
        clouds = scene->clouds && !artfOrtho(&frame);
        under = v->wide && artfSceneHasUnder(scene);
        const uint32_t needs = postNeedsOf(&frame, clouds || under, v->sceneWidth != v->width || v->sceneHeight != v->height);
        if (needs != v->postNeeds) {
            v->postNeeds = needs;
            if (!postBuffersMake(v)) return false;
        }
        if (!syncLut(v, scene)) return false;
        if (clouds && !cloudsSync(v, scene, &v->cloudGeneration, &cloudColumns, &cloudRows)) return false;
        /* the frame before, and with temporal antialiasing a place within the pixel (as softDraw) */
        frame.output[3] = (float)v->postFrames;
        float now[16];
        memcpy(now, frame.viewProj, sizeof now);
        if (v->hasPrevious) memcpy(frame.previous, v->previous, sizeof v->previous);
        memcpy(v->previous, now, sizeof now);
        v->hasPrevious = true;
        if (frame.post[1][3] > 0) artfFrameJitter(&frame, v->postFrames++, v->sceneWidth, v->sceneHeight);
        frame.grade2[2][3] = (float)(v->grainFrames++ & 0xFFFFFu);
        frame.traced &= ~(uint32_t)ARTF_TRACED_REFLECTIONS;
        if (frame.post[1][3] > 0 || frame.motionBlur > 0) {
            /* how each instance moved: four rows each, after the instances */
            artfMotionStep(&v->motion, scene, false);
            frame.moving = v->motion.moving ? 1.f : 0.f;
            float* rows = artfMalloc(v->motion.count * 64 + 64);
            for (size_t i = 0; i < v->motion.count; ++i) {
                memcpy(rows + i * 16, v->motion.motion + i * 12, 48);
                rows[i * 16 + 12] = rows[i * 16 + 13] = rows[i * 16 + 14] = rows[i * 16 + 15] = 0;
            }
            if (v->motion.count) bufferPut(v, &v->instances, scene->instances.len * sizeof(ArtfInstanceData), rows, v->motion.count * 64);
            artfFree(rows);
        }
    }
    if (!v->decalScratch) v->decalScratch = artfMalloc(ARTF_MAX_DECALS * sizeof(ArtfDecalData));
    const uint32_t decals = artfSceneDecals(scene, &frame, v->sceneHeight, v->decalScratch);
    frame.indirect[1][3] = (float)decals;
    bufferPut(v, &v->decals, 0, v->decalScratch, (size_t)decals * sizeof(ArtfDecalData));
    if (!syncLights(v, scene, &frame)) return false;
    artfSceneResidencyStep(scene, &frame, v->sceneHeight);
    /* what shows what is behind it, far to near: each its instance and mesh, after what the cull writes (as Vulkan's) */
    uint32_t layerCount = 0;
    if (scene->translucentCount && scene->instances.len && !tracing) {
        artfVecReserve(&v->layers, scene->translucentCount);
        layerCount = artfSceneLayers(scene, &frame, v->layers.items);
        artfLayerRange(&frame, v->layers.items, layerCount);
        if (layerCount) {
            if (!v->listScratch || v->listScratchSize < (size_t)layerCount * 2) {
                artfFree(v->listScratch);
                v->listScratchSize = (size_t)layerCount * 2;
                v->listScratch = artfMalloc(v->listScratchSize * 4);
            }
            for (uint32_t i = 0; i < layerCount; ++i) {
                v->listScratch[i * 2] = v->layers.items[i].instance;
                v->listScratch[i * 2 + 1] = v->layers.items[i].mesh;
            }
            bufferPut(v, &v->visible, (uint64_t)layersAt(v) * 8, v->listScratch, (size_t)layerCount * 8);
        }
    }
    bufferPut(v, &v->frame, 0, &frame, sizeof frame);
    /* the frame's lights' profiles, past the lights in their buffer (shaders.c's profileAt) */
    for (uint32_t k = 0; k < frame.lightCount; ++k)
        if (frame.lightShapes[k][2][3] > 0)
            bufferPut(v, &v->extraLights, (uint64_t)ARTF_MAX_LIGHTS * sizeof(ArtfLightData) + (uint64_t)k * ARTF_PROFILE_FLOATS * sizeof(float),
                      scene->lightProfile[(int)frame.lightShapes[k][2][3] - 1], ARTF_PROFILE_FLOATS * sizeof(float));
    uploadOverlay(v);

    ArtfShadowFrame shadows;
    memset(&shadows, 0, sizeof shadows);
    const uint32_t shadowViews = tracing ? 0 : artfSceneShadowFrame(scene, &frame, &shadows);
    bufferPut(v, &v->shadowData, 0, &shadows, sizeof shadows);
    if (shadowViews) {
        const int width = (int)shadows.sun[2] * ARTF_SHADOW_ATLAS_COLUMNS, height = (int)shadows.sun[3];
        if (width != v->shadowWidth || height > v->shadowHeight) {
            releaseTexture(&v->atlas, &v->atlasView);
            v->atlas = textureNew(v, WGPUTextureFormat_Depth32Float, width, height, 1, 1, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding);
            if (!v->atlas) return false;
            v->atlasView = viewOf(v->atlas);
            v->shadowWidth = width;
            v->shadowHeight = height;
            ++v->stamp;
        }
        if (!syncShadowList(v, scene)) return false;
        /* each view's push constants, at its offset: its number, where Vulkan's sits (four bytes in) and before it */
        uint32_t push[ARTF_SHADOW_VIEWS * PUSH_STRIDE / 4];
        memset(push, 0, sizeof push);
        for (uint32_t i = 0; i < shadowViews; ++i) push[i * PUSH_STRIDE / 4] = push[i * PUSH_STRIDE / 4 + 1] = i;
        bufferPut(v, &v->push, 0, push, (size_t)shadowViews * PUSH_STRIDE);
    }
    if (!v->atlasView) {
        /* (a map to bind where there is none: one texel) */
        v->atlas = textureNew(v, WGPUTextureFormat_Depth32Float, 1, 1, 1, 1, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding);
        v->atlasView = viewOf(v->atlas);
        ++v->stamp;
    }

    const uint32_t meshCount = (uint32_t)v->meshes.len, instanceCount = tracing ? 0 : (uint32_t)scene->instances.len;
    bool probeStep = false;
    if (!tracing && frame.indirect[0][3] > 0 && !probesSync(v, scene, &probeStep)) return false;
    if (frame.traced & ARTF_TRACED_REFLECTIONS &&
        (!v->reflectionTraced.compute || v->reflectionGraphs != scene->graphs.len || (scene->graphs.len && v->reflectionScene != scene->id))) {
        programFree(&v->reflectionTraced);
        size_t size = 0;
        uint32_t* code = artfShaderPostReflectionTraced(&size, v->maxTextures | ARTF_TEXTURES_UNIFORM, scene->graphs.items, (uint32_t)scene->graphs.len, false);
        char* text = wgslOf(code, size, false, &v->reflectionTraced.bindings);
        if (!computeMake(v, &v->reflectionTraced, text)) artfLog("artful: WebGPU's reflections from rays could not be made: from the screen");
        v->reflectionGraphs = (uint32_t)scene->graphs.len;
        v->reflectionScene = scene->id;
    }
    bool traceClear = false, traceShown = false;
    uint32_t traceAdding = 0;
    if (tracing) {
        /* what tracing gathers (vulkan.c's): from the start when what it sees changed, unless the filter finds what holds */
        const ArtfTraceDesc* d = &scene->trace.desc;
        const bool more = scene->moreMaterials > 0 || artfSceneShapedLights(scene);
        if (!traceSync(v, scene, d->denoise) || !tracePrograms(v, scene, d->denoise, more)) return false;
        const uint64_t key = artfSceneTraceKey(scene, &frame);
        if (key != v->traceKey || !v->traceDone) {
            traceClear = !d->denoise;
            v->traceDone = 0;
            v->traceKey = key;
        }
        traceAdding = (uint32_t)d->samples;
        if (d->maxSamples > 0) traceAdding = v->traceDone >= (uint32_t)d->maxSamples ? 0 : (uint32_t)artfMini(d->samples, d->maxSamples - (int)v->traceDone);
        if (d->denoise && traceAdding) traceClear = true;
        ArtfTraceFrame* traced = artfMalloc(sizeof *traced);
        artfSceneTraceFrame(scene, &frame, v->sceneWidth, v->sceneHeight, d->denoise ? v->traceFrames : v->traceDone, traced);
        traced->pad[0] = d->denoise ? 1 : v->traceDone + traceAdding;
        traced->pad[1] = (v->denoiseFirst ? 1u : 0u) | (d->denoise && !v->denoiseFirst && !memcmp(v->tracePrevious, frame.viewProj, sizeof v->tracePrevious) ? 2u : 0u);
        memcpy(traced->previous, v->tracePrevious, sizeof traced->previous);
        bufferPut(v, &v->traceFrameUbo, 0, traced, sizeof *traced);
        artfFree(traced);
        v->traceDone += traceAdding;
        if (d->denoise && traceAdding) {
            v->traceFrames += traceAdding;
            v->denoiseFirst = false;
            memcpy(v->tracePrevious, frame.viewProj, sizeof v->tracePrevious);
        }
        traceShown = v->traceDone || d->denoise;
    }
    const bool cutouts = scene->cutoutCount != 0 || scene->features[ARTF_FEATURE_LOD_FADE];
    if (instanceCount) bufferPut(v, &v->indirect, 0, v->templateCpu, (size_t)meshCount * 80);

    WGPUCommandEncoder c = wgpuDeviceCreateCommandEncoder(v->device, NULL);
    if (tracing) traceRecord(v, c, traceClear, traceAdding != 0, scene->trace.desc.denoise);
    if (probeStep) probesRecord(v, c, scene);
    if (instanceCount) {
        WGPUComputePassEncoder e = wgpuCommandEncoderBeginComputePass(c, NULL);
        wgpuComputePassEncoderSetPipeline(e, v->cull.compute);
        wgpuComputePassEncoderSetBindGroup(e, 0, groupOf(v, &v->cull), 0, NULL);
        wgpuComputePassEncoderDispatchWorkgroups(e, (instanceCount + 63) / 64, 1, 1);
        wgpuComputePassEncoderEnd(e);
        wgpuComputePassEncoderRelease(e);
    }
    if (shadowViews && instanceCount) drawShadows(v, c, &shadows, shadowViews);
    const bool fogGrid = v->froxels && frame.post[1][1] > 0 && frame.look[1] > 0;
    if (fogGrid) {
        /* fog that light goes through: its grid, from the sun's maps, for what is drawn to read */
        WGPUComputePassEncoder g = wgpuCommandEncoderBeginComputePass(c, NULL);
        postStep(v, g, POST_FROXELS, 0, (uint32_t)((v->sceneWidth + ARTF_FROXEL_TILE - 1) / ARTF_FROXEL_TILE) * (uint32_t)((v->sceneHeight + ARTF_FROXEL_TILE - 1) / ARTF_FROXEL_TILE));
        wgpuComputePassEncoderEnd(g);
        wgpuComputePassEncoderRelease(g);
    }

    WGPURenderPassColorAttachment colors[2] = {WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT, WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT};
    WGPURenderPassColorAttachment* const color = &colors[0];
    color->view = v->samples > 1 ? v->samplesView : v->frameView;
    color->resolveTarget = v->samples > 1 ? v->frameView : NULL;
    color->loadOp = WGPULoadOp_Clear;
    color->storeOp = v->samples > 1 ? WGPUStoreOp_Discard : WGPUStoreOp_Store;
    color->clearValue.r = frame.background[0];
    color->clearValue.g = frame.background[1];
    color->clearValue.b = frame.background[2];
    color->clearValue.a = frame.background[3];
    WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    depth.view = v->depthView;
    depth.depthLoadOp = WGPULoadOp_Clear;
    depth.depthStoreOp = v->floats ? WGPUStoreOp_Store : WGPUStoreOp_Discard;
    depth.depthClearValue = 0.f;
    if (v->floats) {
        /* the light (the background as light, as on the CPU) and the surface words; what is drawn into is kept, for
         * the gather and the layers' pass after it */
        color->view = v->samples > 1 ? v->floatSamplesView : v->floatColorView;
        color->resolveTarget = v->samples > 1 ? v->floatColorView : NULL;
        color->storeOp = WGPUStoreOp_Store;
        const float alpha = frame.background[3], over = alpha > 0 ? 1.f / alpha : 0.f;
        color->clearValue.r = artfSrgbDecode(frame.background[0] * over) * alpha;
        color->clearValue.g = artfSrgbDecode(frame.background[1] * over) * alpha;
        color->clearValue.b = artfSrgbDecode(frame.background[2] * over) * alpha;
        colors[1].view = v->floatSurfaceView;
        colors[1].loadOp = WGPULoadOp_Clear;
        colors[1].storeOp = WGPUStoreOp_Store;
    }
    WGPURenderPassDescriptor pass = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    pass.colorAttachmentCount = v->floats ? 2 : 1;
    pass.colorAttachments = colors;
    pass.depthStencilAttachment = &depth;
    WGPURenderPassEncoder e = wgpuCommandEncoderBeginRenderPass(c, &pass);
    if (frame.sky[3] > 0 && frame.ground[3] > 0 && v->environmentBehind.render) { /* (light from all around, behind) */
        use(v, e, &v->environmentBehind, 0);
        wgpuRenderPassEncoderDraw(e, 3, 1, 0, 0);
    }
    if (fogGrid && v->fogBehind.render) {
        use(v, e, &v->fogBehind, 0);
        wgpuRenderPassEncoderDraw(e, 3, 1, 0, 0);
    }
    if (traceShown && v->traceShow.render) {
        use(v, e, &v->traceShow, 0);
        wgpuRenderPassEncoderDraw(e, 3, 1, 0, 0);
    }
    if (instanceCount) {
        wgpuRenderPassEncoderSetVertexBuffer(e, 0, v->vertices.buffer, 0, v->vertices.size);
        wgpuRenderPassEncoderSetIndexBuffer(e, v->indices.buffer, WGPUIndexFormat_Uint32, 0, v->indices.size);
        use(v, e, &v->scene, 0);
        drawCommands(v, e, v->indirect.buffer, 0, meshCount);
        if (cutouts) {
            use(v, e, &v->cutout, 0);
            drawCommands(v, e, v->indirect.buffer, meshCount * 2, meshCount);
        }
        if (v->floats) {
            /* (what comes after reads the depth of what is solid: taken now, before layers write theirs; then the
             * layers in a pass that keeps what was drawn) */
            wgpuRenderPassEncoderEnd(e);
            wgpuRenderPassEncoderRelease(e);
            gatherDepth(v, c);
            color->loadOp = colors[1].loadOp = WGPULoadOp_Load;
            depth.depthLoadOp = WGPULoadOp_Load;
            e = wgpuCommandEncoderBeginRenderPass(c, &pass);
            wgpuRenderPassEncoderSetVertexBuffer(e, 0, v->vertices.buffer, 0, v->vertices.size);
            wgpuRenderPassEncoderSetIndexBuffer(e, v->indices.buffer, WGPUIndexFormat_Uint32, 0, v->indices.size);
        }
        /* the layers, far to near: each its depth alone, then itself at that depth; particles of one quad, at once */
        for (uint32_t i = 0; i < layerCount; ++i) {
            const MeshRange* m = &v->meshes.items[v->layers.items[i].mesh];
            if (!m->indexCount) continue;
            const bool particle = (scene->instances.items[v->layers.items[i].instance].flags & ARTF_INSTANCE_PARTICLE) != 0;
            if (particle) {
                uint32_t end = i + 1;
                while (end < layerCount && v->layers.items[end].mesh == v->layers.items[i].mesh &&
                       (scene->instances.items[v->layers.items[end].instance].flags & ARTF_INSTANCE_PARTICLE))
                    ++end;
                use(v, e, &v->particle, 0);
                wgpuRenderPassEncoderDrawIndexed(e, m->indexCount, end - i, m->firstIndex, m->vertexOffset, layersAt(v) + i);
                i = end - 1;
                continue;
            }
            use(v, e, &v->layerDepth, 0);
            wgpuRenderPassEncoderDrawIndexed(e, m->indexCount, 1, m->firstIndex, m->vertexOffset, layersAt(v) + i);
            use(v, e, &v->layer, 0);
            wgpuRenderPassEncoderDrawIndexed(e, m->indexCount, 1, m->firstIndex, m->vertexOffset, layersAt(v) + i);
        }
    }
    if (v->overlayPixels && v->overlayUsable && !v->floats) {
        use(v, e, &v->overlayDraw, 0);
        wgpuRenderPassEncoderDraw(e, 3, 1, 0, 0);
    }
    wgpuRenderPassEncoderEnd(e);
    wgpuRenderPassEncoderRelease(e);
    if (v->floats) {
        if (!instanceCount) gatherDepth(v, c);
        postChain(v, c, &frame, under, clouds, cloudColumns, cloudRows);
        finalPass(v, c);
    }
    if (v->pushCount) bufferPut(v, &v->push, (uint64_t)PUSH_POST_AT * PUSH_STRIDE, v->pushes, (size_t)v->pushCount * PUSH_STRIDE);

    WGPUSurfaceTexture shown = WGPU_SURFACE_TEXTURE_INIT;
    WGPUTextureView shownView = NULL;
    if (v->surface && v->surfaceReady) {
        wgpuSurfaceGetCurrentTexture(v->surface, &shown);
        if (shown.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal || shown.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
            shownView = viewOf(shown.texture);
            WGPURenderPassColorAttachment to = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
            to.view = shownView;
            to.loadOp = WGPULoadOp_Clear;
            to.storeOp = WGPUStoreOp_Store;
            WGPURenderPassDescriptor bp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            bp.colorAttachmentCount = 1;
            bp.colorAttachments = &to;
            WGPURenderPassEncoder b = wgpuCommandEncoderBeginRenderPass(c, &bp);
            wgpuRenderPassEncoderSetPipeline(b, v->blit.render);
            /* (the blit's one binding: the frame, made again each time, as it is cheap) */
            WGPUBindGroupEntry entry = WGPU_BIND_GROUP_ENTRY_INIT;
            entry.binding = 0;
            entry.textureView = v->frameView;
            WGPUBindGroupDescriptor gd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            gd.layout = v->blit.layout;
            gd.entryCount = 1;
            gd.entries = &entry;
            WGPUBindGroup group = wgpuDeviceCreateBindGroup(v->device, &gd);
            wgpuRenderPassEncoderSetBindGroup(b, 0, group, 0, NULL);
            wgpuRenderPassEncoderDraw(b, 3, 1, 0, 0);
            wgpuRenderPassEncoderEnd(b);
            wgpuRenderPassEncoderRelease(b);
            wgpuBindGroupRelease(group);
        }
    }
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(c, NULL);
    wgpuQueueSubmit(v->queue, 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(c);
    if (shownView) {
#ifndef __EMSCRIPTEN__
        wgpuSurfacePresent(v->surface); /* (a browser shows the canvas when the frame's task ends) */
#endif
        wgpuTextureViewRelease(shownView);
    }
    if (shown.texture) wgpuTextureRelease(shown.texture);
    ++v->frameNumber;
    return true;
}

static bool webgpuDraw(ArtfRenderer* r, ArtfScene* scene) {
    WebGpu* v = (WebGpu*)r;
    const double start = artfNowMs();
    artfSceneScaleStep(scene, v->width, v->height);
    const bool ok = frameDraw(v, scene);
    v->cpuMs = artfNowMs() - start;
    return ok;
}

static bool mapped;
static void gotMapped(WGPUMapAsyncStatus status, WGPUStringView message, void* user, void* unused) {
    (void)message; (void)unused;
    *(bool*)user = status == WGPUMapAsyncStatus_Success;
    mapped = true;
}

static bool webgpuReadPixels(ArtfRenderer* r, uint32_t* dst) {
    WebGpu* v = (WebGpu*)r;
    if (!v->frameNumber || !v->frameTexture) return false;
    const uint32_t row = ((uint32_t)v->width * 4 + 255) & ~255u; /* (WebGPU's rows are 256 bytes apart at least) */
    Gpu readback;
    memset(&readback, 0, sizeof readback);
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.size = (uint64_t)row * (uint64_t)v->height;
    bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    readback.buffer = wgpuDeviceCreateBuffer(v->device, &bd);
    if (!readback.buffer) return false;
    WGPUCommandEncoder c = wgpuDeviceCreateCommandEncoder(v->device, NULL);
    WGPUTexelCopyTextureInfo from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    from.texture = v->frameTexture;
    WGPUTexelCopyBufferInfo to = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    to.buffer = readback.buffer;
    to.layout.bytesPerRow = row;
    to.layout.rowsPerImage = (uint32_t)v->height;
    const WGPUExtent3D size = {(uint32_t)v->width, (uint32_t)v->height, 1};
    wgpuCommandEncoderCopyTextureToBuffer(c, &from, &to, &size);
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(c, NULL);
    wgpuQueueSubmit(v->queue, 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(c);
    bool ok = false;
    mapped = false;
    WGPUBufferMapCallbackInfo info = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    info.mode = WGPUCallbackMode_AllowSpontaneous;
    info.callback = gotMapped;
    info.userdata1 = &ok;
    wgpuBufferMapAsync(readback.buffer, WGPUMapMode_Read, 0, bd.size, info);
    if (waitFor(v, &mapped) && ok) {
        const uint8_t* bytes = wgpuBufferGetConstMappedRange(readback.buffer, 0, bd.size);
        for (int y = 0; y < v->height; ++y) memcpy(dst + (size_t)y * v->width, bytes + (size_t)y * row, (size_t)v->width * 4);
        wgpuBufferUnmap(readback.buffer);
    }
    bufferFree(&readback);
    return ok;
}

static void webgpuResize(ArtfRenderer* r, int width, int height) {
    WebGpu* v = (WebGpu*)r;
    v->width = width;
    v->height = height;
    v->targetsStale = true;
}

static uint32_t* webgpuPixels(ArtfRenderer* r) { (void)r; return NULL; }
static const char* webgpuDescription(const ArtfRenderer* r) { return ((const WebGpu*)r)->description; }
static void webgpuStats(const ArtfRenderer* r, ArtfFrameStats* out) {
    memset(out, 0, sizeof *out);
    out->cpuMs = ((const WebGpu*)r)->cpuMs;
}

static void programFree(Program* p) {
    if (p->group) wgpuBindGroupRelease(p->group);
    if (p->render == NULL && p->compute == NULL && p->layout == NULL) {
        memset(p, 0, sizeof *p);
        return;
    }
    for (int k = 0; k < 27; ++k)
        if (p->roleGroups[k]) wgpuBindGroupRelease(p->roleGroups[k]);
    if (p->layout1) wgpuBindGroupLayoutRelease(p->layout1);
    if (p->render) wgpuRenderPipelineRelease(p->render);
    if (p->compute) wgpuComputePipelineRelease(p->compute);
    if (p->pipelineLayout) wgpuPipelineLayoutRelease(p->pipelineLayout);
    if (p->layout) wgpuBindGroupLayoutRelease(p->layout);
    memset(p, 0, sizeof *p);
}

static void webgpuFree(ArtfRenderer* r) {
    WebGpu* v = (WebGpu*)r;
    targetsFree(v);
    releaseTexture(&v->atlas, &v->atlasView);
    releaseTexture(&v->white, &v->whiteView);
    for (size_t i = 0; i < v->textures.len; ++i) releaseTexture(&v->textures.items[i], &v->textureViews.items[i]);
    Gpu* buffers[] = {&v->empty, &v->emptyWritable, &v->frame, &v->shadowData, &v->push, &v->instances, &v->visible, &v->indirect, &v->materials, &v->meshTable, &v->vertices,
                      &v->indices, &v->colors, &v->shadowList, &v->shadowCommands, &v->meshletMeshes, &v->meshletState, &v->extraLights, &v->tileLights, &v->decals};
    for (size_t i = 0; i < ARTF_COUNT(buffers); ++i) bufferFree(buffers[i]);
    Program* programs[] = {&v->cull, &v->scene, &v->cutout, &v->overlayDraw, &v->shadow, &v->shadowHoles, &v->blit, &v->layerDepth, &v->layer, &v->particle, &v->final, &v->overlayFloats, &v->fogBehind, &v->environmentBehind, &v->trace, &v->traceShow, &v->probeStep, &v->reflectionTraced};
    for (size_t i = 0; i < ARTF_COUNT(programs); ++i) programFree(programs[i]);
    for (int k = 0; k < POST_KINDS; ++k) programFree(&v->posts[k]);
    for (int k = 0; k < 8; ++k) bufferFree(&v->post[k]);
    bufferFree(&v->postLocks);
    bufferFree(&v->postGathered);
    bufferFree(&v->cloudField);
    bufferFree(&v->cloudGrid);
    bufferFree(&v->environment);
    for (int k = 0; k < 9; ++k) programFree(&v->denoisePasses[k]);
    Gpu* traced[] = {&v->traceNodes, &v->traceTriangles, &v->traceVertices, &v->traceTop, &v->traceInstances, &v->traceSum, &v->traceFrameUbo, &v->denoiseShift, &v->traceMotion};
    for (size_t k = 0; k < ARTF_COUNT(traced); ++k) bufferFree(traced[k]);
    for (int k = 0; k < 7; ++k) bufferFree(&v->denoiseBuffers[k]);
    bufferFree(&v->probesNow);
    bufferFree(&v->probesNext);
    artfMotionFree(&v->motion);
    artfFree(v->pushes);
    if (v->linear) wgpuSamplerRelease(v->linear);
    if (v->nearest) wgpuSamplerRelease(v->nearest);
    if (v->surface) wgpuSurfaceRelease(v->surface);
    if (v->queue) wgpuQueueRelease(v->queue);
    if (v->device) wgpuDeviceRelease(v->device);
    if (v->adapter) wgpuAdapterRelease(v->adapter);
    if (v->instance) wgpuInstanceRelease(v->instance);
    artfFree(v->templateCpu);
    artfFree(v->lightScratch);
    artfFree(v->listScratch);
    artfFree(v->decalScratch);
    artfVecFree(&v->meshes);
    artfVecFree(&v->layers);
    artfVecFree(&v->textures);
    artfVecFree(&v->textureViews);
    artfVecFree(&v->textureMarks);
    artfVecFree(&v->copyStarts);
    artfFree(v);
}

static const ArtfRendererVtbl webgpuVtbl = {webgpuDescription, webgpuResize, webgpuSetOverlay, webgpuDraw, webgpuReadPixels, webgpuPixels, webgpuStats, webgpuFree};

/* The surface to show frames on, where the options name one: a window's, or a canvas's in a page. */
static WGPUSurface surfaceOf(WebGpu* v, const ArtfSurface* s) {
    WGPUSurfaceDescriptor d = WGPU_SURFACE_DESCRIPTOR_INIT;
#ifdef __EMSCRIPTEN__
    if (s->kind != ARTF_SURFACE_WEB_CANVAS) return NULL;
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    canvas.selector.data = s->a;
    canvas.selector.length = strlen(s->a);
    d.nextInChain = &canvas.chain;
    return wgpuInstanceCreateSurface(v->instance, &d);
#elif defined(_WIN32)
    if (s->kind != ARTF_SURFACE_WIN32) return NULL;
    WGPUSurfaceSourceWindowsHWND window = WGPU_SURFACE_SOURCE_WINDOWS_HWND_INIT;
    window.hwnd = s->a;
    window.hinstance = s->b;
    d.nextInChain = &window.chain;
    return wgpuInstanceCreateSurface(v->instance, &d);
#else
    (void)v; (void)d; (void)s;
    return NULL;
#endif
}

ArtfRenderer* artfWebGpuNew(const ArtfRendererOptions* o) {
    WebGpu* v = artfCalloc(1, sizeof *v);
    v->base.vt = &webgpuVtbl;
    v->base.backend = ARTF_BACKEND_WEBGPU;
    v->width = o->width;
    v->height = o->height;
    v->samples = o->antialias ? 4 : 1;
    v->floats = o->hdr;
    v->scale = v->base.renderScale = o->hdr && o->renderScale > 0 && o->renderScale < 1.f ? artfMaxf(o->renderScale, .25f) : 1.f;
    v->base.renderScaleMost = v->scale;
    v->pushes = artfCalloc((size_t)PUSH_POST_MOST * PUSH_STRIDE, 1);
    v->meshesUp = ~(size_t)0;
    v->targetsStale = true;
    v->instance = wgpuCreateInstance(NULL);
    if (!v->instance) goto failed;
    if (o->surface.kind != ARTF_SURFACE_NONE && !(v->surface = surfaceOf(v, &o->surface))) {
        artfLog("artful: WebGPU cannot show frames on that surface");
        goto failed;
    }
    adapterGot = deviceGot = false;
    WGPURequestAdapterOptions ao = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    ao.powerPreference = WGPUPowerPreference_HighPerformance;
    ao.compatibleSurface = v->surface;
    WGPURequestAdapterCallbackInfo ai = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    ai.mode = WGPUCallbackMode_AllowSpontaneous;
    ai.callback = gotAdapter;
    ai.userdata1 = v;
    wgpuInstanceRequestAdapter(v->instance, &ao, ai);
    if (!waitFor(v, &adapterGot) || !v->adapter) goto failed;
    /* what it needs: indirect draws that say their first instance; and as much as the adapter has, for textures */
    if (!wgpuAdapterHasFeature(v->adapter, WGPUFeatureName_IndirectFirstInstance)) {
        artfLog("artful: this WebGPU adapter lacks indirect-first-instance");
        goto failed;
    }
    WGPULimits limits = WGPU_LIMITS_INIT;
    if (wgpuAdapterGetLimits(v->adapter, &limits) != WGPUStatus_Success) goto failed;
    /* (the textures of the array: as many as a stage may have, but for the shadows' map and the overlay) */
    v->maxTextures = limits.maxSampledTexturesPerShaderStage > 4 ? limits.maxSampledTexturesPerShaderStage - 2 : 1;
    v->maxGroups = limits.maxComputeWorkgroupsPerDimension ? limits.maxComputeWorkgroupsPerDimension : 65535;
    if (v->maxTextures > 64) v->maxTextures = 64;
    const WGPUFeatureName features[1] = {WGPUFeatureName_IndirectFirstInstance};
    WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
    dd.requiredFeatureCount = 1;
    dd.requiredFeatures = features;
    dd.requiredLimits = &limits;
    dd.uncapturedErrorCallbackInfo.callback = gpuError;
    WGPURequestDeviceCallbackInfo di = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    di.mode = WGPUCallbackMode_AllowSpontaneous;
    di.callback = gotDevice;
    di.userdata1 = v;
    wgpuAdapterRequestDevice(v->adapter, &dd, di);
    if (!waitFor(v, &deviceGot) || !v->device) goto failed;
    v->queue = wgpuDeviceGetQueue(v->device);
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    wgpuAdapterGetInfo(v->adapter, &info);
    /* (a browser names no device: its vendor and architecture, then) */
    if (info.device.length) snprintf(v->description, sizeof v->description, "%.*s (WebGPU)", (int)info.device.length, info.device.data);
    else snprintf(v->description, sizeof v->description, "%.*s %.*s (WebGPU)", (int)info.vendor.length, info.vendor.data ? info.vendor.data : "", (int)info.architecture.length,
                  info.architecture.data ? info.architecture.data : "");
    wgpuAdapterInfoFreeMembers(info);
    if (v->surface) {
        WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
        wgpuSurfaceGetCapabilities(v->surface, v->adapter, &caps);
        v->surfaceFormat = caps.formatCount ? caps.formats[0] : WGPUTextureFormat_BGRA8Unorm;
        for (size_t k = 0; k < caps.formatCount; ++k)
            if (caps.formats[k] == WGPUTextureFormat_BGRA8Unorm || caps.formats[k] == WGPUTextureFormat_RGBA8Unorm) v->surfaceFormat = caps.formats[k];
        wgpuSurfaceCapabilitiesFreeMembers(caps);
    }
    v->white = textureNew(v, WGPUTextureFormat_BGRA8Unorm, 1, 1, 1, 1, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst);
    const uint32_t white = ~(uint32_t)0;
    texturePut(v, v->white, 0, 0, 0, 1, 1, &white, 1);
    v->whiteView = viewOf(v->white);
    if (!bufferNew(v, &v->empty, BLANK_BYTES, STORAGE | UNIFORM) || !bufferNew(v, &v->emptyWritable, BLANK_BYTES, STORAGE) || !bufferNew(v, &v->frame, sizeof(ArtfFrameData), UNIFORM) ||
        !bufferNew(v, &v->shadowData, sizeof(ArtfShadowFrame), UNIFORM) || !bufferNew(v, &v->push, (uint64_t)(ARTF_SHADOW_VIEWS + PUSH_POST_MOST) * PUSH_STRIDE, UNIFORM) ||
        !bufferNew(v, &v->extraLights, (uint64_t)ARTF_MAX_LIGHTS * sizeof(ArtfLightData) + (uint64_t)ARTF_FRAME_LIGHTS * ARTF_PROFILE_FLOATS * sizeof(float), STORAGE) || !bufferNew(v, &v->tileLights, 64, STORAGE) ||
        !bufferNew(v, &v->decals, (uint64_t)ARTF_MAX_DECALS * sizeof(ArtfDecalData), STORAGE) || !bufferNew(v, &v->shadowList, 64, STORAGE) ||
        !bufferNew(v, &v->shadowCommands, 64, WGPUBufferUsage_Indirect))
        goto failed;
    uint8_t* zeros = artfCalloc(BLANK_BYTES, 1);
    bufferPut(v, &v->empty, 0, zeros, BLANK_BYTES);
    bufferPut(v, &v->emptyWritable, 0, zeros, BLANK_BYTES);
    artfFree(zeros);
    if (!pipelinesMake(v) || !targetsMake(v)) {
        artfLog("artful: WebGPU's pipelines could not be made");
        goto failed;
    }
    return &v->base;
failed:
    webgpuFree(&v->base);
    return NULL;
}
