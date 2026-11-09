/* artful — the Metal backend, for macOS and iOS. NEVER RUN: this was written without a Mac. It builds, and its
 * shaders are Vulkan's own, translated (tools/mkmetal.py); what it does on a device nobody has seen yet.
 *
 * Plain C: Metal is called through the Objective-C runtime, both opened at run time, so there is nothing to link.
 * The frame is Vulkan's (see vulkan.c), made simpler by what Metal has:
 *
 *   memory is shared with the CPU, so nothing is staged, and a frame waits for the one before: one copy of all;
 *   a shader's resources are an argument buffer, written when a resource changes;
 *   what is drawn is drawn into a texture of its own, and copied to the window, so reading a frame is the same
 *   with or without one.
 *
 * What it does not have yet: occlusion (the picture is the same, without its savings), a black hole, materials
 * written in C (each shows its plain values) and compressed textures on the GPU (they go up decoded). It says so
 * in the log, once. It needs Metal 3, so that argument buffers hold a thousand textures. */
#include "render/rendererInternal.h"
#include "scene/sceneInternal.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef void* id;
typedef void* SEL;
typedef unsigned long NSUInteger;

typedef struct MTLSize { NSUInteger width, height, depth; } MTLSize;
typedef struct MTLOrigin { NSUInteger x, y, z; } MTLOrigin;
typedef struct MTLRegion { MTLOrigin origin; MTLSize size; } MTLRegion;
typedef struct MTLViewport { double x, y, width, height, near_, far_; } MTLViewport;
typedef struct MTLScissorRect { NSUInteger x, y, width, height; } MTLScissorRect;
typedef struct MTLClearColor { double red, green, blue, alpha; } MTLClearColor;
typedef struct NSRange { NSUInteger location, length; } NSRange;
typedef struct CGSize { double width, height; } CGSize;

enum {
    PIXEL_BGRA8 = 80, PIXEL_DEPTH32 = 252,
    STORAGE_SHARED = 0, STORAGE_MANAGED = 1, STORAGE_PRIVATE = 2,
    TEXTURE_READ = 1, TEXTURE_TARGET = 4, TEXTURE_2D = 2, TEXTURE_2D_MS = 4,
    LOAD_CLEAR = 2, STORE_DONT_CARE = 0, STORE_STORE = 1, STORE_RESOLVE = 2,
    COMPARE_LESS = 1, COMPARE_EQUAL = 2, COMPARE_GREATER = 4, COMPARE_ALWAYS = 7,
    BLEND_ONE = 1, BLEND_ONE_MINUS_SOURCE_ALPHA = 5,
    VERTEX_USHORT4_NORMALIZED = 21, VERTEX_SHORT2_NORMALIZED = 22, VERTEX_HALF2 = 25,
    PRIMITIVE_TRIANGLE = 3, INDEX_UINT32 = 1, WINDING_COUNTER_CLOCKWISE = 1, CULL_NONE = 0, CULL_BACK = 2,
    SAMPLER_NEAREST = 0, SAMPLER_LINEAR = 1, MIP_LINEAR = 2, ADDRESS_CLAMP = 0, ADDRESS_REPEAT = 2,
    USE_READ_WRITE = 3, STAGE_VERTEX_FRAGMENT = 3,
    STATUS_ERROR = 5,
    FAMILY_METAL3 = 5001, ARGUMENT_TIER2 = 1, LANGUAGE_3_0 = 3 << 16,
    VERTICES_AT = 30 /* where vertices are bound: past what the shaders take */
};

#define MAX_TEXTURES 1024
#define BINDINGS 64 /* (as many as Vulkan's set has: the shaders name them by binding) */
#define BLANK_BYTES 65536

/* ---- the runtime ---- */

static id (*objcClass)(const char*);
static SEL (*selName)(const char*);
static void (*objcSend)(void);
static void* (*poolPush)(void);
static void (*poolPop)(void*);
static id (*systemDevice)(void);

/* A message: what it returns, to whom, its name, the types of what it takes after the two every method takes,
 * and those. */
#define SEND(ret, to, name, types, ...) ((ret(*) types)objcSend)((to), selName(name), __VA_ARGS__)
#define GET(ret, to, name) ((ret(*)(id, SEL))objcSend)((to), selName(name))
#define DO(to, name) ((void (*)(id, SEL))objcSend)((to), selName(name))
#define SET_U(to, name, value) SEND(void, to, name, (id, SEL, NSUInteger), (NSUInteger)(value))
#define SET_B(to, name, value) SEND(void, to, name, (id, SEL, signed char), (signed char)(value))
#define SET_ID(to, name, value) SEND(void, to, name, (id, SEL, id), (id)(value))
#define AT(list, index) SEND(id, list, "objectAtIndexedSubscript:", (id, SEL, NSUInteger), (NSUInteger)(index))

static bool runtimeOpen(void) {
    if (objcSend) return true;
    void* objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_GLOBAL);
    void* metal = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW | RTLD_GLOBAL);
    /* (what names a window's layer and what is drawn in it) */
    dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW | RTLD_GLOBAL);
    if (!objc || !metal) return false;
    *(void**)&objcClass = dlsym(objc, "objc_getClass");
    *(void**)&selName = dlsym(objc, "sel_registerName");
    *(void**)&poolPush = dlsym(objc, "objc_autoreleasePoolPush");
    *(void**)&poolPop = dlsym(objc, "objc_autoreleasePoolPop");
    *(void**)&systemDevice = dlsym(metal, "MTLCreateSystemDefaultDevice");
    *(void**)&objcSend = dlsym(objc, "objc_msgSend");
    return objcClass && selName && poolPush && poolPop && systemDevice && objcSend;
}

static id nsString(const char* text) { return SEND(id, objcClass("NSString"), "stringWithUTF8String:", (id, SEL, const char*), text); }
static const char* errorText(id error) {
    return error ? GET(const char*, GET(id, error, "localizedDescription"), "UTF8String") : "no reason given";
}
static void release(id object) { if (object) DO(object, "release"); }

/* ---- what the shaders are ---- */

/* id: its place in the argument buffer; binding: Vulkan's number for it. kind: 0 a buffer, 1 a texture, 2 a
 * sampler; 3 and 4 the array of each. */
typedef struct MetalEntry { uint16_t id; uint8_t binding, kind; } MetalEntry;
typedef struct MetalShaderCode { const char* source; const MetalEntry* entries; uint32_t count; } MetalShaderCode;
#include "metal/shaders.inc"

typedef struct Shader {
    id library, function, encoder;
    id arguments;   /* the argument buffer */
    uint64_t stamp; /* what it was written for */
} Shader;

typedef struct Buffer { id buffer; size_t size; void* mapped; } Buffer;

typedef struct Pipeline { id state, depth; int cull; int vertex, fragment; } Pipeline;

typedef struct MeshRange { uint32_t firstIndex; int32_t vertexOffset; uint32_t indexCount, colorBase; uint64_t deformVersion; } MeshRange;

