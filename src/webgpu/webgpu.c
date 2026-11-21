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
 * What it leaves out, it says in the log: a frame in floats and what follows it, light from all around and from
 * probes, layers of transparency, tracing, materials in C and views. */
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
    uint64_t stamp; /* the resources' version: bind groups are made again when it moves */
    double cpuMs;
    uint64_t frameNumber;
    bool warnedLayers, warnedTrace, warnedGraphs, warnedFloats, warnedView;

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
    case 25: return v->atlasView;
    default: return v->whiteView;
    }
}

/* The layout, from what the shaders bind (compute: their stage; else the vertex's and the pixel's). */
static bool layoutMake(WebGpu* v, Program* p, bool compute) {
    WGPUBindGroupLayoutEntry entries[IR_WGSL_BINDINGS];
    const int count = p->bindings.count;
    for (int k = 0; k < count; ++k) {
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
        case IR_WGSL_SAMPLER: e.sampler.type = b->source == 25 ? WGPUSamplerBindingType_NonFiltering : WGPUSamplerBindingType_Filtering; break;
        default:
            e.texture.sampleType = b->kind == IR_WGSL_TEXTURE_UINT ? WGPUTextureSampleType_Uint : b->source == 25 ? WGPUTextureSampleType_UnfilterableFloat : WGPUTextureSampleType_Float;
            e.texture.viewDimension = WGPUTextureViewDimension_2D;
            e.texture.multisampled = b->kind == IR_WGSL_TEXTURE_MULTISAMPLED;
            break;
        }
        entries[k] = e;
    }
    WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    d.entryCount = (size_t)count;
    d.entries = entries;
    p->layout = wgpuDeviceCreateBindGroupLayout(v->device, &d);
    WGPUPipelineLayoutDescriptor pd = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pd.bindGroupLayoutCount = 1;
    pd.bindGroupLayouts = &p->layout;
    p->pipelineLayout = wgpuDeviceCreatePipelineLayout(v->device, &pd);
    return p->layout && p->pipelineLayout;
}