typedef struct Metal {
    ArtfRenderer base;
    id device, queue, layer;
    char description[300];
    bool noVsync, unified;
    int width, height, samples;
    bool targetsStale;
    id frameTexture, samplesTexture, depthTexture, blank, overlay, shadowAtlas;
    int shadowWidth, shadowHeight;
    id sampler, nearest;
    id vertexDescriptor;
    id depthOn, depthSame, depthOff, depthNear;
    Shader shaders[METAL_SHADERS];
    Pipeline scene, cutout, layerDepth, layer_, overlayDraw, traceShow, shadow, shadowHoles;
    id cull, trace, traceGuides, denoiseLight, denoiseTime, denoiseSpace[3], denoiseEnd;
    id last; /* the frame on its way */
    double gpuMs, cpuMs;
    uint64_t frameNumber;
    bool warnedHole, warnedGraphs;

    /* what each binding holds, and a stamp that says it changed */
    id bound[BINDINGS];
    id boundTexture[BINDINGS];
    id textureTable[MAX_TEXTURES];
    id samplerTable[MAX_TEXTURES];
    uint64_t stamp;
    ARTF_VEC(id) resources; /* scratch: all that shaders reach */

    Buffer empty, frame, instances, visible, indirect, template_, materials, meshTable, vertices, indices, colors;
    Buffer shadowData, shadowList, shadowCommands, traceData;
    Buffer traceNodes, traceTriangles, traceVertices, traceTop, traceInstances, traceSum, denoiseBuffers[7], denoiseShift, motion, meshletMeshes, meshletState;

    uint64_t sceneId, meshRemovals, textureVersion, meshTableVersion, lodVersion, structureVersion, instanceVersion, materialVersion;
    bool fresh;
    size_t meshesUp;
    ARTF_VEC(MeshRange) meshes;
    ARTF_VEC(id) textures;
    ARTF_VEC(uint64_t) textureMarks;
    ARTF_VEC(uint32_t) copyStarts;
    ARTF_VEC(ArtfLayer) layers;
    uint32_t instanceCapacity, materialCapacity, meshCapacity;
    uint64_t shadowInstances, shadowStructure, shadowMaterials, shadowScene;
    uint32_t shadowMeshes;

    uint64_t traceScene, traceMeshVersion, traceTopVersion, traceKey;
    uint32_t traceDone, traceFrames;
    bool denoiseFirst;
    float tracePrevious[16];

    const uint32_t* overlayPixels;
    int overlayWidth, overlayHeight, overlayStride;
    bool overlayFull, overlayPending, overlayUsable;
    ARTF_VEC(ArtfRect) overlayDamage;
} Metal;

/* ---- memory ---- */

static void waitLast(Metal* v) {
    if (!v->last) return;
    DO(v->last, "waitUntilCompleted");
    const double from = GET(double, v->last, "GPUStartTime"), to = GET(double, v->last, "GPUEndTime");
    v->gpuMs = (to - from) * 1000.0;
    release(v->last);
    v->last = NULL;
}

static void bufferFree(Buffer* b) {
    release(b->buffer);
    memset(b, 0, sizeof *b);
}

/* Room for at least size; what it held is gone. binding < 0: no shader reads it. */
static bool bufferNew(Metal* v, Buffer* b, size_t size, int binding) {
    bufferFree(b);
    size = size ? size : 16;
    b->buffer = SEND(id, v->device, "newBufferWithLength:options:", (id, SEL, NSUInteger, NSUInteger), (NSUInteger)size, (NSUInteger)0);
    if (!b->buffer) {
        artfLog("artful: no memory for a Metal buffer of %zu bytes", size);
        return false;
    }
    b->size = size;
    b->mapped = GET(void*, b->buffer, "contents");
    if (binding >= 0) {
        v->bound[binding] = b->buffer;
        ++v->stamp;
    }
    return true;
}

static bool bufferRoom(Metal* v, Buffer* b, size_t size, int binding) { return size <= b->size && b->buffer ? true : bufferNew(v, b, size * 2, binding); }

static id textureNew(Metal* v, int format, int width, int height, NSUInteger mips, int samples, NSUInteger usage, int storage) {
    id d = SEND(id, objcClass("MTLTextureDescriptor"), "texture2DDescriptorWithPixelFormat:width:height:mipmapped:", (id, SEL, NSUInteger, NSUInteger, NSUInteger, signed char), (NSUInteger)format, (NSUInteger)width, (NSUInteger)height, (signed char)0);
    SET_U(d, "setMipmapLevelCount:", mips);
    SET_U(d, "setUsage:", usage);
    SET_U(d, "setStorageMode:", storage);
    if (samples > 1) {
        SET_U(d, "setTextureType:", TEXTURE_2D_MS);
        SET_U(d, "setSampleCount:", samples);
    }
    return SEND(id, v->device, "newTextureWithDescriptor:", (id, SEL, id), d);
}

static void texturePut(id t, NSUInteger level, int x, int y, int w, int h, const void* pixels, int stride) {
    const MTLRegion region = {{(NSUInteger)x, (NSUInteger)y, 0}, {(NSUInteger)w, (NSUInteger)h, 1}};
    SEND(void, t, "replaceRegion:mipmapLevel:withBytes:bytesPerRow:", (id, SEL, MTLRegion, NSUInteger, const void*, NSUInteger), region, level, pixels, (NSUInteger)stride * 4);
}

static int hostStorage(const Metal* v) { return v->unified ? STORAGE_SHARED : STORAGE_MANAGED; }

/* ---- shaders ---- */

static bool shaderMake(Metal* v, int index) {
    Shader* s = &v->shaders[index];
    if (s->function) return true;
    id options = GET(id, objcClass("MTLCompileOptions"), "new");
    SET_U(options, "setLanguageVersion:", LANGUAGE_3_0);
    id error = NULL;
    s->library = SEND(id, v->device, "newLibraryWithSource:options:error:", (id, SEL, id, id, id*), nsString(metalShaderCode[index].source), options, &error);
    release(options);
    if (!s->library) {
        artfLog("artful: a Metal shader did not compile: %s", errorText(error));
        return false;
    }
    s->function = SEND(id, s->library, "newFunctionWithName:", (id, SEL, id), nsString("main0"));
    if (!s->function) return false;
    if (metalShaderCode[index].count) {
        s->encoder = SEND(id, s->function, "newArgumentEncoderWithBufferIndex:", (id, SEL, NSUInteger), (NSUInteger)0);
        const NSUInteger length = GET(NSUInteger, s->encoder, "encodedLength");
        s->arguments = SEND(id, v->device, "newBufferWithLength:options:", (id, SEL, NSUInteger, NSUInteger), length, (NSUInteger)0);
        SEND(void, s->encoder, "setArgumentBuffer:offset:", (id, SEL, id, NSUInteger), s->arguments, (NSUInteger)0);
    }
    return true;
}

/* This shader's argument buffer, up to date. */
static id arguments(Metal* v, int index) {
    Shader* s = &v->shaders[index];
    if (s->stamp == v->stamp) return s->arguments;
    s->stamp = v->stamp;
    const MetalShaderCode* code = &metalShaderCode[index];
    for (uint32_t i = 0; i < code->count; ++i) {
        const MetalEntry* e = &code->entries[i];
        const NSUInteger at = e->id;
        switch (e->kind) {
        case 0:
            SEND(void, s->encoder, "setBuffer:offset:atIndex:", (id, SEL, id, NSUInteger, NSUInteger), v->bound[e->binding] ? v->bound[e->binding] : v->empty.buffer, (NSUInteger)0, at);
            break;
        case 1:
            SEND(void, s->encoder, "setTexture:atIndex:", (id, SEL, id, NSUInteger), v->boundTexture[e->binding] ? v->boundTexture[e->binding] : v->blank, at);
            break;
        case 2:
            SEND(void, s->encoder, "setSamplerState:atIndex:", (id, SEL, id, NSUInteger), e->binding == 25 ? v->nearest : v->sampler, at);
            break;
        case 3: {
            const NSRange range = {at, MAX_TEXTURES};
            SEND(void, s->encoder, "setTextures:withRange:", (id, SEL, id*, NSRange), v->textureTable, range);
            break;
        }
        default: {
            const NSRange range = {at, MAX_TEXTURES};
            SEND(void, s->encoder, "setSamplerStates:withRange:", (id, SEL, id*, NSRange), v->samplerTable, range);
            break;
        }
        }
    }
    return s->arguments;
}

static id computeMake(Metal* v, int shader) {
    if (!shaderMake(v, shader)) return NULL;
    id error = NULL;
    id made = SEND(id, v->device, "newComputePipelineStateWithFunction:error:", (id, SEL, id, id*), v->shaders[shader].function, &error);
    if (!made) artfLog("artful: a Metal compute pipeline failed: %s", errorText(error));
    return made;
}

/* depth: its state; blend: premultiplied; color: writes color; target: 0 the frame, 1 shadows. */
static bool pipelineMake(Metal* v, Pipeline* p, int vertex, int fragment, id depth, int cull, bool blend, bool color, bool hasVertices, bool shadow) {
    if (!shaderMake(v, vertex) || (fragment >= 0 && !shaderMake(v, fragment))) return false;
    id d = GET(id, objcClass("MTLRenderPipelineDescriptor"), "new");
    SET_ID(d, "setVertexFunction:", v->shaders[vertex].function);
    if (fragment >= 0) SET_ID(d, "setFragmentFunction:", v->shaders[fragment].function);
    if (hasVertices) SET_ID(d, "setVertexDescriptor:", v->vertexDescriptor);
    if (shadow) {
        SET_U(d, "setDepthAttachmentPixelFormat:", PIXEL_DEPTH32);
    } else {
        SET_U(d, "setRasterSampleCount:", v->samples);
        SET_U(d, "setDepthAttachmentPixelFormat:", PIXEL_DEPTH32);
        id attachment = AT(GET(id, d, "colorAttachments"), 0);
        SET_U(attachment, "setPixelFormat:", PIXEL_BGRA8);
        if (!color) SET_U(attachment, "setWriteMask:", 0);
        if (blend) {
            SET_B(attachment, "setBlendingEnabled:", 1);
            SET_U(attachment, "setSourceRGBBlendFactor:", BLEND_ONE);
            SET_U(attachment, "setSourceAlphaBlendFactor:", BLEND_ONE);
            SET_U(attachment, "setDestinationRGBBlendFactor:", BLEND_ONE_MINUS_SOURCE_ALPHA);
            SET_U(attachment, "setDestinationAlphaBlendFactor:", BLEND_ONE_MINUS_SOURCE_ALPHA);
        }
    }
    id error = NULL;
    p->state = SEND(id, v->device, "newRenderPipelineStateWithDescriptor:error:", (id, SEL, id, id*), d, &error);
    release(d);
    if (!p->state) {
        artfLog("artful: a Metal pipeline failed: %s", errorText(error));
        return false;
    }
    p->depth = depth;
    p->cull = cull;
    p->vertex = vertex;
    p->fragment = fragment;
    return true;
}

static id depthState(Metal* v, int compare, bool write) {
    id d = GET(id, objcClass("MTLDepthStencilDescriptor"), "new");
    SET_U(d, "setDepthCompareFunction:", compare);
    SET_B(d, "setDepthWriteEnabled:", write);
    id made = SEND(id, v->device, "newDepthStencilStateWithDescriptor:", (id, SEL, id), d);
    release(d);
    return made;
}

static id samplerMake(Metal* v, bool linear) {
    id d = GET(id, objcClass("MTLSamplerDescriptor"), "new");
    const int filter = linear ? SAMPLER_LINEAR : SAMPLER_NEAREST, address = linear ? ADDRESS_REPEAT : ADDRESS_CLAMP;
    SET_U(d, "setMinFilter:", filter);
    SET_U(d, "setMagFilter:", filter);
    if (linear) SET_U(d, "setMipFilter:", MIP_LINEAR);
    SET_U(d, "setSAddressMode:", address);
    SET_U(d, "setTAddressMode:", address);
    SET_B(d, "setSupportArgumentBuffers:", 1);
    id made = SEND(id, v->device, "newSamplerStateWithDescriptor:", (id, SEL, id), d);
    release(d);
    return made;
}

static bool pipelinesMake(Metal* v) {
    v->sampler = samplerMake(v, true);
    v->nearest = samplerMake(v, false);
    for (int i = 0; i < MAX_TEXTURES; ++i) v->samplerTable[i] = v->sampler;

    v->vertexDescriptor = GET(id, objcClass("MTLVertexDescriptor"), "vertexDescriptor");
    DO(v->vertexDescriptor, "retain");
    static const int formats[3] = {VERTEX_USHORT4_NORMALIZED, VERTEX_SHORT2_NORMALIZED, VERTEX_HALF2}, offsets[3] = {0, 8, 12};
    id attributes = GET(id, v->vertexDescriptor, "attributes");
    for (int i = 0; i < 3; ++i) {
        id a = AT(attributes, i);
        SET_U(a, "setFormat:", formats[i]);
        SET_U(a, "setOffset:", offsets[i]);
        SET_U(a, "setBufferIndex:", VERTICES_AT);
    }
    SET_U(AT(GET(id, v->vertexDescriptor, "layouts"), VERTICES_AT), "setStride:", sizeof(ArtfPackedVertex));

    v->depthOn = depthState(v, COMPARE_GREATER, true); /* (depth is reversed) */
    v->depthSame = depthState(v, COMPARE_EQUAL, false);
    v->depthOff = depthState(v, COMPARE_ALWAYS, false);
    v->depthNear = depthState(v, COMPARE_LESS, true);  /* (these maps are not) */

    v->cull = computeMake(v, SHADER_CULLALL);
    return v->cull &&
           pipelineMake(v, &v->scene, SHADER_SCENEVERTEX, SHADER_SCENEFRAGMENT, v->depthOn, CULL_BACK, false, true, true, false) &&
           pipelineMake(v, &v->cutout, SHADER_SCENEVERTEX, SHADER_SCENECUTOUT, v->depthOn, CULL_NONE, false, true, true, false) &&
           pipelineMake(v, &v->layerDepth, SHADER_SCENEVERTEX, SHADER_SCENECUTOUT, v->depthOn, CULL_NONE, false, false, true, false) &&
           pipelineMake(v, &v->layer_, SHADER_SCENEVERTEX, SHADER_SCENECUTOUT, v->depthSame, CULL_NONE, true, true, true, false) &&
           pipelineMake(v, &v->overlayDraw, SHADER_OVERLAYVERTEX, SHADER_OVERLAYFRAGMENT, v->depthOff, CULL_NONE, true, true, false, false) &&
           pipelineMake(v, &v->traceShow, SHADER_OVERLAYVERTEX, SHADER_TRACESHOW, v->depthOff, CULL_NONE, true, true, false, false) &&
           pipelineMake(v, &v->shadow, SHADER_SHADOWVERTEX, -1, v->depthNear, CULL_NONE, false, false, true, true) &&
           pipelineMake(v, &v->shadowHoles, SHADER_SHADOWVERTEX, SHADER_SHADOWFRAGMENT, v->depthNear, CULL_NONE, false, false, true, true);
}

/* ---- what is drawn into ---- */

static void targetsFree(Metal* v) {
    release(v->frameTexture);
    release(v->samplesTexture);
    release(v->depthTexture);
    release(v->overlay);
    v->frameTexture = v->samplesTexture = v->depthTexture = v->overlay = NULL;
    v->boundTexture[7] = NULL;
    ++v->stamp;
    v->overlayUsable = false;
    bufferFree(&v->traceSum);
    for (int i = 0; i < 7; ++i) bufferFree(&v->denoiseBuffers[i]);
    bufferFree(&v->denoiseShift);
    bufferFree(&v->motion);
    bufferFree(&v->meshletMeshes);
    bufferFree(&v->meshletState);
    v->traceDone = 0;
}

static bool targetsMake(Metal* v) {
    targetsFree(v);
    v->targetsStale = false;
    if (v->width <= 0 || v->height <= 0) return true;
    v->frameTexture = textureNew(v, PIXEL_BGRA8, v->width, v->height, 1, 1, TEXTURE_TARGET | TEXTURE_READ, STORAGE_PRIVATE);
    v->depthTexture = textureNew(v, PIXEL_DEPTH32, v->width, v->height, 1, v->samples, TEXTURE_TARGET, STORAGE_PRIVATE);
    if (v->samples > 1) v->samplesTexture = textureNew(v, PIXEL_BGRA8, v->width, v->height, 1, v->samples, TEXTURE_TARGET, STORAGE_PRIVATE);
    v->overlay = textureNew(v, PIXEL_BGRA8, v->width, v->height, 1, 1, TEXTURE_READ, hostStorage(v));
    v->boundTexture[7] = v->overlay;
    ++v->stamp;
    if (v->layer) {
        const CGSize size = {(double)v->width, (double)v->height};
        SEND(void, v->layer, "setDrawableSize:", (id, SEL, CGSize), size);
    }
    return v->frameTexture && v->depthTexture && v->overlay;
}

/* ---- the scene ---- */