/* Its bind group, as the resources now are. */
static WGPUBindGroup groupOf(WebGpu* v, Program* p) {
    if (p->group && p->stamp == v->stamp) return p->group;
    if (p->group) wgpuBindGroupRelease(p->group);
    WGPUBindGroupEntry entries[IR_WGSL_BINDINGS];
    for (int k = 0; k < p->bindings.count; ++k) {
        const IrWgslBinding* b = &p->bindings.items[k];
        WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
        e.binding = b->binding;
        switch (b->kind) {
        case IR_WGSL_UNIFORM: case IR_WGSL_STORAGE_READ: case IR_WGSL_STORAGE: {
            const Gpu* g = bufferOrEmpty(v, b->source, b->kind == IR_WGSL_STORAGE);
            e.buffer = g->buffer;
            e.size = b->source == IR_WGSL_PUSH_AT ? 16 : g->size;
            break;
        }
        case IR_WGSL_SAMPLER: e.sampler = b->source == 25 ? v->nearest : v->linear; break;
        default: e.textureView = textureAt(v, b->source, b->element); break;
        }
        entries[k] = e;
    }
    WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    d.layout = p->layout;
    d.entryCount = (size_t)p->bindings.count;
    d.entries = entries;
    p->group = wgpuDeviceCreateBindGroup(v->device, &d);
    p->stamp = v->stamp;
    return p->group;
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
    if (st->target == 0 || st->shadow) {
        depth.format = WGPUTextureFormat_Depth32Float;
        depth.depthCompare = st->depth;
        depth.depthWriteEnabled = st->depthWrite ? WGPUOptionalBool_True : WGPUOptionalBool_False;
        d.depthStencil = &depth;
    }
    d.multisample.count = st->shadow || st->target ? 1u : (uint32_t)v->samples;
    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    blend.color.srcFactor = blend.alpha.srcFactor = WGPUBlendFactor_One;
    blend.color.dstFactor = blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    blend.color.operation = blend.alpha.operation = WGPUBlendOperation_Add;
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = st->target ? st->target : WGPUTextureFormat_BGRA8Unorm;
    target.writeMask = st->color ? WGPUColorWriteMask_All : WGPUColorWriteMask_None;
    if (st->blend) target.blend = &blend;
    WGPUFragmentState fragmentState = WGPU_FRAGMENT_STATE_INIT;
    if (fragment || !st->shadow) {
        fragmentState.module = fragment;
        fragmentState.entryPoint.data = "main";
        fragmentState.entryPoint.length = 4;
        if (!st->shadow) {
            fragmentState.targetCount = 1;
            fragmentState.targets = &target;
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
    RENDER(&v->scene, artfShaderSceneVertex(&size, false), false, artfShaderSceneFragment(&size, textures, false, NULL, 0, 0, 0), {WGPUCompareFunction_Greater, true, false, true, true, false, WGPUCullMode_Back, 0});
    RENDER(&v->cutout, artfShaderSceneVertex(&size, false), false, artfShaderSceneFragment(&size, textures, true, NULL, 0, 0, 0), {WGPUCompareFunction_Greater, true, false, true, true, false, WGPUCullMode_None, 0});
    RENDER(&v->overlayDraw, artfShaderOverlayVertex(&size), false, artfShaderOverlayFragment(&size, false), {WGPUCompareFunction_Always, false, true, true, false, false, WGPUCullMode_None, 0});
    RENDER(&v->shadow, artfShaderShadowVertex(&size), true, (uint32_t*)NULL, {WGPUCompareFunction_Less, true, false, false, true, true, WGPUCullMode_None, 0});
    RENDER(&v->shadowHoles, artfShaderShadowVertex(&size), true, artfShaderShadowFragment(&size, textures), {WGPUCompareFunction_Less, true, false, false, true, true, WGPUCullMode_None, 0});
#undef RENDER
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
}

static bool targetsMake(WebGpu* v) {
    targetsFree(v);
    v->targetsStale = false;
    if (v->width <= 0 || v->height <= 0) return true;
    v->frameTexture = textureNew(v, WGPUTextureFormat_BGRA8Unorm, v->width, v->height, 1, 1, WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc | WGPUTextureUsage_TextureBinding);
    v->depthTexture = textureNew(v, WGPUTextureFormat_Depth32Float, v->width, v->height, 1, v->samples, WGPUTextureUsage_RenderAttachment);
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
        if (!bufferNew(v, &v->instances, (uint64_t)v->instanceCapacity * sizeof(ArtfInstanceData), STORAGE)) return false;
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

static bool frameDraw(WebGpu* v, ArtfScene* scene) {
    if (v->targetsStale && !targetsMake(v)) return false;
    if (!v->frameTexture) return true; /* (a window with no size) */
#define ONCE(flag, ...) do { if (!v->flag) { v->flag = true; artfLog(__VA_ARGS__); } } while (0)
    if (scene->graphs.len) ONCE(warnedGraphs, "artful: WebGPU does not run materials written in C yet: they show their plain values");
    if (scene->trace.desc.mode != ARTF_TRACE_NONE) ONCE(warnedTrace, "artful: WebGPU does not trace: the scene is drawn");
    if (scene->translucentCount) ONCE(warnedLayers, "artful: WebGPU does not draw layers of transparency yet: they are left out");
    if (v->base.view) ONCE(warnedView, "artful: WebGPU shows no views but the frame");
#undef ONCE
    if (!syncScene(v, scene)) return false;
    ArtfFrameData frame;
    artfSceneFrame(scene, v->width, v->height, &frame);
    frame.output[0] = (float)v->width;
    frame.output[1] = (float)v->height;
    /* (what is left out here: light from all around and from probes, lights' shapes, rays, and all a frame in floats has) */
    frame.sky[3] = frame.ground[3] = 0;
    memset(frame.lightShapes, 0, sizeof frame.lightShapes);
    memset(frame.indirect, 0, sizeof frame.indirect);
    memset(frame.post, 0, sizeof frame.post);
    frame.traced = frame.tracedCasts = 0;
    if (!v->decalScratch) v->decalScratch = artfMalloc(ARTF_MAX_DECALS * sizeof(ArtfDecalData));
    const uint32_t decals = artfSceneDecals(scene, &frame, v->height, v->decalScratch);
    frame.indirect[1][3] = (float)decals;
    bufferPut(v, &v->decals, 0, v->decalScratch, (size_t)decals * sizeof(ArtfDecalData));
    if (!syncLights(v, scene, &frame)) return false;
    artfSceneResidencyStep(scene, &frame, v->height);
    bufferPut(v, &v->frame, 0, &frame, sizeof frame);
    uploadOverlay(v);

    ArtfShadowFrame shadows;
    memset(&shadows, 0, sizeof shadows);
    const uint32_t shadowViews = artfSceneShadowFrame(scene, &frame, &shadows);
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

    const uint32_t meshCount = (uint32_t)v->meshes.len, instanceCount = (uint32_t)scene->instances.len;
    const bool cutouts = scene->cutoutCount != 0 || scene->features[ARTF_FEATURE_LOD_FADE];
    if (instanceCount) bufferPut(v, &v->indirect, 0, v->templateCpu, (size_t)meshCount * 80);

    WGPUCommandEncoder c = wgpuDeviceCreateCommandEncoder(v->device, NULL);
    if (instanceCount) {
        WGPUComputePassEncoder e = wgpuCommandEncoderBeginComputePass(c, NULL);
        wgpuComputePassEncoderSetPipeline(e, v->cull.compute);
        wgpuComputePassEncoderSetBindGroup(e, 0, groupOf(v, &v->cull), 0, NULL);
        wgpuComputePassEncoderDispatchWorkgroups(e, (instanceCount + 63) / 64, 1, 1);
        wgpuComputePassEncoderEnd(e);
        wgpuComputePassEncoderRelease(e);
    }
    if (shadowViews && instanceCount) drawShadows(v, c, &shadows, shadowViews);

    WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = v->samples > 1 ? v->samplesView : v->frameView;
    color.resolveTarget = v->samples > 1 ? v->frameView : NULL;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = v->samples > 1 ? WGPUStoreOp_Discard : WGPUStoreOp_Store;
    color.clearValue.r = frame.background[0];
    color.clearValue.g = frame.background[1];
    color.clearValue.b = frame.background[2];
    color.clearValue.a = frame.background[3];
    WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    depth.view = v->depthView;
    depth.depthLoadOp = WGPULoadOp_Clear;
    depth.depthStoreOp = WGPUStoreOp_Discard;
    depth.depthClearValue = 0.f;
    WGPURenderPassDescriptor pass = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    pass.colorAttachmentCount = 1;
    pass.colorAttachments = &color;
    pass.depthStencilAttachment = &depth;
    WGPURenderPassEncoder e = wgpuCommandEncoderBeginRenderPass(c, &pass);
    if (instanceCount) {
        wgpuRenderPassEncoderSetVertexBuffer(e, 0, v->vertices.buffer, 0, v->vertices.size);
        wgpuRenderPassEncoderSetIndexBuffer(e, v->indices.buffer, WGPUIndexFormat_Uint32, 0, v->indices.size);
        use(v, e, &v->scene, 0);
        drawCommands(v, e, v->indirect.buffer, 0, meshCount);
        if (cutouts) {
            use(v, e, &v->cutout, 0);
            drawCommands(v, e, v->indirect.buffer, meshCount * 2, meshCount);
        }
    }
    if (v->overlayPixels && v->overlayUsable) {
        use(v, e, &v->overlayDraw, 0);
        wgpuRenderPassEncoderDraw(e, 3, 1, 0, 0);
    }
    wgpuRenderPassEncoderEnd(e);
    wgpuRenderPassEncoderRelease(e);

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
    Program* programs[] = {&v->cull, &v->scene, &v->cutout, &v->overlayDraw, &v->shadow, &v->shadowHoles, &v->blit};
    for (size_t i = 0; i < ARTF_COUNT(programs); ++i) programFree(programs[i]);
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
        !bufferNew(v, &v->shadowData, sizeof(ArtfShadowFrame), UNIFORM) || !bufferNew(v, &v->push, (uint64_t)ARTF_SHADOW_VIEWS * PUSH_STRIDE, UNIFORM) ||
        !bufferNew(v, &v->extraLights, (uint64_t)ARTF_MAX_LIGHTS * sizeof(ArtfLightData), STORAGE) || !bufferNew(v, &v->tileLights, 64, STORAGE) ||
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