static bool uploadTexture(Metal* v, ArtfTextureData* t, uint32_t index) {
    if (t->format) artfTextureDecode(t);
    const int first = artfMini(t->residentMip, t->mipCount - 1);
    int width = t->width, height = t->height;
    for (int level = 0; level < first; ++level) {
        width = artfMaxi(width / 2, 1);
        height = artfMaxi(height / 2, 1);
    }
    id image = textureNew(v, PIXEL_BGRA8, width, height, (NSUInteger)(t->mipCount - first), 1, TEXTURE_READ, hostStorage(v));
    if (!image) return false;
    for (int level = first, w = width, h = height; level < t->mipCount; ++level) {
        texturePut(image, (NSUInteger)(level - first), 0, 0, w, h, t->pixels + t->offsets[level], w);
        w = artfMaxi(w / 2, 1);
        h = artfMaxi(h / 2, 1);
    }
    const uint64_t mark = (uint64_t)t->generation << 8 | (uint64_t)first;
    if (index < v->textures.len) {
        release(v->textures.items[index]);
        v->textures.items[index] = image;
        v->textureMarks.items[index] = mark;
    } else {
        artfVecPush(&v->textures, image);
        artfVecPush(&v->textureMarks, mark);
    }
    /* (the first, white, goes everywhere: every entry of the table is then a real one) */
    if (!index)
        for (int i = 0; i < MAX_TEXTURES; ++i) v->textureTable[i] = image;
    v->textureTable[index] = image;
    ++v->stamp;
    return true;
}

/* All meshes go up together when one comes or goes: memory is shared, so that is a copy and no more. */
static bool syncMeshes(Metal* v, ArtfScene* scene) {
    size_t vertices = 0, indices = 0, colors = 1;
    for (size_t i = 0; i < scene->meshes.len; ++i) {
        const ArtfMeshData* m = &scene->meshes.items[i];
        if (m->removed) continue;
        vertices += m->vertexCount;
        indices += m->indexCount;
        if (m->colors) colors += m->vertexCount;
    }
    if (!bufferRoom(v, &v->vertices, vertices * sizeof(ArtfPackedVertex), -1)) return false;
    if (!bufferRoom(v, &v->indices, indices * 4, -1)) return false;
    if (!bufferRoom(v, &v->colors, colors * 4, 16)) return false;
    v->meshes.len = 0;
    uint32_t atVertex = 0, atIndex = 0, atColor = 1;
    ((uint32_t*)v->colors.mapped)[0] = ~(uint32_t)0; /* (white: what a mesh without colors reads) */
    for (size_t i = 0; i < scene->meshes.len; ++i) {
        const ArtfMeshData* m = &scene->meshes.items[i];
        MeshRange range = {0, 0, 0, 0, m->skin ? m->pose.applied : m->deformVersion};
        if (m->vertexCount && !m->removed) {
            memcpy((char*)v->vertices.mapped + (size_t)atVertex * sizeof(ArtfPackedVertex), m->vertices, (size_t)m->vertexCount * sizeof(ArtfPackedVertex));
            memcpy((uint32_t*)v->indices.mapped + atIndex, m->indices, (size_t)m->indexCount * 4);
            range.firstIndex = atIndex;
            range.vertexOffset = (int32_t)atVertex;
            range.indexCount = m->indexCount;
            if (m->colors) {
                memcpy((uint32_t*)v->colors.mapped + atColor, m->colors, (size_t)m->vertexCount * 4);
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
        if (!bufferNew(v, &v->meshTable, (size_t)v->meshCapacity * 80, 3)) return false;
        if (!bufferNew(v, &v->indirect, (size_t)v->meshCapacity * 80, 5)) return false;
        if (!bufferNew(v, &v->template_, (size_t)v->meshCapacity * 80, -1)) return false;
        /* (the cull shader asks of meshlets, and is told none) */
        if (!bufferNew(v, &v->meshletMeshes, (size_t)v->meshCapacity * 16, 41)) return false;
        if (!bufferNew(v, &v->meshletState, 64, 43)) return false;
        memset(v->meshletMeshes.mapped, 0, (size_t)v->meshCapacity * 16);
        memset(v->meshletState.mapped, 0, 64);
    }
    return true;
}

static uint32_t layersAt(const Metal* v) { return v->instanceCapacity * (ARTF_MAX_LODS + 1) * 4; }

static bool syncScene(Metal* v, ArtfScene* scene) {
    if (v->sceneId != scene->id) {
        v->sceneId = scene->id;
        for (size_t i = 0; i < v->textures.len; ++i) release(v->textures.items[i]);
        v->textures.len = v->textureMarks.len = 0;
        v->meshes.len = 0;
        v->meshesUp = ~(size_t)0;
        v->textureVersion = ~scene->textureVersion;
        v->fresh = true;
    }
    if (v->meshesUp != scene->meshes.len || v->meshRemovals != scene->meshRemovals) {
        if (!syncMeshes(v, scene)) return false;
        v->fresh = true;
    }
    if (v->meshTableVersion != scene->meshes.len + scene->lodVersion) {
        v->meshTableVersion = scene->meshes.len + scene->lodVersion;
        float* out = v->meshTable.mapped;
        for (size_t i = 0; i < scene->meshes.len; ++i, out += 20) {
            const ArtfMeshData* m = &scene->meshes.items[i];
            float row[20] = {m->center[0], m->center[1], m->center[2], m->radius, m->scale[0], m->scale[1], m->scale[2], 0, m->offset[0], m->offset[1], m->offset[2], 0};
            memcpy(&row[7], &v->meshes.items[i].colorBase, 4);
            memcpy(&row[11], &v->meshes.items[i].vertexOffset, 4);
            for (uint32_t k = 0; k < ARTF_MAX_LODS; ++k) {
                const uint32_t mesh = k < m->lodCount ? m->lods[k] : 0;
                memcpy(&row[12 + k], &mesh, 4);
                row[16 + k] = k < m->lodCount ? m->lodErrors[k] : -1.f;
            }
            memcpy(out, row, sizeof row);
        }
        v->fresh = true;
    }
    const size_t textures = scene->textures.len < MAX_TEXTURES ? scene->textures.len : MAX_TEXTURES;
    if (v->textureVersion != scene->textureVersion || textures > v->textures.len) {
        v->textureVersion = scene->textureVersion;
        for (size_t i = 0; i < textures; ++i) {
            ArtfTextureData* t = &scene->textures.items[i];
            const uint64_t mark = (uint64_t)t->generation << 8 | (uint64_t)artfMini(t->residentMip, artfMaxi(t->mipCount - 1, 0));
            if (i < v->textures.len && v->textureMarks.items[i] == mark) continue;
            if (t->removed) {
                if (i >= v->textures.len) continue;
                v->textureTable[i] = v->textures.items[0];
                v->textureMarks.items[i] = mark;
                ++v->stamp;
                continue;
            }
            if (!uploadTexture(v, t, (uint32_t)i)) return false;
        }
    }
    if (scene->instances.len > v->instanceCapacity) {
        v->instanceCapacity = (uint32_t)scene->instances.len * 2;
        if (!bufferNew(v, &v->instances, (size_t)v->instanceCapacity * sizeof(ArtfInstanceData), 1)) return false;
        const size_t visible = ((size_t)layersAt(v) + v->instanceCapacity) * 8;
        if (!bufferNew(v, &v->visible, visible, 2)) return false;
        v->fresh = true;
    }
    if (scene->materials.len > v->materialCapacity) {
        v->materialCapacity = (uint32_t)scene->materials.len * 2;
        if (!bufferNew(v, &v->materials, (size_t)v->materialCapacity * sizeof(ArtfMaterialData), 4)) return false;
        v->fresh = true;
    }
    /* what changes from frame to frame */
    if (v->fresh || v->materialVersion != scene->materialVersion) {
        memcpy(v->materials.mapped, scene->materials.items, scene->materials.len * sizeof(ArtfMaterialData));
        v->materialVersion = scene->materialVersion;
    }
    if (v->fresh || v->instanceVersion != scene->instanceVersion) {
        if (scene->instances.len) memcpy(v->instances.mapped, scene->instances.items, scene->instances.len * sizeof(ArtfInstanceData));
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
        uint32_t* out = v->template_.mapped;
        uint32_t at = 0;
        for (size_t i = 0; i < v->meshes.len; ++i, out += 5) {
            const MeshRange* m = &v->meshes.items[i];
            const uint32_t command[5] = {m->indexCount, 0, m->firstIndex, (uint32_t)m->vertexOffset, at};
            memcpy(out, command, sizeof command);
            at += starts[i];
        }
        for (uint32_t part = 1; part < 4; ++part, out += v->meshes.len * 5) {
            memcpy(out, v->template_.mapped, v->meshes.len * 20);
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
        memcpy((char*)v->vertices.mapped + (size_t)range->vertexOffset * sizeof(ArtfPackedVertex), m->vertices, (size_t)m->vertexCount * sizeof(ArtfPackedVertex));
        range->deformVersion = m->deformVersion;
    }
    return true;
}

static bool syncShadowList(Metal* v, const ArtfScene* scene) {
    const uint32_t meshes = (uint32_t)v->meshes.len;
    if (v->shadowInstances == scene->instanceVersion && v->shadowStructure == scene->structureVersion && v->shadowMaterials == scene->materialVersion && v->shadowScene == scene->id && v->shadowMeshes == meshes) return true;
    if (!bufferRoom(v, &v->shadowList, (scene->instances.len + 1) * 8, 26)) return false;
    if (!bufferRoom(v, &v->shadowCommands, (size_t)meshes * 40 + 40, -1)) return false;
    artfVecReserve(&v->copyStarts, (size_t)meshes * 2 + 1);
    uint32_t* counts = v->copyStarts.items;
    memset(counts, 0, ((size_t)meshes * 2 + 1) * 4);
#define KIND(d) (scene->materials.items[(d)->material].flags & ARTF_MATERIAL_MASK ? meshes : 0)
    for (size_t i = 0; i < scene->instances.len; ++i) {
        const ArtfInstanceData* d = &scene->instances.items[i];
        if (!d->translucent) ++counts[KIND(d) + d->mesh];
    }
    uint32_t* commands = v->shadowCommands.mapped;
    uint32_t at = 0;
    for (uint32_t i = 0; i < meshes * 2; ++i) {
        const MeshRange* m = &v->meshes.items[i % meshes];
        const uint32_t command[5] = {m->indexCount, counts[i], m->firstIndex, (uint32_t)m->vertexOffset, at};
        memcpy(commands + i * 5, command, sizeof command);
        const uint32_t count = counts[i];
        counts[i] = at;
        at += count;
    }
    uint32_t* list = v->shadowList.mapped;
    for (size_t i = 0; i < scene->instances.len; ++i) {
        const ArtfInstanceData* d = &scene->instances.items[i];
        if (d->translucent) continue;
        const uint32_t slot = counts[KIND(d) + d->mesh]++;
        list[slot * 2] = (uint32_t)i;
        list[slot * 2 + 1] = d->mesh;
    }
#undef KIND
    v->shadowInstances = scene->instanceVersion;
    v->shadowStructure = scene->structureVersion;
    v->shadowMaterials = scene->materialVersion;
    v->shadowScene = scene->id;
    v->shadowMeshes = meshes;
    return true;
}

static bool syncTrace(Metal* v, ArtfScene* scene) {
    artfSceneTraceSync(scene);
    const ArtfTrace* t = &scene->trace;
    const bool guides = t->desc.denoise;
    if (!guides && !v->trace && !(v->trace = computeMake(v, SHADER_TRACE))) return false;
    if (guides && !v->traceGuides && !(v->traceGuides = computeMake(v, SHADER_TRACEGUIDES))) return false;
    const bool otherScene = v->traceScene != scene->id;
#define UP(buffer, binding, list, count) do { const size_t bytes = (count) * sizeof *(list); if (!bufferRoom(v, buffer, bytes, binding)) return false; memcpy((buffer)->mapped, list, bytes); } while (0)
    if (otherScene || v->traceMeshVersion != t->meshVersion) {
        UP(&v->traceNodes, 17, t->nodes.items, t->nodes.len);
        UP(&v->traceTriangles, 18, t->triangles.items, t->triangles.len);
        UP(&v->traceVertices, 19, t->vertices.items, t->vertices.len);
        v->traceMeshVersion = t->meshVersion;
    }
    if (otherScene || v->traceTopVersion != t->topVersion) {
        UP(&v->traceTop, 20, t->top.items, t->top.len);
        UP(&v->traceInstances, 21, t->instances.items, t->instances.len + 1);
        v->traceTopVersion = t->topVersion;
    }
#undef UP
    v->traceScene = scene->id;
    const size_t bytes = (size_t)v->width * v->height * 16;
    if (!v->traceSum.buffer) {
        if (!bufferNew(v, &v->traceSum, bytes, 22)) return false;
        v->traceDone = 0;
    }
    if (guides && !v->denoiseBuffers[0].buffer) {
        for (int i = 0; i < 7; ++i)
            if (!bufferNew(v, &v->denoiseBuffers[i], bytes, 27 + i)) return false;
        if (!bufferNew(v, &v->denoiseShift, bytes, 48)) return false;
        v->denoiseFirst = true;
    }
    if (guides && !v->denoiseTime) {
        v->denoiseLight = computeMake(v, SHADER_DENOISELIGHT);
        v->denoiseTime = computeMake(v, SHADER_DENOISETIME);
        v->denoiseSpace[0] = computeMake(v, SHADER_DENOISESPACE0);
        v->denoiseSpace[1] = computeMake(v, SHADER_DENOISESPACE1);
        v->denoiseSpace[2] = computeMake(v, SHADER_DENOISESPACE2);
        v->denoiseEnd = computeMake(v, SHADER_DENOISEEND);
        if (!v->denoiseLight || !v->denoiseTime || !v->denoiseSpace[0] || !v->denoiseSpace[1] || !v->denoiseSpace[2] || !v->denoiseEnd) return false;
    }
    return true;
}

/* ---- the overlay ---- */

static void metalSetOverlay(ArtfRenderer* r, const ArtfOverlay* o) {
    Metal* v = (Metal*)r;
    if (!o || !o->pixels) {
        v->overlayPixels = NULL;
        v->overlayPending = v->overlayUsable = false;
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

static void uploadOverlay(Metal* v) {
    if (!v->overlayPending) return;
    const int ow = artfMini(v->overlayWidth, v->width), oh = artfMini(v->overlayHeight, v->height);
    if (v->overlayFull || !v->overlayUsable) {
        if (ow < v->width || oh < v->height) {
            uint32_t* clear = artfCalloc((size_t)v->width, 4);
            for (int y = 0; y < v->height; ++y) texturePut(v->overlay, 0, 0, y, v->width, 1, clear, v->width);
            artfFree(clear);
        }
        texturePut(v->overlay, 0, 0, 0, ow, oh, v->overlayPixels, v->overlayStride);
    } else {
        for (size_t i = 0; i < v->overlayDamage.len; ++i) {
            const ArtfRect d = v->overlayDamage.items[i];
            const int x0 = artfMaxi(d.x, 0), y0 = artfMaxi(d.y, 0), x1 = artfMini(d.x + d.w, ow), y1 = artfMini(d.y + d.h, oh);
            if (x1 > x0 && y1 > y0) texturePut(v->overlay, 0, x0, y0, x1 - x0, y1 - y0, v->overlayPixels + (size_t)y0 * v->overlayStride + x0, v->overlayStride);
        }
    }
    v->overlayUsable = true;
    v->overlayPending = v->overlayFull = false;
    v->overlayDamage.len = 0;
}

/* ---- a frame ---- */

/* All that shaders reach through argument buffers: Metal cannot see it there, and is told. The atlas comes last,
 * for a pass that draws into it to leave out. */
static void resourcesGather(Metal* v) {
    v->resources.len = 0;
    for (int i = 0; i < BINDINGS; ++i)
        if (v->bound[i]) artfVecPush(&v->resources, v->bound[i]);
    artfVecPush(&v->resources, v->empty.buffer);
    artfVecPush(&v->resources, v->blank);
    for (size_t i = 0; i < v->textures.len; ++i) artfVecPush(&v->resources, v->textures.items[i]);
    if (v->overlay) artfVecPush(&v->resources, v->overlay);
    if (v->shadowAtlas) artfVecPush(&v->resources, v->shadowAtlas);
}

static void computeUse(Metal* v, id e) {
    SEND(void, e, "useResources:count:usage:", (id, SEL, id*, NSUInteger, NSUInteger), v->resources.items, (NSUInteger)v->resources.len, (NSUInteger)USE_READ_WRITE);
}

static void renderUse(Metal* v, id e, bool withAtlas) {
    const NSUInteger count = v->resources.len - (!withAtlas && v->shadowAtlas ? 1 : 0);
    SEND(void, e, "useResources:count:usage:stages:", (id, SEL, id*, NSUInteger, NSUInteger, NSUInteger), v->resources.items, count, (NSUInteger)USE_READ_WRITE, (NSUInteger)STAGE_VERTEX_FRAGMENT);
}

static void dispatch(Metal* v, id e, id pipeline, int shader, NSUInteger width, NSUInteger height, NSUInteger groupWidth, NSUInteger groupHeight) {
    SET_ID(e, "setComputePipelineState:", pipeline);
    SEND(void, e, "setBuffer:offset:atIndex:", (id, SEL, id, NSUInteger, NSUInteger), arguments(v, shader), (NSUInteger)0, (NSUInteger)0);
    const MTLSize groups = {(width + groupWidth - 1) / groupWidth, (height + groupHeight - 1) / groupHeight, 1}, size = {groupWidth, groupHeight, 1};
    SEND(void, e, "dispatchThreadgroups:threadsPerThreadgroup:", (id, SEL, MTLSize, MTLSize), groups, size);
}

static void pipelineUse(Metal* v, id e, const Pipeline* p) {
    SET_ID(e, "setRenderPipelineState:", p->state);
    SET_ID(e, "setDepthStencilState:", p->depth);
    SET_U(e, "setCullMode:", p->cull);
    if (metalShaderCode[p->vertex].count) SEND(void, e, "setVertexBuffer:offset:atIndex:", (id, SEL, id, NSUInteger, NSUInteger), arguments(v, p->vertex), (NSUInteger)0, (NSUInteger)0);
    if (p->fragment >= 0 && metalShaderCode[p->fragment].count) SEND(void, e, "setFragmentBuffer:offset:atIndex:", (id, SEL, id, NSUInteger, NSUInteger), arguments(v, p->fragment), (NSUInteger)0, (NSUInteger)0);
}

static void verticesUse(Metal* v, id e) {
    SEND(void, e, "setVertexBuffer:offset:atIndex:", (id, SEL, id, NSUInteger, NSUInteger), v->vertices.buffer, (NSUInteger)0, (NSUInteger)VERTICES_AT);
}

/* The commands from first on. (Metal has no draw of many at once, short of command buffers on the GPU.) */
static void drawCommands(Metal* v, id e, id commands, uint32_t first, uint32_t count) {
    SEL draw = selName("drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:");
    for (uint32_t i = 0; i < count; ++i)
        if (v->meshes.items[i % v->meshes.len].indexCount)
            ((void (*)(id, SEL, NSUInteger, NSUInteger, id, NSUInteger, id, NSUInteger))objcSend)(e, draw, PRIMITIVE_TRIANGLE, INDEX_UINT32, v->indices.buffer, 0, commands, (NSUInteger)(first + i) * 20);
}

static void drawTriangle(id e) {
    SEND(void, e, "drawPrimitives:vertexStart:vertexCount:", (id, SEL, NSUInteger, NSUInteger, NSUInteger), (NSUInteger)PRIMITIVE_TRIANGLE, (NSUInteger)0, (NSUInteger)3);
}

static void drawShadows(Metal* v, id commands, const ArtfShadowFrame* shadows, uint32_t views) {
    id pass = GET(id, objcClass("MTLRenderPassDescriptor"), "renderPassDescriptor");
    id depth = GET(id, pass, "depthAttachment");
    SET_ID(depth, "setTexture:", v->shadowAtlas);
    SET_U(depth, "setLoadAction:", LOAD_CLEAR);
    SET_U(depth, "setStoreAction:", STORE_STORE);
    SEND(void, depth, "setClearDepth:", (id, SEL, double), 1.0);
    id e = SEND(id, commands, "renderCommandEncoderWithDescriptor:", (id, SEL, id), pass);
    renderUse(v, e, false);
    SET_U(e, "setFrontFacingWinding:", WINDING_COUNTER_CLOCKWISE);
    verticesUse(v, e);
    const uint32_t meshes = (uint32_t)v->meshes.len;
    for (uint32_t i = 0; i < views; ++i) {
        const float* info = shadows->views[i].info;
        const double side = (double)shadows->views[i].size[0];
        const MTLViewport viewport = {info[0], info[1], side, side, 0, 1};
        const MTLScissorRect square = {(NSUInteger)info[0], (NSUInteger)info[1], (NSUInteger)side, (NSUInteger)side};
        SEND(void, e, "setViewport:", (id, SEL, MTLViewport), viewport);
        SEND(void, e, "setScissorRect:", (id, SEL, MTLScissorRect), square);
        /* (Vulkan's constant sits four bytes in: this has it in both places, wherever the shader looks) */
        const uint32_t push[2] = {i, i};
        SEND(void, e, "setVertexBytes:length:atIndex:", (id, SEL, const void*, NSUInteger, NSUInteger), push, (NSUInteger)sizeof push, (NSUInteger)1);
        pipelineUse(v, e, &v->shadow);
        drawCommands(v, e, v->shadowCommands.buffer, 0, meshes);
        pipelineUse(v, e, &v->shadowHoles);
        drawCommands(v, e, v->shadowCommands.buffer, meshes, meshes);
    }
    DO(e, "endEncoding");
}

static void denoise(Metal* v, id e) {
    const NSUInteger w = (NSUInteger)v->width, h = (NSUInteger)v->height;
    dispatch(v, e, v->denoiseLight, SHADER_DENOISELIGHT, w, h, 8, 8);
    dispatch(v, e, v->denoiseTime, SHADER_DENOISETIME, w, h, 8, 8);
    dispatch(v, e, v->denoiseSpace[0], SHADER_DENOISESPACE0, w, h, 8, 8);
    dispatch(v, e, v->denoiseSpace[1], SHADER_DENOISESPACE1, w, h, 8, 8);
    dispatch(v, e, v->denoiseSpace[2], SHADER_DENOISESPACE2, w, h, 8, 8);
    dispatch(v, e, v->denoiseEnd, SHADER_DENOISEEND, w, h, 8, 8);
}

static bool frameDraw(Metal* v, ArtfScene* scene) {
    waitLast(v);
    if (v->targetsStale && !targetsMake(v)) return false;
    if (!v->frameTexture) return true; /* a window with no size */
    if (scene->hole.on && !v->warnedHole) {
        v->warnedHole = true;
        artfLog("artful: Metal does not draw a black hole yet");
    }
    if (scene->graphs.len && !v->warnedGraphs) {
        v->warnedGraphs = true;
        artfLog("artful: Metal does not run materials written in C yet: they show their plain values");
    }
    if (!syncScene(v, scene)) return false;
    const bool tracing = scene->trace.desc.mode != ARTF_TRACE_NONE;
    if (tracing && !syncTrace(v, scene)) return false;
    ArtfFrameData frame;
    artfSceneFrame(scene, v->width, v->height, &frame);
    frame.sky[3] = frame.ground[3] = 0; /* (light from all around is left out here: the maps are never sent) */
    /* (as are lights past the frame's own, light that bounced and what comes after a frame in floats) */
    frame.eye[2][3] = 0;
    memset(frame.indirect, 0, sizeof frame.indirect);
    memset(frame.post, 0, sizeof frame.post);
    artfSceneResidencyStep(scene, &frame, v->height);
    memcpy(v->frame.mapped, &frame, sizeof frame);
    uploadOverlay(v);

    ArtfShadowFrame shadows;
    memset(&shadows, 0, sizeof shadows);
    const uint32_t shadowViews = tracing ? 0 : artfSceneShadowFrame(scene, &frame, &shadows);
    memcpy(v->shadowData.mapped, &shadows, sizeof shadows);
    if (shadowViews) {
        const int width = (int)shadows.sun[2] * ARTF_SHADOW_ATLAS_COLUMNS, height = (int)shadows.sun[3];
        if (width != v->shadowWidth || height > v->shadowHeight) {
            release(v->shadowAtlas);
            v->shadowAtlas = textureNew(v, PIXEL_DEPTH32, width, height, 1, 1, TEXTURE_TARGET | TEXTURE_READ, STORAGE_PRIVATE);
            if (!v->shadowAtlas) return false;
            v->shadowWidth = width;
            v->shadowHeight = height;
            v->boundTexture[25] = v->shadowAtlas;
            ++v->stamp;
        }
        if (!syncShadowList(v, scene)) return false;
    }

    uint32_t layerCount = 0, traceAdding = 0;
    bool traceClear = false;
    if (tracing) {
        const uint64_t key = artfSceneTraceKey(scene, &frame);
        const ArtfTraceDesc* d = &scene->trace.desc;
        if (key != v->traceKey || !v->traceDone) {
            traceClear = !d->denoise;
            v->traceDone = 0;
            v->traceKey = key;
        }
        traceAdding = (uint32_t)d->samples;
        if (d->maxSamples > 0) traceAdding = v->traceDone >= (uint32_t)d->maxSamples ? 0 : (uint32_t)artfMini(d->samples, d->maxSamples - (int)v->traceDone);
        if (d->denoise && traceAdding) traceClear = true;
        ArtfTraceFrame traced;
        artfSceneTraceFrame(scene, &frame, v->width, v->height, d->denoise ? v->traceFrames : v->traceDone, &traced);
        traced.pad[0] = d->denoise ? 1 : v->traceDone + traceAdding;
        traced.pad[1] = v->denoiseFirst;
        if (d->denoise) {
            /* (one frame at a time: nothing is reading this) */
            const ArtfTrace* t = &scene->trace;
            if (!bufferRoom(v, &v->motion, (t->motion.len + 12) * 4, 37)) return false;
            memcpy(v->motion.mapped, t->motion.items, t->motion.len * 4);
        }
        memcpy(traced.previous, v->tracePrevious, sizeof traced.previous);
        memcpy(v->traceData.mapped, &traced, sizeof traced);
        v->traceDone += traceAdding;
        if (d->denoise && traceAdding) {
            v->traceFrames += traceAdding;
            v->denoiseFirst = false;
            memcpy(v->tracePrevious, frame.viewProj, sizeof v->tracePrevious);
        }
        if (traceClear) memset(v->traceSum.mapped, 0, v->traceSum.size);
    } else if (scene->translucentCount) {
        artfVecReserve(&v->layers, scene->translucentCount);
        layerCount = artfSceneLayers(scene, &frame, v->layers.items);
        uint32_t* out = (uint32_t*)v->visible.mapped + (size_t)layersAt(v) * 2;
        for (uint32_t i = 0; i < layerCount; ++i) {
            out[i * 2] = v->layers.items[i].instance;
            out[i * 2 + 1] = v->layers.items[i].mesh;
        }
    }

    const uint32_t meshCount = (uint32_t)v->meshes.len, instanceCount = tracing ? 0 : (uint32_t)scene->instances.len;
    const bool cutouts = !tracing && (scene->cutoutCount != 0 || scene->features[ARTF_FEATURE_LOD_FADE]);
    if (instanceCount) memcpy(v->indirect.mapped, v->template_.mapped, (size_t)meshCount * 80);

    resourcesGather(v);
    id commands = GET(id, v->queue, "commandBuffer");
    DO(commands, "retain");
    v->last = commands;

    if (instanceCount || (tracing && traceAdding)) {
        id e = GET(id, commands, "computeCommandEncoder");
        computeUse(v, e);
        if (instanceCount) dispatch(v, e, v->cull, SHADER_CULLALL, instanceCount, 1, 64, 1);
        if (tracing) {
            const bool guides = scene->trace.desc.denoise;
            dispatch(v, e, guides ? v->traceGuides : v->trace, guides ? SHADER_TRACEGUIDES : SHADER_TRACE, (NSUInteger)v->width, (NSUInteger)v->height, 8, 8);
            if (guides) denoise(v, e);
        }
        DO(e, "endEncoding");
        if (tracing && scene->trace.desc.denoise) {
            /* what this frame found becomes the frame before */
            id blit = GET(id, commands, "blitCommandEncoder");
            const NSUInteger all = (NSUInteger)v->width * (NSUInteger)v->height * 16;
            SEL copy = selName("copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:");
            ((void (*)(id, SEL, id, NSUInteger, id, NSUInteger, NSUInteger))objcSend)(blit, copy, v->denoiseBuffers[0].buffer, 0, v->denoiseBuffers[2].buffer, 0, all);
            ((void (*)(id, SEL, id, NSUInteger, id, NSUInteger, NSUInteger))objcSend)(blit, copy, v->denoiseBuffers[4].buffer, 0, v->denoiseBuffers[3].buffer, 0, all);
            DO(blit, "endEncoding");
        }
    }
    if (shadowViews) drawShadows(v, commands, &shadows, shadowViews);

    id pass = GET(id, objcClass("MTLRenderPassDescriptor"), "renderPassDescriptor");
    id color = AT(GET(id, pass, "colorAttachments"), 0);
    const MTLClearColor clear = {frame.background[0], frame.background[1], frame.background[2], frame.background[3]};
    SET_U(color, "setLoadAction:", LOAD_CLEAR);
    SEND(void, color, "setClearColor:", (id, SEL, MTLClearColor), clear);
    if (v->samples > 1) {
        SET_ID(color, "setTexture:", v->samplesTexture);
        SET_ID(color, "setResolveTexture:", v->frameTexture);
        SET_U(color, "setStoreAction:", STORE_RESOLVE);
    } else {
        SET_ID(color, "setTexture:", v->frameTexture);
        SET_U(color, "setStoreAction:", STORE_STORE);
    }
    id depth = GET(id, pass, "depthAttachment");
    SET_ID(depth, "setTexture:", v->depthTexture);
    SET_U(depth, "setLoadAction:", LOAD_CLEAR);
    SET_U(depth, "setStoreAction:", STORE_DONT_CARE);
    SEND(void, depth, "setClearDepth:", (id, SEL, double), 0.0);
    id e = SEND(id, commands, "renderCommandEncoderWithDescriptor:", (id, SEL, id), pass);
    renderUse(v, e, true);
    /* (y goes up in clip space and down in a frame here, as Vulkan's flipped viewport has it) */
    SET_U(e, "setFrontFacingWinding:", WINDING_COUNTER_CLOCKWISE);
    if (instanceCount) {
        verticesUse(v, e);
        pipelineUse(v, e, &v->scene);
        drawCommands(v, e, v->indirect.buffer, 0, meshCount);
        if (cutouts) {
            pipelineUse(v, e, &v->cutout);
            drawCommands(v, e, v->indirect.buffer, meshCount * 2, meshCount);
        }
    }
    if (layerCount) {
        SEL draw = selName("drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:");
        for (uint32_t i = 0; i < layerCount; ++i) {
            const MeshRange* m = &v->meshes.items[v->layers.items[i].mesh];
            for (int step = 0; step < 2; ++step) {
                pipelineUse(v, e, step ? &v->layer_ : &v->layerDepth);
                ((void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, id, NSUInteger, NSUInteger, long, NSUInteger))objcSend)(e, draw, PRIMITIVE_TRIANGLE, m->indexCount, INDEX_UINT32, v->indices.buffer, (NSUInteger)m->firstIndex * 4, 1, (long)m->vertexOffset, (NSUInteger)layersAt(v) + i);
            }
        }
    }
    if (tracing && (v->traceDone || scene->trace.desc.denoise)) {
        pipelineUse(v, e, &v->traceShow);
        drawTriangle(e);
    }
    if (v->overlayPixels && v->overlayUsable) {
        pipelineUse(v, e, &v->overlayDraw);
        drawTriangle(e);
    }
    DO(e, "endEncoding");

    if (v->layer) {
        id drawable = GET(id, v->layer, "nextDrawable");
        if (drawable) {
            id blit = GET(id, commands, "blitCommandEncoder");
            SEND(void, blit, "copyFromTexture:toTexture:", (id, SEL, id, id), v->frameTexture, GET(id, drawable, "texture"));
            DO(blit, "endEncoding");
            SET_ID(commands, "presentDrawable:", drawable);
        }
    }
    DO(commands, "commit");
    ++v->frameNumber;
    return GET(NSUInteger, commands, "status") != STATUS_ERROR;
}

static bool metalDraw(ArtfRenderer* r, ArtfScene* scene) {
    Metal* v = (Metal*)r;
    const double start = artfNowMs();
    void* pool = poolPush();
    const bool ok = frameDraw(v, scene);
    poolPop(pool);
    v->cpuMs = artfNowMs() - start;
    return ok;
}

static bool metalReadPixels(ArtfRenderer* r, uint32_t* dst) {
    Metal* v = (Metal*)r;
    if (!v->frameNumber || !v->frameTexture) return false;
    waitLast(v);
    void* pool = poolPush();
    const NSUInteger row = (NSUInteger)v->width * 4, bytes = row * (NSUInteger)v->height;
    Buffer readback;
    memset(&readback, 0, sizeof readback);
    bool ok = bufferNew(v, &readback, bytes, -1);
    if (ok) {
        id commands = GET(id, v->queue, "commandBuffer");
        id blit = GET(id, commands, "blitCommandEncoder");
        const MTLOrigin origin = {0, 0, 0};
        const MTLSize size = {(NSUInteger)v->width, (NSUInteger)v->height, 1};
        SEND(void, blit, "copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toBuffer:destinationOffset:destinationBytesPerRow:destinationBytesPerImage:",
             (id, SEL, id, NSUInteger, NSUInteger, MTLOrigin, MTLSize, id, NSUInteger, NSUInteger, NSUInteger), v->frameTexture, (NSUInteger)0, (NSUInteger)0, origin, size, readback.buffer, (NSUInteger)0, row, bytes);
        DO(blit, "endEncoding");
        DO(commands, "commit");
        DO(commands, "waitUntilCompleted");
        ok = GET(NSUInteger, commands, "status") != STATUS_ERROR;
        if (ok) memcpy(dst, readback.mapped, bytes);
    }
    bufferFree(&readback);
    poolPop(pool);
    return ok;
}

static void metalResize(ArtfRenderer* r, int width, int height) {
    Metal* v = (Metal*)r;
    waitLast(v);
    v->width = width;
    v->height = height;
    v->targetsStale = true;
}

static uint32_t* metalPixels(ArtfRenderer* r) { (void)r; return NULL; }
static const char* metalDescription(const ArtfRenderer* r) { return ((const Metal*)r)->description; }

static void metalStats(const ArtfRenderer* r, ArtfFrameStats* out) {
    const Metal* v = (const Metal*)r;
    memset(out, 0, sizeof *out);
    out->cpuMs = v->cpuMs;
    out->gpuMs = v->gpuMs;
    out->traceSamples = v->traceDone;
}

static void pipelineFree(Pipeline* p) { release(p->state); }

static void metalFree(ArtfRenderer* r) {
    Metal* v = (Metal*)r;
    waitLast(v);
    targetsFree(v);
    Buffer* buffers[] = {&v->empty, &v->frame, &v->instances, &v->visible, &v->indirect, &v->template_, &v->materials, &v->meshTable, &v->vertices, &v->indices, &v->colors, &v->shadowData,
                         &v->shadowList, &v->shadowCommands, &v->traceData, &v->traceNodes, &v->traceTriangles, &v->traceVertices, &v->traceTop, &v->traceInstances};
    for (size_t i = 0; i < ARTF_COUNT(buffers); ++i) bufferFree(buffers[i]);
    for (size_t i = 0; i < v->textures.len; ++i) release(v->textures.items[i]);
    Pipeline* pipelines[] = {&v->scene, &v->cutout, &v->layerDepth, &v->layer_, &v->overlayDraw, &v->traceShow, &v->shadow, &v->shadowHoles};
    for (size_t i = 0; i < ARTF_COUNT(pipelines); ++i) pipelineFree(pipelines[i]);
    const id others[] = {v->cull, v->trace, v->traceGuides, v->denoiseLight, v->denoiseTime, v->denoiseSpace[0], v->denoiseSpace[1], v->denoiseSpace[2], v->denoiseEnd, v->depthOn, v->depthSame, v->depthOff, v->depthNear,
                         v->sampler, v->nearest, v->vertexDescriptor, v->blank, v->shadowAtlas, v->queue, v->device};
    for (int i = 0; i < METAL_SHADERS; ++i) {
        release(v->shaders[i].arguments);
        release(v->shaders[i].encoder);
        release(v->shaders[i].function);
        release(v->shaders[i].library);
    }
    for (size_t i = 0; i < ARTF_COUNT(others); ++i) release(others[i]);
    artfVecFree(&v->resources);
    artfVecFree(&v->meshes);
    artfVecFree(&v->textures);
    artfVecFree(&v->textureMarks);
    artfVecFree(&v->copyStarts);
    artfVecFree(&v->layers);
    artfVecFree(&v->overlayDamage);
    artfFree(v);
}

static const ArtfRendererVtbl metalVtbl = {metalDescription, metalResize, metalSetOverlay, metalDraw, metalReadPixels, metalPixels, metalStats, metalFree};

ArtfRenderer* artfMetalNew(const ArtfRendererOptions* o) {
    if (!runtimeOpen()) return NULL;
    id device = systemDevice();
    if (!device) return NULL;
    if (!SEND(signed char, device, "supportsFamily:", (id, SEL, NSUInteger), (NSUInteger)FAMILY_METAL3) || GET(NSUInteger, device, "argumentBuffersSupport") != ARGUMENT_TIER2) {
        artfLog("artful: this device has no Metal 3");
        release(device);
        return NULL;
    }
    Metal* v = artfCalloc(1, sizeof *v);
    v->base.vt = &metalVtbl;
    v->base.backend = ARTF_BACKEND_METAL;
    v->device = device;
    v->queue = GET(id, device, "newCommandQueue");
    v->unified = GET(signed char, device, "hasUnifiedMemory");
    v->width = o->width;
    v->height = o->height;
    v->samples = o->antialias ? 4 : 1;
    v->noVsync = o->noVsync;
    v->denoiseFirst = true;
    v->meshesUp = ~(size_t)0;
    snprintf(v->description, sizeof v->description, "%s (Metal)", GET(const char*, GET(id, device, "name"), "UTF8String"));
    if (o->surface.kind == ARTF_SURFACE_METAL) {
        v->layer = o->surface.a;
        SET_ID(v->layer, "setDevice:", device);
        SET_U(v->layer, "setPixelFormat:", PIXEL_BGRA8);
        SET_B(v->layer, "setFramebufferOnly:", 0); /* (the frame is copied in) */
        if (o->noVsync && SEND(signed char, v->layer, "respondsToSelector:", (id, SEL, SEL), selName("setDisplaySyncEnabled:"))) SET_B(v->layer, "setDisplaySyncEnabled:", 0);
    }
    void* pool = poolPush();
    v->blank = textureNew(v, PIXEL_BGRA8, 1, 1, 1, 1, TEXTURE_READ, hostStorage(v));
    const uint32_t white = ~(uint32_t)0;
    texturePut(v->blank, 0, 0, 0, 1, 1, &white, 1);
    for (int i = 0; i < MAX_TEXTURES; ++i) v->textureTable[i] = v->blank;
    bool ok = bufferNew(v, &v->empty, BLANK_BYTES, -1) && bufferNew(v, &v->frame, sizeof(ArtfFrameData), 0) && bufferNew(v, &v->shadowData, sizeof(ArtfShadowFrame), 24) &&
              bufferNew(v, &v->traceData, sizeof(ArtfTraceFrame), 23) && pipelinesMake(v) && targetsMake(v);
    poolPop(pool);
    if (!ok) {
        metalFree(&v->base);
        return NULL;
    }
    return &v->base;
}
