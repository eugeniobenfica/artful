/* artful — the Metal backend, for macOS and iOS. Built only with ARTFUL_METAL, chosen only by name, and run so far
 * only where CI's macOS runner lets it (docs/limits.md).
 *
 * Plain C: Metal is called through the Objective-C runtime, both opened at run time, so there is nothing to link.
 * Its shaders are Vulkan's: shaders.c builds them as SPIR-V, the IR reads them back (irFromSpirv) and writes them as
 * Metal (irMsl), which Metal compiles as the backend starts; irMsl also says what each one's argument buffer holds,
 * and that is what is written into it. So they read the bindings of now, as Vulkan's do, and this side gives them
 * what Vulkan gives: its frame is the WebGPU backend's (webgpu.c), made simpler by what Metal has:
 *
 *   memory is shared with the CPU, so nothing is staged, and a frame waits for the one before: one copy of all;
 *   a shader's resources are an argument buffer, written when a resource changes;
 *   what is drawn is drawn into a texture of its own, and copied to the window, so reading a frame is the same
 *   with or without one.
 *
 * With argument buffers of tier 2, textures are read by any index, a thousand of them; with tier 1, by constant
 * indices (ARTF_TEXTURES_UNIFORM), 64. It draws what webgpu.c draws, as it does: materials in C, layers of
 * transparency, the frame in floats and what follows it (set 1, the light's roles, an argument buffer for each way they
 * fall), light from all around, tracing and the probes through the scene's trees, views and a background pass (its
 * shader read back into the IR as the library's are). What it leaves out, it says in the log; compressed textures go up
 * decoded. */
#include "render/rendererInternal.h"
#include "scene/sceneInternal.h"
#include "shaderir/ir.h"
#include "shaderir/msl.h"
#include "vulkan/shaders.h"

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
    PIXEL_BGRA8 = 80, PIXEL_DEPTH32 = 252, PIXEL_RGBA16F = 115, PIXEL_R32U = 53, PIXEL_RGBA32U = 123,
    STORAGE_SHARED = 0, STORAGE_MANAGED = 1, STORAGE_PRIVATE = 2,
    TEXTURE_READ = 1, TEXTURE_TARGET = 4, TEXTURE_2D = 2, TEXTURE_2D_MS = 4,
    LOAD_LOAD = 1, LOAD_CLEAR = 2, STORE_DONT_CARE = 0, STORE_STORE = 1, STORE_RESOLVE = 2, STORE_STORE_RESOLVE = 3,
    COMPARE_LESS = 1, COMPARE_EQUAL = 2, COMPARE_GREATER = 4, COMPARE_ALWAYS = 7,
    BLEND_ZERO = 0, BLEND_ONE = 1, BLEND_SOURCE_ALPHA = 4, BLEND_ONE_MINUS_SOURCE_ALPHA = 5, BLEND_DESTINATION_ALPHA = 8,
    VERTEX_USHORT4_NORMALIZED = 21, VERTEX_SHORT2_NORMALIZED = 22, VERTEX_HALF2 = 25,
    PRIMITIVE_TRIANGLE = 3, INDEX_UINT32 = 1, WINDING_COUNTER_CLOCKWISE = 1, CULL_NONE = 0, CULL_BACK = 2,
    SAMPLER_NEAREST = 0, SAMPLER_LINEAR = 1, MIP_LINEAR = 2, ADDRESS_CLAMP = 0, ADDRESS_REPEAT = 2,
    USE_READ_WRITE = 3, STAGE_VERTEX_FRAGMENT = 3,
    STATUS_ERROR = 5,
    ARGUMENT_TIER2 = 1, LANGUAGE_3_0 = 3 << 16,
    VERTICES_AT = 30 /* where vertices are bound: past what the shaders take */
};

#define MAX_TEXTURES 1024
#define UNIFORM_TEXTURES 64
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

typedef struct Shader {
    id library, function, encoder;
    id arguments;   /* the argument buffer */
    uint64_t stamp; /* what it was written for */
    IrMslArguments table;
    /* set 1, Vulkan's roles (the light, the second, the history): an argument buffer for each way they fall, one after
     * the other in roleArguments, each roleStride bytes */
    id encoder1, roleArguments;
    NSUInteger roleStride;
    uint64_t roleStamp[27];
    MTLSize threads; /* a compute shader's threadgroup: its LocalSize */
} Shader;

typedef struct Buffer { id buffer; size_t size; void* mapped; } Buffer;

typedef struct Pipeline { id state, depth; int cull; Shader vertex, fragment; bool hasFragment; } Pipeline;
typedef struct Compute { Shader shader; id state; } Compute;

typedef struct MeshRange { uint32_t firstIndex; int32_t vertexOffset; uint32_t indexCount, colorBase; uint64_t deformVersion; } MeshRange;

typedef struct Metal {
    ArtfRenderer base;
    id device, queue, layer;
    char description[300];
    bool noVsync, unified, uniformTextures;
    int width, height, samples;
    uint32_t maxTextures;
    bool targetsStale;
    id frameTexture, samplesTexture, depthTexture, blank, overlay, shadowAtlas;
    int shadowWidth, shadowHeight;
    id sampler, nearest;
    id vertexDescriptor;
    id depthOn, depthOff, depthNear;
    Shader cullShader;
    id cull;
    Pipeline scene, cutout, overlayDraw, shadow, shadowHoles;
    Pipeline layerDepth, layerDraw, particle, environmentBehind, fogBehind, traceShow, final, overlayFloats;
    id depthEqual;
    id last; /* the frame on its way */
    double gpuMs, cpuMs;
    uint64_t frameNumber;
    bool warnedTrace, warnedWide;
    /* the overdraw view's (artfShaderOverdraw): what is solid, then what has holes and the layers, both sides */
    Pipeline overdraw[2];
    /* a background pass of the program's (as webgpu.c's): its shader writes the light (12) from the block (11), its data
     * (14) and stream (15); the bloom's passes (13), and what draws it behind */
    Buffer backgroundBlock, backgroundLight, backgroundBloom, backgroundData, backgroundStream;
    Compute backgroundUser, bloomFirst, bloomNext, bloomUp;
    Pipeline backdrop;
    uint64_t backgroundModuleStamp, backgroundModuleScene, backgroundDataStamp, backgroundDataScene, backgroundStreamStamp, backgroundStreamScene;
    /* what the scene's pipelines were made for: its materials of their own, the variant, wide */
    bool programsMade, madeWide, froxels, wide, floats;
    uint32_t graphCount, madeVariant, variantBits;
    uint64_t graphScene;
    ARTF_VEC(ArtfLayer) layers;
    /* a frame in floats (as webgpu.c's) */
    int sceneWidth, sceneHeight;
    float scale;
    id floatColor, floatSamples, floatSurface;
    Buffer post[8], postLocks, postGathered, cloudField, cloudGrid, environment;
    uint32_t postNeeds, bloomSizes[6][3], postFrames, grainFrames, gatheredHalf, lutRoom, lockHalf;
    uint8_t role[3];
    bool historyValid, exposureValid, hasPrevious, gatheredValid;
    uint64_t lutScene, lutVersion, cloudGeneration, environmentVersion, environmentScene;
    float previous[16];
    ArtfMotion motion;
    Compute posts[22], trace, denoisePasses[9], probeStep, reflectionTraced;
    /* tracing and the probes, through the scene's trees */
    Buffer traceNodes, traceTriangles, traceVertices, traceTop, traceInstances, traceSum, traceFrameUbo, denoiseBuffers[7], denoiseShift, traceMotion, probesNow, probesNext;
    uint64_t traceScene, traceMeshVersion, traceTopVersion, traceKey, traceProgramScene, probeKey, probeScene, probeSerial, probeGraphScene, reflectionScene;
    uint32_t traceDone, traceFrames, traceProgramKey, probeFrames, probeGraphs, reflectionGraphs;
    bool denoiseFirst;
    float tracePrevious[16];

    id textureTable[MAX_TEXTURES];
    id samplerTable[MAX_TEXTURES];
    uint64_t stamp;         /* moves when what a binding holds does */
    ARTF_VEC(id) resources; /* scratch: all that shaders reach */

    Buffer empty, frame, instances, visible, indirect, template_, materials, meshTable, vertices, indices, colors;
    Buffer shadowData, shadowList, shadowCommands, meshletMeshes, meshletState, extraLights, tileLights, decals;

    uint64_t sceneId, meshRemovals, textureVersion, meshTableVersion, lodVersion, structureVersion, instanceVersion, materialVersion;
    bool fresh;
    size_t meshesUp;
    ARTF_VEC(MeshRange) meshes;
    ARTF_VEC(id) textures;
    ARTF_VEC(uint64_t) textureMarks;
    ARTF_VEC(uint32_t) copyStarts;
    uint32_t instanceCapacity, materialCapacity, meshCapacity;
    uint64_t shadowInstances, shadowStructure, shadowMaterials, shadowScene;
    uint32_t shadowMeshes;
    ArtfLightData* lightScratch;
    uint32_t* listScratch;
    size_t listScratchSize;

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

/* Room for at least size; what it held is gone. */
static bool bufferNew(Metal* v, Buffer* b, size_t size) {
    bufferFree(b);
    size = size ? size : 16;
    b->buffer = SEND(id, v->device, "newBufferWithLength:options:", (id, SEL, NSUInteger, NSUInteger), (NSUInteger)size, (NSUInteger)0);
    if (!b->buffer) {
        artfLog("artful: no memory for a Metal buffer of %zu bytes", size);
        return false;
    }
    b->size = size;
    b->mapped = GET(void*, b->buffer, "contents");
    memset(b->mapped, 0, size);
    ++v->stamp;
    return true;
}

static bool bufferRoom(Metal* v, Buffer* b, size_t size) { return size <= b->size && b->buffer ? true : bufferNew(v, b, size * 2); }

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

/* A shader of shaders.c's, as Metal: compiled, its argument buffer made, and what it holds. */
static bool shaderMake(Metal* v, Shader* s, uint32_t* code, size_t bytes, bool flipY) {
    if (!code) return false;
    IrModule m;
    irInit(&m);
    const bool read = irFromSpirv(&m, code, bytes / 4);
    artfFree(code);
    char why[256] = "not SPIR-V";
    const IrMslOptions options = {flipY};
    char* text = read ? irMsl(&m, &options, &s->table, why, sizeof why) : NULL;
    irFree(&m);
    if (!text) {
        artfLog("artful: a shader could not be written for Metal: %s", why);
        return false;
    }
    bool set1 = false;
    for (int k = 0; k < s->table.count; ++k) {
        if (s->table.items[k].set > 1) {
            artfLog("artful: a Metal shader binds set %u: only sets 0 and 1 are given", s->table.items[k].set);
            artfFree(text);
            return false;
        }
        set1 = set1 || s->table.items[k].set == 1;
    }
    id options_ = GET(id, objcClass("MTLCompileOptions"), "new");
    SET_U(options_, "setLanguageVersion:", LANGUAGE_3_0);
    id error = NULL;
    s->library = SEND(id, v->device, "newLibraryWithSource:options:error:", (id, SEL, id, id, id*), nsString(text), options_, &error);
    release(options_);
    artfFree(text);
    if (!s->library) {
        artfLog("artful: a Metal shader did not compile: %s", errorText(error));
        return false;
    }
    s->function = SEND(id, s->library, "newFunctionWithName:", (id, SEL, id), nsString("main0"));
    if (!s->function) return false;
    if (s->table.count) {
        s->encoder = SEND(id, s->function, "newArgumentEncoderWithBufferIndex:", (id, SEL, NSUInteger), (NSUInteger)0);
        const NSUInteger length = GET(NSUInteger, s->encoder, "encodedLength");
        s->arguments = SEND(id, v->device, "newBufferWithLength:options:", (id, SEL, NSUInteger, NSUInteger), length ? length : 16, (NSUInteger)0);
        SEND(void, s->encoder, "setArgumentBuffer:offset:", (id, SEL, id, NSUInteger), s->arguments, (NSUInteger)0);
    }
    if (set1) {
        s->encoder1 = SEND(id, s->function, "newArgumentEncoderWithBufferIndex:", (id, SEL, NSUInteger), (NSUInteger)1);
        const NSUInteger length = GET(NSUInteger, s->encoder1, "encodedLength");
        s->roleStride = (length + 255) & ~(NSUInteger)255;
        s->roleArguments = SEND(id, v->device, "newBufferWithLength:options:", (id, SEL, NSUInteger, NSUInteger), s->roleStride * 27, (NSUInteger)0);
    }
    return true;
}

/* A compute shader's threadgroup, as its SPIR-V says (OpExecutionMode LocalSize): Metal's dispatch gives it. */
static MTLSize localSizeOf(const uint32_t* code, size_t bytes) {
    MTLSize size = {64, 1, 1};
    for (size_t at = 5; code && at < bytes / 4;) {
        const uint32_t count = code[at] >> 16, op = code[at] & 0xFFFF;
        if (!count) break;
        if (op == 16 && count >= 6 && code[at + 2] == 17) {
            size.width = code[at + 3], size.height = code[at + 4], size.depth = code[at + 5];
            break;
        }
        at += count;
    }
    return size;
}

/* What Vulkan's binding is here (as webgpu.c's bufferAt). */
static Buffer* bufferAt(Metal* v, uint32_t binding) {
    Buffer* b;
    switch (binding) {
    case 0: b = &v->frame; break;
    case 1: b = &v->instances; break;
    case 2: b = &v->visible; break;
    case 3: b = &v->meshTable; break;
    case 4: b = &v->materials; break;
    case 5: b = &v->indirect; break;
    case 16: b = &v->colors; break;
    case 24: b = &v->shadowData; break;
    case 26: b = &v->shadowList; break;
    case 41: b = &v->meshletMeshes; break;
    case 43: b = &v->meshletState; break;
    case 60: b = &v->extraLights; break;
    case 61: b = &v->tileLights; break;
    case 63: b = &v->decals; break;
    case 17: b = &v->traceNodes; break;
    case 18: b = &v->traceTriangles; break;
    case 19: b = &v->traceVertices; break;
    case 20: b = &v->traceTop; break;
    case 21: b = &v->traceInstances; break;
    case 22: b = &v->traceSum; break;
    case 23: b = &v->traceFrameUbo; break;
    case 27: case 28: case 29: case 30: case 31: case 32: case 33: b = &v->denoiseBuffers[binding - 27]; break;
    case 37: b = &v->traceMotion; break;
    case 11: b = &v->backgroundBlock; break;
    case 12: b = &v->backgroundLight; break;
    case 13: b = &v->backgroundBloom; break;
    case 14: b = &v->backgroundData; break;
    case 15: b = &v->backgroundStream; break;
    case 48: b = &v->denoiseShift; break;
    case 49: b = &v->environment; break;
    case 50: case 51: case 52: case 53: case 54: case 55: case 56: case 57: b = &v->post[binding - 50]; break;
    case 62: b = &v->probesNow; break;
    case 69: b = v->postNeeds & 2 /* NEED_BLUR */ ? &v->post[5] : &v->empty; break;
    case 70: b = &v->probesNext; break;
    case 71: b = &v->postLocks; break;
    case 72: b = &v->postGathered; break;
    case 73: b = &v->cloudField; break;
    case 74: b = &v->cloudGrid; break;
    default: b = &v->empty; break;
    }
    return b->buffer ? b : &v->empty;
}

/* (what is read as it is, never filtered: depth, the shadow atlas, the light and the surface words) */
static bool unfiltered(uint32_t binding) { return binding == 8 || binding == 25 || binding == 58 || binding == 59; }

/* This shader's argument buffer, up to date. */
static id arguments(Metal* v, Shader* s) {
    if (s->stamp == v->stamp) return s->arguments;
    s->stamp = v->stamp;
    for (int k = 0; k < s->table.count; ++k) {
        const IrMslArgument* e = &s->table.items[k];
        const NSUInteger at = e->id;
        if (e->set) continue; /* (set 1: rolesArguments) */
        switch (e->kind) {
        case IR_MSL_BUFFER:
            SEND(void, s->encoder, "setBuffer:offset:atIndex:", (id, SEL, id, NSUInteger, NSUInteger), bufferAt(v, e->binding)->buffer, (NSUInteger)0, at);
            break;
        case IR_MSL_TEXTURE:
            if (e->count > 1) {
                const NSRange range = {at, e->count};
                SEND(void, s->encoder, "setTextures:withRange:", (id, SEL, id*, NSRange), v->textureTable, range);
            } else {
                id t = e->binding == 7 ? v->overlay : e->binding == 25 ? v->shadowAtlas : e->binding == 8 ? v->depthTexture : e->binding == 58 ? v->floatColor : e->binding == 59 ? v->floatSurface : NULL;
                SEND(void, s->encoder, "setTexture:atIndex:", (id, SEL, id, NSUInteger), t ? t : v->blank, at);
            }
            break;
        default:
            if (e->count > 1) {
                const NSRange range = {at, e->count};
                SEND(void, s->encoder, "setSamplerStates:withRange:", (id, SEL, id*, NSRange), v->samplerTable, range);
            } else {
                SEND(void, s->encoder, "setSamplerState:atIndex:", (id, SEL, id, NSUInteger), unfiltered(e->binding) ? v->nearest : v->sampler, at);
            }
            break;
        }
    }
    return s->arguments;
}

/* Its set 1, as the light buffers' roles now fall (vulkan.c's roleSets): its buffer, and offset the place in it. */
static id rolesArguments(Metal* v, Shader* s, NSUInteger* offset) {
    const int key = v->role[0] * 9 + v->role[1] * 3 + v->role[2];
    *offset = s->roleStride * (NSUInteger)key;
    if (s->roleStamp[key] == v->stamp + 1) return s->roleArguments;
    s->roleStamp[key] = v->stamp + 1;
    Buffer* const three[3] = {&v->post[0], v->post[3].buffer ? &v->post[3] : &v->empty, v->post[4].buffer ? &v->post[4] : &v->empty};
    SEND(void, s->encoder1, "setArgumentBuffer:offset:", (id, SEL, id, NSUInteger), s->roleArguments, *offset);
    for (int k = 0; k < s->table.count; ++k) {
        const IrMslArgument* e = &s->table.items[k];
        if (e->set != 1 || e->kind != IR_MSL_BUFFER || e->binding > 2) continue;
        id b = three[v->role[e->binding]]->buffer ? three[v->role[e->binding]]->buffer : v->empty.buffer;
        SEND(void, s->encoder1, "setBuffer:offset:atIndex:", (id, SEL, id, NSUInteger, NSUInteger), b, (NSUInteger)0, (NSUInteger)e->id);
    }
    return s->roleArguments;
}

/* What a pipeline draws into and how (as webgpu.c's RenderState): color, the light (light: its format; 0: BGRA8); in
 * floats the surface words' target, written by what is solid; blending (1 premultiplied, 2 fog over what is behind, 3
 * added with alpha kept: the overdraw view's);
 * depth (0: no depth attachment); samples (0: the frame's). */
typedef struct RenderSpec { int light, surface; bool surfaceWrite; int blend; bool color, depth; int samples; bool vertices; } RenderSpec;

static bool pipelineMakeAs(Metal* v, Pipeline* p, uint32_t* vertexCode, size_t vertexBytes, uint32_t* fragmentCode, size_t fragmentBytes, id depth, int cull, const RenderSpec* r) {
    if (!shaderMake(v, &p->vertex, vertexCode, vertexBytes, false)) {
        artfFree(fragmentCode);
        return false;
    }
    p->hasFragment = fragmentCode != NULL;
    if (p->hasFragment && !shaderMake(v, &p->fragment, fragmentCode, fragmentBytes, false)) return false;
    id d = GET(id, objcClass("MTLRenderPipelineDescriptor"), "new");
    SET_ID(d, "setVertexFunction:", p->vertex.function);
    if (p->hasFragment) SET_ID(d, "setFragmentFunction:", p->fragment.function);
    if (r->vertices) SET_ID(d, "setVertexDescriptor:", v->vertexDescriptor);
    SET_U(d, "setRasterSampleCount:", r->samples ? r->samples : v->samples);
    if (r->depth) SET_U(d, "setDepthAttachmentPixelFormat:", PIXEL_DEPTH32);
    id attachments = GET(id, d, "colorAttachments");
    id attachment = AT(attachments, 0);
    SET_U(attachment, "setPixelFormat:", r->light ? r->light : PIXEL_BGRA8);
    if (!r->color) SET_U(attachment, "setWriteMask:", 0);
    if (r->blend) {
        SET_B(attachment, "setBlendingEnabled:", 1);
        if (r->blend == 3) {
            SET_U(attachment, "setSourceRGBBlendFactor:", BLEND_ONE);
            SET_U(attachment, "setDestinationRGBBlendFactor:", BLEND_ONE);
            SET_U(attachment, "setSourceAlphaBlendFactor:", BLEND_ZERO);
            SET_U(attachment, "setDestinationAlphaBlendFactor:", BLEND_ONE);
        } else if (r->blend == 2) {
            /* fog over what is behind: light * what is left + added * what shows of it, alpha kept */
            SET_U(attachment, "setSourceRGBBlendFactor:", BLEND_DESTINATION_ALPHA);
            SET_U(attachment, "setDestinationRGBBlendFactor:", BLEND_SOURCE_ALPHA);
            SET_U(attachment, "setSourceAlphaBlendFactor:", BLEND_ZERO);
            SET_U(attachment, "setDestinationAlphaBlendFactor:", BLEND_ONE);
        } else {
            SET_U(attachment, "setSourceRGBBlendFactor:", BLEND_ONE);
            SET_U(attachment, "setSourceAlphaBlendFactor:", BLEND_ONE);
            SET_U(attachment, "setDestinationRGBBlendFactor:", BLEND_ONE_MINUS_SOURCE_ALPHA);
            SET_U(attachment, "setDestinationAlphaBlendFactor:", BLEND_ONE_MINUS_SOURCE_ALPHA);
        }
    }
    if (r->surface) {
        id second = AT(attachments, 1);
        SET_U(second, "setPixelFormat:", r->surface);
        if (!r->surfaceWrite) SET_U(second, "setWriteMask:", 0);
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
    return true;
}

/* A compute pipeline of shaders.c's. */
static bool computeMake(Metal* v, Compute* c, uint32_t* code, size_t bytes) {
    c->shader.threads = localSizeOf(code, bytes);
    if (!shaderMake(v, &c->shader, code, bytes, false)) return false;
    id error = NULL;
    c->state = SEND(id, v->device, "newComputePipelineStateWithFunction:error:", (id, SEL, id, id*), c->shader.function, &error);
    if (!c->state) {
        artfLog("artful: a Metal compute pipeline failed: %s", errorText(error));
        return false;
    }
    return true;
}

/* depth: its state; blend: premultiplied; color: writes color; shadow: into the atlas, depth alone. */
static bool pipelineMake(Metal* v, Pipeline* p, uint32_t* vertexCode, size_t vertexBytes, bool flipY, uint32_t* fragmentCode, size_t fragmentBytes, id depth, int cull, bool blend, bool color, bool hasVertices, bool shadow) {
    if (!shaderMake(v, &p->vertex, vertexCode, vertexBytes, flipY)) {
        artfFree(fragmentCode);
        return false;
    }
    p->hasFragment = fragmentCode != NULL;
    if (p->hasFragment && !shaderMake(v, &p->fragment, fragmentCode, fragmentBytes, false)) return false;
    id d = GET(id, objcClass("MTLRenderPipelineDescriptor"), "new");
    SET_ID(d, "setVertexFunction:", p->vertex.function);
    if (p->hasFragment) SET_ID(d, "setFragmentFunction:", p->fragment.function);
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

static bool postProgramsMake(Metal* v);

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
    v->depthOff = depthState(v, COMPARE_ALWAYS, false);
    v->depthNear = depthState(v, COMPARE_LESS, true);  /* (these maps are not) */

    const uint32_t textures = v->maxTextures | (v->uniformTextures ? ARTF_TEXTURES_UNIFORM : 0u);
    size_t size = 0, more = 0;
    uint32_t* code = artfShaderCull(&size, ARTF_CULL_ALL);
    if (!shaderMake(v, &v->cullShader, code, size, false)) return false;
    id error = NULL;
    v->cull = SEND(id, v->device, "newComputePipelineStateWithFunction:error:", (id, SEL, id, id*), v->cullShader.function, &error);
    if (!v->cull) {
        artfLog("artful: a Metal compute pipeline failed: %s", errorText(error));
        return false;
    }
    /* (each shader made before its size is read) */
    uint32_t *vertex, *fragment;
#define PIPELINE(p, vertexCall, flip, fragmentCall, ...) \
    (vertex = vertexCall, more = size, fragment = fragmentCall, pipelineMake(v, p, vertex, more, flip, fragment, size, __VA_ARGS__))
    v->depthEqual = depthState(v, COMPARE_EQUAL, false);
    return PIPELINE(&v->overlayDraw, artfShaderOverlayVertex(&size), false, artfShaderOverlayFragment(&size, false), v->depthOff, CULL_NONE, true, true, false, false) &&
           PIPELINE(&v->shadow, artfShaderShadowVertex(&size), true, (uint32_t*)NULL, v->depthNear, CULL_NONE, false, false, true, true) &&
           PIPELINE(&v->shadowHoles, artfShaderShadowVertex(&size), true, artfShaderShadowFragment(&size, textures), v->depthNear, CULL_NONE, false, false, true, true) &&
           (!v->floats || postProgramsMake(v));
#undef PIPELINE
}

/* ---- what is drawn into ---- */

static void targetsFree(Metal* v) {
    release(v->frameTexture);
    release(v->samplesTexture);
    release(v->depthTexture);
    release(v->overlay);
    release(v->floatColor);
    release(v->floatSamples);
    release(v->floatSurface);
    v->frameTexture = v->samplesTexture = v->depthTexture = v->overlay = v->floatColor = v->floatSamples = v->floatSurface = NULL;
    ++v->stamp;
    v->overlayUsable = false;
}

static bool postBuffersMake(Metal* v);

static bool targetsMake(Metal* v) {
    targetsFree(v);
    v->targetsStale = false;
    if (v->width <= 0 || v->height <= 0) return true;
    v->frameTexture = textureNew(v, PIXEL_BGRA8, v->width, v->height, 1, 1, TEXTURE_TARGET | TEXTURE_READ, STORAGE_PRIVATE);
    /* (with renderScale, in floats, the scene is drawn smaller, and made whole at the frame's size after it) */
    v->sceneWidth = v->floats ? artfMaxi((int)((float)v->width * v->scale + .5f), 1) : v->width;
    v->sceneHeight = v->floats ? artfMaxi((int)((float)v->height * v->scale + .5f), 1) : v->height;
    v->depthTexture = textureNew(v, PIXEL_DEPTH32, v->sceneWidth, v->sceneHeight, 1, v->samples, TEXTURE_TARGET | (v->floats ? TEXTURE_READ : 0), STORAGE_PRIVATE);
    if (v->samples > 1) v->samplesTexture = textureNew(v, PIXEL_BGRA8, v->width, v->height, 1, v->samples, TEXTURE_TARGET, STORAGE_PRIVATE);
    if (v->floats) {
        v->floatColor = textureNew(v, PIXEL_RGBA16F, v->sceneWidth, v->sceneHeight, 1, 1, TEXTURE_TARGET | TEXTURE_READ, STORAGE_PRIVATE);
        if (v->samples > 1) v->floatSamples = textureNew(v, PIXEL_RGBA16F, v->sceneWidth, v->sceneHeight, 1, v->samples, TEXTURE_TARGET, STORAGE_PRIVATE);
        v->floatSurface = textureNew(v, v->wide ? PIXEL_RGBA32U : PIXEL_R32U, v->sceneWidth, v->sceneHeight, 1, v->samples, TEXTURE_TARGET | TEXTURE_READ, STORAGE_PRIVATE);
        if (!v->floatColor || !v->floatSurface || !postBuffersMake(v)) return false;
    }
    v->overlay = textureNew(v, PIXEL_BGRA8, v->width, v->height, 1, 1, TEXTURE_READ, hostStorage(v));
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
    if (!bufferRoom(v, &v->vertices, vertices * sizeof(ArtfPackedVertex))) return false;
    if (!bufferRoom(v, &v->indices, indices * 4)) return false;
    if (!bufferRoom(v, &v->colors, colors * 4)) return false;
    v->meshes.len = 0;
    uint32_t atVertex = 0, atIndex = 0, atColor = 1;
    ((uint32_t*)v->colors.mapped)[0] = ~(uint32_t)0; /* (white: what a mesh without colors reads) */
    for (size_t i = 0; i < scene->meshes.len; ++i) {
        const ArtfMeshData* m = &scene->meshes.items[i];
        MeshRange range = {0, 0, 0, 0, m->skin ? m->pose.applied : m->deformVersion};
        if (m->vertexCount && !m->removed && !m->evicted) {
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
        if (!bufferNew(v, &v->meshTable, (size_t)v->meshCapacity * 80)) return false;
        if (!bufferNew(v, &v->indirect, (size_t)v->meshCapacity * 80)) return false;
        if (!bufferNew(v, &v->template_, (size_t)v->meshCapacity * 80)) return false;
        /* (the cull shader asks of meshlets, and is told none) */
        if (!bufferNew(v, &v->meshletMeshes, (size_t)v->meshCapacity * 16)) return false;
        if (!bufferNew(v, &v->meshletState, 64)) return false;
    }
    return true;
}

static uint32_t layersAt(const Metal* v) { return v->instanceCapacity * (ARTF_MAX_LODS + 1) * 4; }

static bool syncScene(Metal* v, ArtfScene* scene) {
    if (v->sceneId != scene->id) {
        v->sceneId = scene->id;
        for (size_t i = 0; i < v->textures.len; ++i) release(v->textures.items[i]);
        v->textures.len = v->textureMarks.len = 0;
        for (int i = 0; i < MAX_TEXTURES; ++i) v->textureTable[i] = v->blank;
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
        float* out = v->meshTable.mapped;
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
            row[19] = m->dagBounds ? 1.f : 0.f; /* (a graph of clusters: shaders.c's drawnErrorSpv) */
            memcpy(out, row, sizeof row);
        }
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
                if (i >= v->textures.len) continue;
                v->textureTable[i] = v->blank;
                v->textureMarks.items[i] = mark;
                ++v->stamp;
                continue;
            }
            if (!uploadTexture(v, t, (uint32_t)i)) return false;
        }
    }
    if (scene->instances.len > v->instanceCapacity) {
        v->instanceCapacity = (uint32_t)scene->instances.len * 2;
        if (!bufferNew(v, &v->instances, (size_t)v->instanceCapacity * (sizeof(ArtfInstanceData) + 64))) return false;
        if (!bufferNew(v, &v->visible, ((size_t)layersAt(v) + v->instanceCapacity) * 8)) return false;
        v->fresh = true;
    }
    if (scene->materials.len > v->materialCapacity) {
        v->materialCapacity = (uint32_t)scene->materials.len * 2;
        if (!bufferNew(v, &v->materials, (size_t)v->materialCapacity * (sizeof(ArtfMaterialData) + sizeof(ArtfMaterialMore)))) return false;
        v->fresh = true;
    }
    if (v->fresh || v->materialVersion != scene->materialVersion) {
        /* (a coat and sheen after them all, as Vulkan's: ArtfFrameData.materials says where) */
        memcpy(v->materials.mapped, scene->materials.items, scene->materials.len * sizeof(ArtfMaterialData));
        memcpy((char*)v->materials.mapped + scene->materials.len * sizeof(ArtfMaterialData), scene->materialsMore.items, scene->materials.len * sizeof(ArtfMaterialMore));
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

/* Every caster in every view: its instance and the mesh it is drawn with, by kind and mesh (as webgpu.c's). */
static bool syncShadowList(Metal* v, const ArtfScene* scene) {
    const uint32_t meshes = (uint32_t)v->meshes.len;
    if (v->shadowInstances == scene->instanceVersion && v->shadowStructure == scene->structureVersion && v->shadowMaterials == scene->materialVersion && v->shadowScene == scene->id && v->shadowMeshes == meshes) return true;
    if (!bufferRoom(v, &v->shadowList, (scene->instances.len + 1) * 8)) return false;
    if (!bufferRoom(v, &v->shadowCommands, (size_t)meshes * 40 + 40)) return false;
    artfVecReserve(&v->copyStarts, (size_t)meshes * 2 + 1);
    uint32_t* counts = v->copyStarts.items;
    memset(counts, 0, ((size_t)meshes * 2 + 1) * 4);
#define KIND(d) (scene->materials.items[(d)->material].flags & ARTF_MATERIAL_MASK ? meshes : 0)
    for (size_t i = 0; i < scene->instances.len; ++i) {
        const ArtfInstanceData* d = &scene->instances.items[i];
        if (!d->translucent) ++counts[KIND(d) + artfMeshHeld(scene, d->mesh)];
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
        const uint32_t held = artfMeshHeld(scene, d->mesh);
        const uint32_t slot = counts[KIND(d) + held]++;
        list[slot * 2] = (uint32_t)i;
        list[slot * 2 + 1] = held;
    }
#undef KIND
    v->shadowInstances = scene->instanceVersion;
    v->shadowStructure = scene->structureVersion;
    v->shadowMaterials = scene->materialVersion;
    v->shadowScene = scene->id;
    v->shadowMeshes = meshes;
    return true;
}

/* Lights past the frame's own, as Vulkan's (syncLights). */
static bool syncLights(Metal* v, const ArtfScene* scene, ArtfFrameData* frame) {
    if (frame->eye[2][3] <= 0) return true;
    if (!v->lightScratch) v->lightScratch = artfMalloc(ARTF_MAX_LIGHTS * sizeof(ArtfLightData));
    const uint32_t count = artfSceneExtraLights(scene, frame, v->lightScratch);
    artfSceneLightSlices(frame, v->lightScratch, count);
    uint32_t omitted = 0;
    const size_t need = artfSceneLightTiles(frame, v->width, v->height, v->lightScratch, count, NULL, &v->listScratch, &v->listScratchSize, &omitted) * 4;
    if (!bufferRoom(v, &v->tileLights, need)) return false;
    memcpy(v->extraLights.mapped, v->lightScratch, (size_t)count * sizeof(ArtfLightData));
    memcpy(v->tileLights.mapped, v->listScratch, need);
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

enum { NEED_OCCLUSION = 1, NEED_BLUR = 2, NEED_FOG = 4, NEED_SECOND = 8, NEED_HISTORY = 16, NEED_GATHERED = 32 };
enum { POST_GATHER, POST_OCCLUSION, POST_OCCLUSION_BLUR, POST_REFLECTION, POST_FROXELS, POST_TEMPORAL, POST_FOCUS, POST_BLOOM_DOWN, POST_BLOOM_UP, POST_REFLECTION_APPLY, POST_METER,
       POST_MOTION_BLUR, POST_VELOCITY, POST_TILE_MOST, POST_UPSCALE, POST_SHARPEN, POST_SAMPLED, POST_SAMPLED_SPACE, POST_CLOUD_COLUMNS, POST_CLOUD_APPLY, POST_UNDER_ACROSS, POST_UNDER_DOWN, POST_KINDS };

/* What goes into a buffer, at a place, with room made (and every argument written again) when there is not. */
static bool bufferPut(Metal* v, Buffer* b, size_t at, const void* data, size_t bytes) {
    if (!b->buffer || at + bytes > b->size) {
        Buffer was = *b;
        memset(b, 0, sizeof *b);
        if (!bufferNew(v, b, (at + bytes) + (at + bytes) / 2 + 64)) return false;
        if (was.buffer) memcpy(b->mapped, was.mapped, was.size < b->size ? was.size : b->size);
        bufferFree(&was);
    }
    if (bytes) memcpy((char*)b->mapped + at, data, bytes);
    return true;
}

/* All that shaders reach through argument buffers: Metal cannot see it there, and is told. The atlas comes last,
 * for a pass that draws into it to leave out. */
static void resourcesGather(Metal* v) {
    v->resources.len = 0;
    Buffer* buffers[] = {&v->empty, &v->frame, &v->instances, &v->visible, &v->indirect, &v->materials, &v->meshTable, &v->colors, &v->shadowData,
                         &v->shadowList, &v->meshletMeshes, &v->meshletState, &v->extraLights, &v->tileLights, &v->decals,
                         &v->post[0], &v->post[1], &v->post[2], &v->post[3], &v->post[4], &v->post[5], &v->post[6], &v->post[7], &v->postLocks, &v->postGathered,
                         &v->cloudField, &v->cloudGrid, &v->environment, &v->traceNodes, &v->traceTriangles, &v->traceVertices, &v->traceTop, &v->traceInstances,
                         &v->traceSum, &v->traceFrameUbo, &v->denoiseBuffers[0], &v->denoiseBuffers[1], &v->denoiseBuffers[2], &v->denoiseBuffers[3],
                         &v->denoiseBuffers[4], &v->denoiseBuffers[5], &v->denoiseBuffers[6], &v->denoiseShift, &v->traceMotion, &v->probesNow, &v->probesNext,
                         &v->backgroundBlock, &v->backgroundLight, &v->backgroundBloom, &v->backgroundData, &v->backgroundStream};
    for (size_t i = 0; i < ARTF_COUNT(buffers); ++i)
        if (buffers[i]->buffer) artfVecPush(&v->resources, buffers[i]->buffer);
    artfVecPush(&v->resources, v->blank);
    for (size_t i = 0; i < v->textures.len; ++i)
        if (v->textures.items[i]) artfVecPush(&v->resources, v->textures.items[i]);
    if (v->overlay) artfVecPush(&v->resources, v->overlay);
    if (v->floats) {
        /* (what the gather reads; drawn into in the scene's passes, read after them) */
        if (v->depthTexture) artfVecPush(&v->resources, v->depthTexture);
        if (v->floatColor) artfVecPush(&v->resources, v->floatColor);
        if (v->floatSurface) artfVecPush(&v->resources, v->floatSurface);
    }
    if (v->shadowAtlas) artfVecPush(&v->resources, v->shadowAtlas);
}

static void computeUse(Metal* v, id e) {
    SEND(void, e, "useResources:count:usage:", (id, SEL, id*, NSUInteger, NSUInteger), v->resources.items, (NSUInteger)v->resources.len, (NSUInteger)USE_READ_WRITE);
}

/* (in a pass that draws into them, the floats' targets and the atlas are not told: a target is not a resource read) */
static void renderUse(Metal* v, id e, bool withAtlas) {
    NSUInteger count = v->resources.len - (!withAtlas && v->shadowAtlas ? 1 : 0);
    if (withAtlas && v->floats) {
        /* (the scene's passes: all but the floats' targets, which come just before the atlas) */
        id kept[512];
        NSUInteger n = 0;
        for (size_t i = 0; i < v->resources.len && n < 512; ++i) {
            id r = v->resources.items[i];
            if (r == v->depthTexture || r == v->floatColor || r == v->floatSurface) continue;
            kept[n++] = r;
        }
        SEND(void, e, "useResources:count:usage:stages:", (id, SEL, id*, NSUInteger, NSUInteger, NSUInteger), kept, n, (NSUInteger)USE_READ_WRITE, (NSUInteger)STAGE_VERTEX_FRAGMENT);
        return;
    }
    SEND(void, e, "useResources:count:usage:stages:", (id, SEL, id*, NSUInteger, NSUInteger, NSUInteger), v->resources.items, count, (NSUInteger)USE_READ_WRITE, (NSUInteger)STAGE_VERTEX_FRAGMENT);
}

static void pipelineUse(Metal* v, id e, Pipeline* p) {
    SET_ID(e, "setRenderPipelineState:", p->state);
    SET_ID(e, "setDepthStencilState:", p->depth);
    SET_U(e, "setCullMode:", p->cull);
    if (p->vertex.table.count) SEND(void, e, "setVertexBuffer:offset:atIndex:", (id, SEL, id, NSUInteger, NSUInteger), arguments(v, &p->vertex), (NSUInteger)0, (NSUInteger)0);
    if (p->hasFragment && p->fragment.table.count) SEND(void, e, "setFragmentBuffer:offset:atIndex:", (id, SEL, id, NSUInteger, NSUInteger), arguments(v, &p->fragment), (NSUInteger)0, (NSUInteger)0);
    if (p->hasFragment && p->fragment.encoder1) {
        NSUInteger offset;
        id b = rolesArguments(v, &p->fragment, &offset);
        SEND(void, e, "setFragmentBuffer:offset:atIndex:", (id, SEL, id, NSUInteger, NSUInteger), b, offset, (NSUInteger)1);
    }
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

/* A compute pass's dispatch, on an encoder: its push constant (where it has one), its sets, groups of its own size. */
static void computeStep(Metal* v, id e, Compute* c, const uint32_t* push, NSUInteger pushBytes, MTLSize groups) {
    SET_ID(e, "setComputePipelineState:", c->state);
    if (c->shader.table.count) SEND(void, e, "setBuffer:offset:atIndex:", (id, SEL, id, NSUInteger, NSUInteger), arguments(v, &c->shader), (NSUInteger)0, (NSUInteger)0);
    if (c->shader.encoder1) {
        NSUInteger offset;
        id b = rolesArguments(v, &c->shader, &offset);
        SEND(void, e, "setBuffer:offset:atIndex:", (id, SEL, id, NSUInteger, NSUInteger), b, offset, (NSUInteger)1);
    }
    if (push && c->shader.table.pushAt >= 0) SEND(void, e, "setBytes:length:atIndex:", (id, SEL, const void*, NSUInteger, NSUInteger), push, pushBytes, (NSUInteger)c->shader.table.pushAt);
    SEND(void, e, "dispatchThreadgroups:threadsPerThreadgroup:", (id, SEL, MTLSize, MTLSize), groups, c->shader.threads);
}

/* A post pass: count invocations of 64 (postStep). */
static void postStep(Metal* v, id e, int kind, uint32_t push, uint32_t count) {
    const uint32_t words[4] = {push, 0, 0, 0};
    const MTLSize groups = {(count + 63) / 64, 1, 1};
    computeStep(v, e, &v->posts[kind], words, sizeof words, groups);
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
        const NSUInteger pushAt = (NSUInteger)(v->shadow.vertex.table.pushAt >= 0 ? v->shadow.vertex.table.pushAt : 1);
        SEND(void, e, "setVertexBytes:length:atIndex:", (id, SEL, const void*, NSUInteger, NSUInteger), push, (NSUInteger)sizeof push, pushAt);
        pipelineUse(v, e, &v->shadow);
        drawCommands(v, e, v->shadowCommands.buffer, 0, meshes);
        pipelineUse(v, e, &v->shadowHoles);
        drawCommands(v, e, v->shadowCommands.buffer, meshes, meshes);
    }
    DO(e, "endEncoding");
}

/* ---- the scene's pipelines, as the scene and the frame are ---- */

static void pipelineFree(Pipeline* p);
static void shaderFree(Shader* s);
static void computeFree(Compute* c) {
    release(c->state);
    shaderFree(&c->shader);
    memset(c, 0, sizeof *c);
}

/* A pipeline of a triangle over the frame (the overlay's vertices), with a pixel shader of shaders.c's. */
static bool fullMake(Metal* v, Pipeline* p, uint32_t* fragmentCode, size_t fragmentBytes, id depth, const RenderSpec* r) {
    size_t size = 0;
    uint32_t* vertex = artfShaderOverlayVertex(&size);
    return pipelineMakeAs(v, p, vertex, size, fragmentCode, fragmentBytes, depth, CULL_NONE, r);
}

/* The scene's pipelines (as webgpu.c's scenePrograms): what is solid; the cutout's (holes, and materials of the
 * program's own), a layer's depth alone, the layer at that depth blended, particles; and what draws behind the scene
 * (light from all around, fog) and what shows what tracing gathered, in the scene's targets. */
static bool scenePrograms(Metal* v, const ArtfGraphData* graphs, uint32_t count) {
    const uint32_t variant = (v->floats ? ARTF_SHADER_FLOATS : 0u) | (v->froxels ? ARTF_SHADER_FROXELS : 0u) | v->variantBits;
    const uint32_t textures = v->maxTextures | (v->uniformTextures ? ARTF_TEXTURES_UNIFORM : 0u);
    const int light = v->floats ? PIXEL_RGBA16F : 0, surface = !v->floats ? 0 : v->wide ? PIXEL_RGBA32U : PIXEL_R32U;
    Pipeline* const programs[5] = {&v->scene, &v->cutout, &v->layerDepth, &v->layerDraw, &v->particle};
    const id depths[5] = {v->depthOn, v->depthOn, v->depthOn, v->depthEqual, v->depthOn};
    const int culls[5] = {CULL_BACK, CULL_NONE, CULL_NONE, CULL_NONE, CULL_NONE};
    const RenderSpec specs[5] = {{light, surface, true, 0, true, true, 0, true}, {light, surface, true, 0, true, true, 0, true}, {light, surface, false, 0, false, true, 0, true},
                                 {light, surface, false, 1, true, true, 0, true}, {light, surface, false, 1, true, true, 0, true}};
    for (int k = 0; k < 5; ++k) {
        pipelineFree(programs[k]);
        size_t size = 0, vertexSize = 0;
        uint32_t* vertex = artfShaderSceneVertex(&vertexSize, v->floats);
        uint32_t* fragment = artfShaderSceneFragment(&size, textures, k != 0, k ? graphs : NULL, k ? count : 0, 0, variant);
        if (!pipelineMakeAs(v, programs[k], vertex, vertexSize, fragment, size, depths[k], culls[k], &specs[k])) return false;
    }
    Pipeline* const behind[3] = {&v->environmentBehind, &v->fogBehind, &v->traceShow};
    for (int k = 0; k < 3; ++k) pipelineFree(behind[k]);
    const RenderSpec over = {light, surface, false, 0, true, true, 0}, fog = {light, surface, false, 2, true, true, 0};
    size_t size = 0;
    uint32_t* code = artfShaderEnvironmentBackdrop(&size, v->floats);
    if (!fullMake(v, &v->environmentBehind, code, size, v->depthOff, &over)) return false;
    code = artfShaderTraceShow(&size, v->floats);
    if (!fullMake(v, &v->traceShow, code, size, v->depthOff, &over)) return false;
    if (v->floats) {
        code = artfShaderFogBackdrop(&size);
        if (!fullMake(v, &v->fogBehind, code, size, v->depthOff, &fog)) return false;
    }
    return true;
}

static bool graphsSync(Metal* v, const ArtfScene* scene) {
    const uint32_t count = (uint32_t)scene->graphs.len;
    const uint32_t variant = (v->froxels ? 1u : 0u) | v->variantBits << 1;
    if (v->programsMade && count == v->graphCount && (!count || v->graphScene == scene->id) && v->madeWide == v->wide && v->madeVariant == variant) return true;
    if (!scenePrograms(v, scene->graphs.items, count)) return false;
    v->graphCount = count;
    v->graphScene = scene->id;
    /* (made again when next asked for: their targets are the scene's) */
    pipelineFree(&v->overdraw[0]);
    pipelineFree(&v->overdraw[1]);
    pipelineFree(&v->backdrop);
    v->madeWide = v->wide;
    v->madeVariant = variant;
    v->programsMade = true;
    return true;
}

/* ---- a frame in floats: what comes after the scene (as webgpu.c's, from vulkan.c's) ---- */

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

static bool postBuffersMake(Metal* v) {
    const size_t pixels = (size_t)v->sceneWidth * v->sceneHeight, framePixels = (size_t)v->width * v->height;
    const size_t fog = (size_t)((v->sceneWidth + ARTF_FROXEL_TILE - 1) / ARTF_FROXEL_TILE) * ((v->sceneHeight + ARTF_FROXEL_TILE - 1) / ARTF_FROXEL_TILE) * ARTF_FROXEL_SLICES;
    size_t bloom = 0;
    uint32_t w = (uint32_t)v->width, h = (uint32_t)v->height;
    for (int l = 0; l < 6; ++l) {
        w = (w + 1) / 2 ? (w + 1) / 2 : 1;
        h = (h + 1) / 2 ? (h + 1) / 2 : 1;
        v->bloomSizes[l][0] = (uint32_t)bloom;
        v->bloomSizes[l][1] = w;
        v->bloomSizes[l][2] = h;
        bloom += (size_t)w * h;
    }
    for (int i = 0; i < 8; ++i) bufferFree(&v->post[i]);
    bufferFree(&v->postGathered);
    bufferFree(&v->postLocks);
    const size_t tiles = (size_t)((v->width + 15) / 16) * ((v->height + 15) / 16);
    const size_t occlusion = v->postNeeds & NEED_OCCLUSION ? pixels * 4 : 0, blur = v->postNeeds & NEED_BLUR ? (framePixels + tiles * 2) * 8 : 0;
    const size_t sizes[8] = {framePixels * 8, pixels * 4, pixels * 12, v->postNeeds & NEED_SECOND ? framePixels * 8 : 0, v->postNeeds & NEED_HISTORY ? framePixels * 8 : 0,
                             occlusion > blur ? occlusion : blur, v->postNeeds & NEED_FOG ? fog * 16 : 0, (bloom + 1 + v->lutRoom) * 16};
    for (int i = 0; i < 8; ++i)
        if (sizes[i] && !bufferNew(v, &v->post[i], sizes[i])) return false;
    if (v->postNeeds & NEED_GATHERED && !bufferNew(v, &v->postGathered, pixels * 64)) return false;
    if (v->postNeeds & NEED_HISTORY && pixels != framePixels && !bufferNew(v, &v->postLocks, framePixels * 8)) return false;
    v->role[0] = 0, v->role[1] = 1, v->role[2] = 2;
    v->historyValid = v->exposureValid = v->hasPrevious = v->gatheredValid = false;
    v->lutVersion = 0;
    ++v->stamp;
    return true;
}

static bool syncLut(Metal* v, const ArtfScene* scene) {
    if (!scene->lut || (v->lutScene == scene->id && v->lutVersion == scene->lutVersion)) return true;
    const uint32_t entries = (uint32_t)(scene->lutFloats / 3);
    const size_t bloom = (size_t)v->bloomSizes[5][0] + (size_t)v->bloomSizes[5][1] * v->bloomSizes[5][2];
    if (entries > v->lutRoom) {
        v->lutRoom = entries;
        if (!bufferNew(v, &v->post[7], (bloom + 1 + v->lutRoom) * 16)) return false;
        v->exposureValid = false;
    }
    float* rows = (float*)((char*)v->post[7].mapped + (bloom + 1) * 16);
    for (uint32_t i = 0; i < entries; ++i) {
        memcpy(rows + i * 4, scene->lut + i * 3, 12);
        rows[i * 4 + 3] = 0;
    }
    v->lutScene = scene->id;
    v->lutVersion = scene->lutVersion;
    return true;
}

static bool cloudsSync(Metal* v, const ArtfScene* scene, uint32_t* columns, uint32_t* rows) {
    *columns = ((uint32_t)v->sceneWidth + ARTF_CLOUD_TILE - 1) / ARTF_CLOUD_TILE;
    *rows = ((uint32_t)v->sceneHeight + ARTF_CLOUD_TILE - 1) / ARTF_CLOUD_TILE;
    const size_t grid = (size_t)*columns * *rows * ARTF_CLOUD_CELL * 4;
    if (!v->cloudField.buffer && !bufferNew(v, &v->cloudField, (size_t)ARTF_CLOUD_FLOATS * 4)) return false;
    if (v->cloudGrid.size != grid) {
        if (!bufferNew(v, &v->cloudGrid, grid)) return false;
        v->cloudGeneration = ~(uint64_t)0;
    }
    if (v->cloudGeneration != scene->cloudsGeneration) {
        memcpy(v->cloudField.mapped, scene->clouds, (size_t)ARTF_CLOUD_FLOATS * 4);
        v->cloudGeneration = scene->cloudsGeneration;
    }
    return true;
}

static bool syncEnvironment(Metal* v, const ArtfScene* scene) {
    const size_t full = ARTF_ENV_FLOATS * sizeof(float);
    if (!v->environment.buffer && !bufferNew(v, &v->environment, full)) return false;
    if (scene->environment && (v->environmentVersion != scene->environmentVersion || v->environmentScene != scene->id)) {
        memcpy(v->environment.mapped, scene->environment, full);
        v->environmentVersion = scene->environmentVersion;
        v->environmentScene = scene->id;
    }
    return true;
}

static bool postProgramsMake(Metal* v) {
    uint32_t* (*const makers[POST_KINDS])(size_t*) = {NULL, artfShaderPostOcclusion, artfShaderPostOcclusionBlur, artfShaderPostReflection, artfShaderPostFroxels,
                                                      artfShaderPostTemporal, artfShaderPostFocus, artfShaderPostBloomDown, artfShaderPostBloomUp, artfShaderPostReflectionApply,
                                                      artfShaderPostMeter, artfShaderPostMotionBlur, artfShaderPostVelocity, artfShaderPostTileMost, artfShaderPostUpscale,
                                                      artfShaderPostSharpen, artfShaderPostSampled, artfShaderPostSampledSpace, artfShaderPostCloudColumns, artfShaderPostCloudApply,
                                                      artfShaderPostUnderAcross, artfShaderPostUnderDown};
    for (int k = 0; k < POST_KINDS; ++k) {
        size_t size = 0;
        uint32_t* code = k == POST_GATHER ? artfShaderPostGather(&size, v->samples) : makers[k](&size);
        if (!computeMake(v, &v->posts[k], code, size)) {
            artfLog("artful: Metal's post pass %d could not be made", k);
            return false;
        }
    }
    size_t size = 0;
    const RenderSpec one = {0, 0, false, 0, true, false, 1}, over = {0, 0, false, 1, true, false, 1};
    uint32_t* code = artfShaderPostFinalFor(&size, false, NULL);
    if (!fullMake(v, &v->final, code, size, v->depthOff, &one)) return false;
    code = artfShaderOverlayFragment(&size, false);
    return fullMake(v, &v->overlayFloats, code, size, v->depthOff, &over);
}

static void postSwap(Metal* v, bool keep) {
    const uint8_t light = v->role[1], was = v->role[0];
    if (keep) v->role[2] = light;
    v->role[0] = light;
    v->role[1] = was != v->role[2] && was != light ? was : (uint8_t)(3 - light - v->role[2]);
}

/* The post passes, in post.c's order (postRecord). */
static void postChain(Metal* v, id commands, const ArtfFrameData* frame, bool under, bool clouds, uint32_t cloudColumns, uint32_t cloudRows) {
    const uint32_t pixels = (uint32_t)v->sceneWidth * (uint32_t)v->sceneHeight, framePixels = (uint32_t)v->width * (uint32_t)v->height;
    const bool scaled = pixels != framePixels;
    id e = GET(id, commands, "computeCommandEncoder");
    computeUse(v, e);
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
        const uint32_t half = v->bloomSizes[0][1] * v->bloomSizes[0][2];
        if (frame->traced & ARTF_TRACED_REFLECTIONS && v->reflectionTraced.state) {
            const MTLSize groups = {(half + 63) / 64, 1, 1};
            computeStep(v, e, &v->reflectionTraced, NULL, 0, groups);
        } else
            postStep(v, e, POST_REFLECTION, 0, half);
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
    DO(e, "endEncoding");
}

static void gatherDepth(Metal* v, id commands) {
    id e = GET(id, commands, "computeCommandEncoder");
    computeUse(v, e);
    postStep(v, e, POST_GATHER, 1, (uint32_t)v->sceneWidth * (uint32_t)v->sceneHeight);
    DO(e, "endEncoding");
}

static void finalPass(Metal* v, id commands) {
    id pass = GET(id, objcClass("MTLRenderPassDescriptor"), "renderPassDescriptor");
    id color = AT(GET(id, pass, "colorAttachments"), 0);
    SET_ID(color, "setTexture:", v->frameTexture);
    SET_U(color, "setLoadAction:", LOAD_CLEAR);
    SET_U(color, "setStoreAction:", STORE_STORE);
    id e = SEND(id, commands, "renderCommandEncoderWithDescriptor:", (id, SEL, id), pass);
    renderUse(v, e, true);
    SET_U(e, "setFrontFacingWinding:", WINDING_COUNTER_CLOCKWISE);
    pipelineUse(v, e, &v->final);
    drawTriangle(e);
    if (v->overlayPixels && v->overlayUsable) {
        pipelineUse(v, e, &v->overlayFloats);
        drawTriangle(e);
    }
    DO(e, "endEncoding");
}

/* ---- tracing and the probes, through the scene's trees (as webgpu.c's) ---- */

static bool treesSync(Metal* v, ArtfScene* scene) {
    artfSceneTraceSync(scene);
    const ArtfTrace* t = &scene->trace;
    if (v->traceScene != scene->id || v->traceMeshVersion != t->meshVersion || scene->deforming.len) {
        if (!bufferPut(v, &v->traceNodes, 0, t->wide.items, t->wide.len * sizeof *t->wide.items) || !bufferPut(v, &v->traceTriangles, 0, t->triangles.items, t->triangles.len * sizeof *t->triangles.items) ||
            !bufferPut(v, &v->traceVertices, 0, t->vertices.items, t->vertices.len * sizeof *t->vertices.items))
            return false;
        v->traceMeshVersion = t->meshVersion;
    }
    if (v->traceScene != scene->id || v->traceTopVersion != t->topVersion) {
        if (!bufferPut(v, &v->traceTop, 0, t->top.items, t->top.len * sizeof *t->top.items) ||
            !bufferPut(v, &v->traceInstances, 0, t->instances.items, (t->instances.len + 1) * sizeof *t->instances.items))
            return false;
        v->traceTopVersion = t->topVersion;
    }
    v->traceScene = scene->id;
    return true;
}

static bool traceSync(Metal* v, ArtfScene* scene, bool guides) {
    if (!treesSync(v, scene)) return false;
    const ArtfTrace* t = &scene->trace;
    const size_t pixels = (size_t)v->sceneWidth * v->sceneHeight;
    if (v->traceSum.size < pixels * 16) {
        if (!bufferNew(v, &v->traceSum, pixels * 16)) return false;
        v->traceDone = 0;
    }
    if (!v->traceFrameUbo.buffer && !bufferNew(v, &v->traceFrameUbo, sizeof(ArtfTraceFrame))) return false;
    if (guides && v->denoiseBuffers[0].size < pixels * 16) {
        for (uint32_t i = 0; i < 7; ++i)
            if (!bufferNew(v, &v->denoiseBuffers[i], pixels * (i == 3 || i == 4 ? 32 : 16))) return false;
        if (!bufferNew(v, &v->denoiseShift, pixels * 16)) return false;
        v->denoiseFirst = true;
    }
    if (guides && !bufferPut(v, &v->traceMotion, 0, t->motion.items, (t->motion.len + 12) * 4)) return false;
    return true;
}

static bool tracePrograms(Metal* v, const ArtfScene* scene, bool guides, bool more) {
    const uint32_t count = (uint32_t)scene->graphs.len, key = count << 2 | (guides ? 1u : 0u) | (more ? 2u : 0u);
    const uint32_t textures = v->maxTextures | (v->uniformTextures ? ARTF_TEXTURES_UNIFORM : 0u);
    if (!v->trace.state || key != v->traceProgramKey || (count && v->traceProgramScene != scene->id)) {
        computeFree(&v->trace);
        size_t size = 0;
        uint32_t* code = artfShaderTrace(&size, textures, scene->graphs.items, count, guides, false, more);
        if (!computeMake(v, &v->trace, code, size)) {
            artfLog("artful: Metal's trace shader could not be made");
            return false;
        }
        v->traceProgramKey = key;
        v->traceProgramScene = scene->id;
    }
    if (guides && !v->denoisePasses[0].state)
        for (int i = 0; i < 9; ++i) {
            size_t size = 0;
            uint32_t* code = i == 0 ? artfShaderDenoiseLight(&size) : i == 1 ? artfShaderDenoiseTime(&size) : i == 2 ? artfShaderDenoiseVariance(&size)
                             : i < 8 ? artfShaderDenoiseSpace(&size, i - 3) : artfShaderDenoiseEnd(&size);
            if (!computeMake(v, &v->denoisePasses[i], code, size)) return false;
        }
    return true;
}

static void traceRecord(Metal* v, id commands, bool clear, bool adding, bool denoise) {
    const size_t pixels = (size_t)v->sceneWidth * v->sceneHeight;
    if (clear) {
        id blit = GET(id, commands, "blitCommandEncoder");
        const NSRange all = {0, pixels * 16};
        SEND(void, blit, "fillBuffer:range:value:", (id, SEL, id, NSRange, unsigned char), v->traceSum.buffer, all, (unsigned char)0);
        DO(blit, "endEncoding");
    }
    if (!adding) return;
    const MTLSize groups = {((NSUInteger)v->sceneWidth + 7) / 8, ((NSUInteger)v->sceneHeight + 7) / 8, 1};
    id e = GET(id, commands, "computeCommandEncoder");
    computeUse(v, e);
    computeStep(v, e, &v->trace, NULL, 0, groups);
    for (int i = 0; denoise && i < 9; ++i) computeStep(v, e, &v->denoisePasses[i], NULL, 0, groups);
    DO(e, "endEncoding");
    if (denoise) {
        id blit = GET(id, commands, "blitCommandEncoder");
        SEL copy = selName("copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:");
        ((void (*)(id, SEL, id, NSUInteger, id, NSUInteger, NSUInteger))objcSend)(blit, copy, v->denoiseBuffers[0].buffer, 0, v->denoiseBuffers[2].buffer, 0, pixels * 16);
        ((void (*)(id, SEL, id, NSUInteger, id, NSUInteger, NSUInteger))objcSend)(blit, copy, v->denoiseBuffers[4].buffer, 0, v->denoiseBuffers[3].buffer, 0, pixels * 32);
        DO(blit, "endEncoding");
    }
}

static bool probesSync(Metal* v, ArtfScene* scene, bool* step) {
    *step = false;
    const ArtfIndirect* g = &scene->indirect;
    const size_t need = (size_t)artfIndirectCount(g) * ARTF_PROBE_FLOATS * 4;
    if (!treesSync(v, scene)) return false;
    if (v->probesNow.size < need) {
        if (!bufferNew(v, &v->probesNow, need) || !bufferNew(v, &v->probesNext, need)) return false;
        v->probeScene = 0;
    }
    const int rays = g->desc.rays > 0 ? g->desc.rays : 64;
    const float keep = g->desc.hysteresis > 0 ? artfClampf(g->desc.hysteresis, 0, .999f) : .9f;
    const bool walls = !g->desc.throughWalls, more = scene->moreMaterials > 0;
    uint32_t keepBits;
    memcpy(&keepBits, &keep, 4);
    const uint64_t key = (uint64_t)keepBits << 32 | (uint64_t)rays << 3 | (walls ? 1u : 0u) | (more ? 4u : 0u);
    if (!v->probeStep.state || v->probeKey != key || v->probeGraphs != scene->graphs.len || v->probeGraphScene != scene->id) {
        computeFree(&v->probeStep);
        size_t size = 0;
        const uint32_t textures = v->maxTextures | (v->uniformTextures ? ARTF_TEXTURES_UNIFORM : 0u);
        uint32_t* code = artfShaderProbes(&size, textures, scene->graphs.items, (uint32_t)scene->graphs.len, false, more, walls, rays, keep);
        if (!computeMake(v, &v->probeStep, code, size)) {
            artfLog("artful: Metal's probes' shader could not be made");
            return false;
        }
        v->probeKey = key;
        v->probeGraphs = (uint32_t)scene->graphs.len;
        v->probeGraphScene = scene->id;
    }
    if (v->probeScene != scene->id || v->probeSerial != g->serial) {
        v->probeScene = scene->id;
        v->probeSerial = g->serial;
        v->probeFrames = 0;
        if (g->frames && g->probes) {
            memcpy(v->probesNow.mapped, g->probes, need);
            v->probeFrames = g->frames;
        } else memset(v->probesNow.mapped, 0, need);
    }
    *step = true;
    return true;
}

static void probesRecord(Metal* v, id commands, const ArtfScene* scene) {
    const ArtfIndirect* g = &scene->indirect;
    id e = GET(id, commands, "computeCommandEncoder");
    computeUse(v, e);
    const uint32_t push[4] = {v->probeFrames, 0, 0, 0};
    const MTLSize groups = {(NSUInteger)(g->dims[0] * g->dims[1]), (NSUInteger)(g->dims[2] * (g->cascades > 0 ? g->cascades : 1)), 1};
    computeStep(v, e, &v->probeStep, push, sizeof push, groups);
    DO(e, "endEncoding");
    id blit = GET(id, commands, "blitCommandEncoder");
    SEL copy = selName("copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:");
    ((void (*)(id, SEL, id, NSUInteger, id, NSUInteger, NSUInteger))objcSend)(blit, copy, v->probesNext.buffer, 0, v->probesNow.buffer, 0, (NSUInteger)artfIndirectCount(g) * ARTF_PROBE_FLOATS * 4);
    DO(blit, "endEncoding");
    ++v->probeFrames;
}

/* A render pass into the scene's targets (in floats: the light and the surface words); clear or keep what is there. */
static id scenePass(Metal* v, id commands, const ArtfFrameData* frame, bool clear) {
    id pass = GET(id, objcClass("MTLRenderPassDescriptor"), "renderPassDescriptor");
    id colors = GET(id, pass, "colorAttachments");
    id color = AT(colors, 0);
    MTLClearColor clearColor = {frame->background[0], frame->background[1], frame->background[2], frame->background[3]};
    if (v->floats) {
        /* (the background as light, as on the CPU) */
        const float alpha = frame->background[3], over = alpha > 0 ? 1.f / alpha : 0.f;
        clearColor.red = artfSrgbDecode(frame->background[0] * over) * alpha;
        clearColor.green = artfSrgbDecode(frame->background[1] * over) * alpha;
        clearColor.blue = artfSrgbDecode(frame->background[2] * over) * alpha;
    }
    SET_U(color, "setLoadAction:", clear ? LOAD_CLEAR : LOAD_LOAD);
    SEND(void, color, "setClearColor:", (id, SEL, MTLClearColor), clearColor);
    id target = v->floats ? v->floatColor : v->frameTexture, samples = v->floats ? v->floatSamples : v->samplesTexture;
    if (v->samples > 1) {
        SET_ID(color, "setTexture:", samples);
        SET_ID(color, "setResolveTexture:", target);
        SET_U(color, "setStoreAction:", v->floats ? STORE_STORE_RESOLVE : STORE_RESOLVE);
    } else {
        SET_ID(color, "setTexture:", target);
        SET_U(color, "setStoreAction:", STORE_STORE);
    }
    if (v->floats) {
        id surface = AT(colors, 1);
        SET_ID(surface, "setTexture:", v->floatSurface);
        SET_U(surface, "setLoadAction:", clear ? LOAD_CLEAR : LOAD_LOAD);
        SET_U(surface, "setStoreAction:", STORE_STORE);
    }
    id depth = GET(id, pass, "depthAttachment");
    SET_ID(depth, "setTexture:", v->depthTexture);
    SET_U(depth, "setLoadAction:", clear ? LOAD_CLEAR : LOAD_LOAD);
    SET_U(depth, "setStoreAction:", v->floats ? STORE_STORE : STORE_DONT_CARE);
    SEND(void, depth, "setClearDepth:", (id, SEL, double), 0.0);
    id e = SEND(id, commands, "renderCommandEncoderWithDescriptor:", (id, SEL, id), pass);
    renderUse(v, e, true);
    /* (y goes up in clip space and down in a frame here, as Vulkan's flipped viewport has it) */
    SET_U(e, "setFrontFacingWinding:", WINDING_COUNTER_CLOCKWISE);
    return e;
}

/* The overdraw view's pipelines (as webgpu.c's overdrawMake): the scene's vertex shader and artfShaderOverdraw, added,
 * with no depth to test or write; the first culls back faces, the second none. */
static bool overdrawMake(Metal* v) {
    if (v->overdraw[0].state) return true;
    const int light = v->floats ? PIXEL_RGBA16F : 0, surface = !v->floats ? 0 : v->wide ? PIXEL_RGBA32U : PIXEL_R32U;
    const RenderSpec added = {light, surface, false, 3, true, true, 0, true};
    for (int k = 0; k < 2; ++k) {
        size_t size = 0, vertexSize = 0;
        uint32_t* vertex = artfShaderSceneVertex(&vertexSize, v->floats);
        uint32_t* fragment = artfShaderOverdraw(&size, v->floats);
        if (!pipelineMakeAs(v, &v->overdraw[k], vertex, vertexSize, fragment, size, v->depthOff, k ? CULL_NONE : CULL_BACK, &added)) return false;
    }
    return true;
}

/* A background pass's buffers, its shader when the scene's is another, the bloom's and what draws it behind (as
 * webgpu.c's backgroundSync). Memory is shared, and the frame before is done: what changed is written in place. */
static bool backgroundSync(Metal* v, const ArtfScene* scene, const ArtfBackgroundBlock* block, uint32_t bloomPixels) {
    const ArtfBackground* b = &scene->backgroundPass;
    const ArtfBackgroundPass* p = &b->pass;
    const size_t light = (size_t)v->sceneWidth * (size_t)v->sceneHeight * 16, bloom = (size_t)(bloomPixels ? bloomPixels : 1) * 16;
    if (v->backgroundLight.size != light && !bufferNew(v, &v->backgroundLight, light)) return false;
    if (v->backgroundBloom.size != bloom && !bufferNew(v, &v->backgroundBloom, bloom)) return false;
    if (!bufferPut(v, &v->backgroundBlock, 0, block, sizeof *block)) return false;
    if (v->backgroundDataStamp != b->dataStamp || v->backgroundDataScene != scene->id || !v->backgroundData.buffer) {
        if (!v->backgroundData.buffer && !bufferNew(v, &v->backgroundData, 16)) return false;
        if (p->data && !bufferPut(v, &v->backgroundData, 0, p->data, p->dataBytes)) return false;
        v->backgroundDataStamp = b->dataStamp;
        v->backgroundDataScene = scene->id;
    }
    if (v->backgroundStreamStamp != b->streamStamp || v->backgroundStreamScene != scene->id || !v->backgroundStream.buffer) {
        if (!v->backgroundStream.buffer && !bufferNew(v, &v->backgroundStream, 16)) return false;
        if (p->stream && !bufferPut(v, &v->backgroundStream, 0, p->stream, p->streamBytes)) return false;
        v->backgroundStreamStamp = b->streamStamp;
        v->backgroundStreamScene = scene->id;
    }
    if (v->backgroundModuleStamp != b->moduleStamp || v->backgroundModuleScene != scene->id) {
        v->backgroundModuleStamp = b->moduleStamp;
        v->backgroundModuleScene = scene->id;
        computeFree(&v->backgroundUser);
        if (b->spirv && p->spirvBytes) {
            uint32_t* code = artfMalloc(p->spirvBytes);
            memcpy(code, b->spirv, p->spirvBytes);
            if (!computeMake(v, &v->backgroundUser, code, p->spirvBytes)) {
                computeFree(&v->backgroundUser);
                artfLog("artful: the background pass's shader could not be made for Metal");
            }
        }
    }
    /* (no shader: no light) */
    if (!v->backgroundUser.state) memset(v->backgroundLight.mapped, 0, v->backgroundLight.size);
    if (!v->bloomFirst.state) {
        size_t size = 0;
        uint32_t* code = artfShaderBloomDown(&size, true);
        if (!computeMake(v, &v->bloomFirst, code, size)) return false;
        code = artfShaderBloomDown(&size, false);
        if (!computeMake(v, &v->bloomNext, code, size)) return false;
        code = artfShaderBloomUp(&size);
        if (!computeMake(v, &v->bloomUp, code, size)) return false;
    }
    if (!v->backdrop.state) {
        const int lightFormat = v->floats ? PIXEL_RGBA16F : 0, surface = !v->floats ? 0 : v->wide ? PIXEL_RGBA32U : PIXEL_R32U;
        const RenderSpec over = {lightFormat, surface, false, 0, true, true, 0};
        size_t size = 0;
        uint32_t* code = artfShaderBackgroundBackdrop(&size, v->floats);
        if (!fullMake(v, &v->backdrop, code, size, v->depthOff, &over)) return false;
    }
    return true;
}

/* Its light (with a shader), then its bloom: before the scene's pass. */
static void backgroundRecord(Metal* v, id commands, const ArtfBackgroundBlock* h) {
    id e = GET(id, commands, "computeCommandEncoder");
    computeUse(v, e);
    if (v->backgroundUser.state) {
        const MTLSize groups = {((NSUInteger)v->sceneWidth + 7) / 8, ((NSUInteger)v->sceneHeight + 7) / 8, 1};
        computeStep(v, e, &v->backgroundUser, NULL, 0, groups);
    }
    const uint32_t levels = h->size[2];
    for (uint32_t i = 0; i < levels; ++i) {
        const uint32_t words[4] = {i, 0, 0, 0};
        const MTLSize groups = {(h->bloom[i][1] * h->bloom[i][2] + 63) / 64, 1, 1};
        computeStep(v, e, i ? &v->bloomNext : &v->bloomFirst, words, sizeof words, groups);
    }
    for (uint32_t i = levels ? levels - 1 : 0; i-- > 0;) {
        const uint32_t words[4] = {i, 0, 0, 0};
        const MTLSize groups = {(h->bloom[i][1] * h->bloom[i][2] + 63) / 64, 1, 1};
        computeStep(v, e, &v->bloomUp, words, sizeof words, groups);
    }
    DO(e, "endEncoding");
}

static bool frameDraw(Metal* v, ArtfScene* scene) {
    waitLast(v);
    /* (the surface's other words: where motion is followed or light goes under the surface; with samples too, as
     * Vulkan's, an RGBA32Uint of four) */
    const bool wants = v->floats && (scene->post.screen.temporal || scene->post.lens.motionBlur > 0 || artfSceneHasUnder(scene));
    if (wants && scene->deforming.len && !v->warnedWide) {
        v->warnedWide = true;
        artfLog("artful: Metal follows the motion of instances, not of what bones bend");
    }
    const bool wide = wants;
    if (wide != v->wide) {
        v->wide = wide;
        v->targetsStale = true;
    }
    if (v->floats && v->base.renderScale != v->scale) {
        v->scale = v->base.renderScale;
        v->targetsStale = true;
    }
    if (v->targetsStale && !targetsMake(v)) return false;
    if (!v->frameTexture) return true; /* a window with no size */
#define ONCE(flag, ...) do { if (!v->flag) { v->flag = true; artfLog(__VA_ARGS__); } } while (0)
    const ArtfView view = v->base.view;
    const bool tracing = scene->trace.desc.mode != ARTF_TRACE_NONE && !view; /* (a view is of triangles) */
    if (tracing && scene->trace.desc.hardware) ONCE(warnedTrace, "artful: Metal traces through the scene's trees: not through its ray tracing hardware");
#undef ONCE
    v->froxels = v->floats && scene->post.screen.volumetric > 0 && scene->fogDensity > 0;
    v->variantBits = (scene->layeredMaterials > 0 ? (uint32_t)ARTF_SHADER_COAT : 0u) | (scene->shadows.soft && scene->shadows.resolution > 0 ? (uint32_t)ARTF_SHADER_SOFT : 0u) |
                     (artfSceneShapedLights(scene) ? (uint32_t)ARTF_SHADER_SHAPES : 0u);
    if (scene->shadows.traced && !tracing) v->variantBits |= (uint32_t)ARTF_SHADER_TRACED;
    if (scene->indirect.on && scene->indirect.probes) v->variantBits |= (uint32_t)ARTF_SHADER_PROBES | (!scene->indirect.desc.throughWalls ? (uint32_t)ARTF_SHADER_WALLS : 0u);
    /* (a view: what it shows, in the pixel shaders; the meshlets view, without the triangles' numbers, shows instances) */
    if (view) v->variantBits |= (uint32_t)ARTF_SHADER_VIEW;
    if (!syncEnvironment(v, scene) || !syncScene(v, scene) || !graphsSync(v, scene)) return false;
    if (v->floats && !v->posts[0].state) return false;
    ArtfFrameData frame;
    artfSceneFrame(scene, v->sceneWidth, v->sceneHeight, &frame);
    artfFrameForView(&frame, (int)view);
    frame.output[0] = (float)v->width;
    frame.output[1] = (float)v->height;
    if (tracing || !v->floats) frame.traced &= ~(uint32_t)ARTF_TRACED_REFLECTIONS;
    if (tracing) frame.traced = frame.tracedCasts = 0;
    if ((frame.traced || frame.tracedCasts) && !treesSync(v, scene)) return false;
    if (tracing) frame.post[1][1] = 0;
    bool clouds = false, under = false;
    uint32_t cloudColumns = 0, cloudRows = 0;
    if (!v->floats) {
        memset(frame.post, 0, sizeof frame.post);
        if (view == ARTF_VIEW_MOTION) {
            /* (the motion view: the frame before, as in floats) */
            if (v->hasPrevious) memcpy(frame.previous, v->previous, sizeof v->previous);
            memcpy(v->previous, frame.viewProj, sizeof v->previous);
            v->hasPrevious = true;
        }
    } else {
        if (!v->wide) {
            frame.post[1][3] = 0;
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
        if (clouds && !cloudsSync(v, scene, &cloudColumns, &cloudRows)) return false;
        frame.output[3] = (float)v->postFrames;
        float now[16];
        memcpy(now, frame.viewProj, sizeof now);
        if (v->hasPrevious) memcpy(frame.previous, v->previous, sizeof v->previous);
        memcpy(v->previous, now, sizeof now);
        v->hasPrevious = true;
        if (frame.post[1][3] > 0) artfFrameJitter(&frame, v->postFrames++, v->sceneWidth, v->sceneHeight);
        frame.grade2[2][3] = (float)(v->grainFrames++ & 0xFFFFFu);
    }
    if (frame.post[1][3] > 0 || frame.motionBlur > 0 || view == ARTF_VIEW_MOTION) {
        /* how each instance moved: four rows each, after the instances */
        artfMotionStep(&v->motion, scene, false);
        frame.moving = v->motion.moving ? 1.f : 0.f;
        float* rows = (float*)((char*)v->instances.mapped + scene->instances.len * sizeof(ArtfInstanceData));
        for (size_t i = 0; i < v->motion.count; ++i) {
            memcpy(rows + i * 16, v->motion.motion + i * 12, 48);
            rows[i * 16 + 12] = rows[i * 16 + 13] = rows[i * 16 + 14] = rows[i * 16 + 15] = 0;
        }
    }
    frame.indirect[1][3] = (float)artfSceneDecals(scene, &frame, v->sceneHeight, v->decals.mapped);
    if (!syncLights(v, scene, &frame)) return false;
    artfSceneResidencyStep(scene, &frame, v->sceneHeight);
    uint32_t layerCount = 0;
    if (scene->translucentCount && scene->instances.len && !tracing) {
        artfVecReserve(&v->layers, scene->translucentCount);
        layerCount = artfSceneLayers(scene, &frame, v->layers.items);
        artfLayerRange(&frame, v->layers.items, layerCount);
        uint32_t* out = (uint32_t*)v->visible.mapped + (size_t)layersAt(v) * 2;
        for (uint32_t i = 0; i < layerCount; ++i) {
            out[i * 2] = v->layers.items[i].instance;
            out[i * 2 + 1] = v->layers.items[i].mesh;
        }
    }
    memcpy(v->frame.mapped, &frame, sizeof frame);
    for (uint32_t k = 0; k < frame.lightCount; ++k)
        if (frame.lightShapes[k][2][3] > 0)
            memcpy((char*)v->extraLights.mapped + ARTF_MAX_LIGHTS * sizeof(ArtfLightData) + (size_t)k * ARTF_PROFILE_FLOATS * sizeof(float), scene->lightProfile[(int)frame.lightShapes[k][2][3] - 1],
                   ARTF_PROFILE_FLOATS * sizeof(float));
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
            ++v->stamp;
        }
        if (!syncShadowList(v, scene)) return false;
    }

    const uint32_t meshCount = (uint32_t)v->meshes.len, instanceCount = tracing ? 0 : (uint32_t)scene->instances.len;
    bool probeStep = false;
    if (!tracing && frame.indirect[0][3] > 0 && !probesSync(v, scene, &probeStep)) return false;
    if (frame.traced & ARTF_TRACED_REFLECTIONS && (!v->reflectionTraced.state || v->reflectionGraphs != scene->graphs.len || (scene->graphs.len && v->reflectionScene != scene->id))) {
        computeFree(&v->reflectionTraced);
        size_t size = 0;
        const uint32_t textures = v->maxTextures | (v->uniformTextures ? ARTF_TEXTURES_UNIFORM : 0u);
        uint32_t* code = artfShaderPostReflectionTraced(&size, textures, scene->graphs.items, (uint32_t)scene->graphs.len, false);
        if (!computeMake(v, &v->reflectionTraced, code, size)) artfLog("artful: Metal's reflections from rays could not be made: from the screen");
        v->reflectionGraphs = (uint32_t)scene->graphs.len;
        v->reflectionScene = scene->id;
    }
    bool traceClear = false, traceShown = false;
    uint32_t traceAdding = 0;
    if (tracing) {
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
        ArtfTraceFrame* traced = v->traceFrameUbo.mapped;
        artfSceneTraceFrame(scene, &frame, v->sceneWidth, v->sceneHeight, d->denoise ? v->traceFrames : v->traceDone, traced);
        traced->pad[0] = d->denoise ? 1 : v->traceDone + traceAdding;
        traced->pad[1] = (v->denoiseFirst ? 1u : 0u) | (d->denoise && !v->denoiseFirst && !memcmp(v->tracePrevious, frame.viewProj, sizeof v->tracePrevious) ? 2u : 0u);
        memcpy(traced->previous, v->tracePrevious, sizeof traced->previous);
        v->traceDone += traceAdding;
        if (d->denoise && traceAdding) {
            v->traceFrames += traceAdding;
            v->denoiseFirst = false;
            memcpy(v->tracePrevious, frame.viewProj, sizeof v->tracePrevious);
        }
        traceShown = v->traceDone || d->denoise;
    }
    const bool cutouts = scene->cutoutCount != 0 || scene->features[ARTF_FEATURE_LOD_FADE];
    if (instanceCount) memcpy(v->indirect.mapped, v->template_.mapped, (size_t)meshCount * 80);
    const bool overdraw = view == ARTF_VIEW_OVERDRAW;
    if (overdraw && !overdrawMake(v)) return false;
    /* a background pass of the program's: not under a view, which is black behind */
    const bool background = scene->backgroundPass.on && !view;
    ArtfBackgroundBlock backgroundFrame;
    if (background) {
        uint32_t bloomPixels = 0;
        artfBackgroundFrame(scene, v->sceneWidth, v->sceneHeight, &backgroundFrame, &bloomPixels);
        if (!backgroundSync(v, scene, &backgroundFrame, bloomPixels)) return false;
    }

    resourcesGather(v);
    id commands = GET(id, v->queue, "commandBuffer");
    DO(commands, "retain");
    v->last = commands;
    if (tracing) traceRecord(v, commands, traceClear, traceAdding != 0, scene->trace.desc.denoise);
    if (probeStep) probesRecord(v, commands, scene);
    if (instanceCount) {
        id e = GET(id, commands, "computeCommandEncoder");
        computeUse(v, e);
        SET_ID(e, "setComputePipelineState:", v->cull);
        SEND(void, e, "setBuffer:offset:atIndex:", (id, SEL, id, NSUInteger, NSUInteger), arguments(v, &v->cullShader), (NSUInteger)0, (NSUInteger)0);
        const MTLSize groups = {(instanceCount + 63) / 64, 1, 1}, size = {64, 1, 1};
        SEND(void, e, "dispatchThreadgroups:threadsPerThreadgroup:", (id, SEL, MTLSize, MTLSize), groups, size);
        DO(e, "endEncoding");
    }
    if (shadowViews && instanceCount) drawShadows(v, commands, &shadows, shadowViews);
    if (background) backgroundRecord(v, commands, &backgroundFrame);
    const bool fogGrid = v->froxels && frame.post[1][1] > 0 && frame.look[1] > 0;
    if (fogGrid) {
        id g = GET(id, commands, "computeCommandEncoder");
        computeUse(v, g);
        postStep(v, g, POST_FROXELS, 0, (uint32_t)((v->sceneWidth + ARTF_FROXEL_TILE - 1) / ARTF_FROXEL_TILE) * (uint32_t)((v->sceneHeight + ARTF_FROXEL_TILE - 1) / ARTF_FROXEL_TILE));
        DO(g, "endEncoding");
    }

    id e = scenePass(v, commands, &frame, true);
    if (background) {
        pipelineUse(v, e, &v->backdrop);
        drawTriangle(e);
    } else if (frame.sky[3] > 0 && frame.ground[3] > 0 && v->environmentBehind.state) {
        pipelineUse(v, e, &v->environmentBehind);
        drawTriangle(e);
    }
    if (fogGrid && v->fogBehind.state) {
        pipelineUse(v, e, &v->fogBehind);
        drawTriangle(e);
    }
    if (traceShown && v->traceShow.state) {
        pipelineUse(v, e, &v->traceShow);
        drawTriangle(e);
    }
    if (instanceCount) {
        verticesUse(v, e);
        pipelineUse(v, e, overdraw ? &v->overdraw[0] : &v->scene);
        drawCommands(v, e, v->indirect.buffer, 0, meshCount);
        if (cutouts) {
            pipelineUse(v, e, overdraw ? &v->overdraw[1] : &v->cutout);
            drawCommands(v, e, v->indirect.buffer, meshCount * 2, meshCount);
        }
        if (v->floats) {
            /* (what comes after reads the depth of what is solid: taken before the layers write theirs) */
            DO(e, "endEncoding");
            gatherDepth(v, commands);
            e = scenePass(v, commands, &frame, false);
            verticesUse(v, e);
        }
        SEL drawOne = selName("drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:");
        for (uint32_t i = 0; i < layerCount; ++i) {
            const MeshRange* m = &v->meshes.items[v->layers.items[i].mesh];
            if (!m->indexCount) continue;
            uint32_t end = i + 1;
            const bool particle = (scene->instances.items[v->layers.items[i].instance].flags & ARTF_INSTANCE_PARTICLE) != 0;
            if (particle)
                while (end < layerCount && v->layers.items[end].mesh == v->layers.items[i].mesh && (scene->instances.items[v->layers.items[end].instance].flags & ARTF_INSTANCE_PARTICLE)) ++end;
            Pipeline* const passes[2] = {overdraw ? &v->overdraw[1] : particle ? &v->particle : &v->layerDepth, particle || overdraw ? NULL : &v->layerDraw};
            for (int k = 0; k < 2 && passes[k]; ++k) {
                pipelineUse(v, e, passes[k]);
                ((void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, id, NSUInteger, NSUInteger, long, NSUInteger))objcSend)(
                    e, drawOne, PRIMITIVE_TRIANGLE, m->indexCount, INDEX_UINT32, v->indices.buffer, (NSUInteger)m->firstIndex * 4, end - i, (long)m->vertexOffset, layersAt(v) + i);
            }
            i = end - 1;
        }
    }
    if (v->overlayPixels && v->overlayUsable && !v->floats) {
        pipelineUse(v, e, &v->overlayDraw);
        drawTriangle(e);
    }
    DO(e, "endEncoding");
    if (v->floats) {
        if (!instanceCount) gatherDepth(v, commands);
        postChain(v, commands, &frame, under, clouds, cloudColumns, cloudRows);
        finalPass(v, commands);
    }

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
    artfSceneScaleStep(scene, v->width, v->height);
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
    bool ok = bufferNew(v, &readback, bytes);
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
}

static void shaderFree(Shader* s) {
    release(s->arguments);
    release(s->encoder);
    release(s->roleArguments);
    release(s->encoder1);
    release(s->function);
    release(s->library);
    memset(s, 0, sizeof *s);
}

static void pipelineFree(Pipeline* p) {
    release(p->state);
    shaderFree(&p->vertex);
    shaderFree(&p->fragment);
    memset(p, 0, sizeof *p);
}

static void metalFree(ArtfRenderer* r) {
    Metal* v = (Metal*)r;
    waitLast(v);
    targetsFree(v);
    Buffer* buffers[] = {&v->empty, &v->frame, &v->instances, &v->visible, &v->indirect, &v->template_, &v->materials, &v->meshTable, &v->vertices, &v->indices, &v->colors,
                         &v->shadowData, &v->shadowList, &v->shadowCommands, &v->meshletMeshes, &v->meshletState, &v->extraLights, &v->tileLights, &v->decals};
    for (size_t i = 0; i < ARTF_COUNT(buffers); ++i) bufferFree(buffers[i]);
    for (size_t i = 0; i < v->textures.len; ++i) release(v->textures.items[i]);
    Pipeline* pipelines[] = {&v->scene, &v->cutout, &v->overlayDraw, &v->shadow, &v->shadowHoles, &v->layerDepth, &v->layerDraw, &v->particle, &v->environmentBehind,
                             &v->fogBehind, &v->traceShow, &v->final, &v->overlayFloats, &v->overdraw[0], &v->overdraw[1], &v->backdrop};
    for (size_t i = 0; i < ARTF_COUNT(pipelines); ++i) pipelineFree(pipelines[i]);
    for (int k = 0; k < 22; ++k) computeFree(&v->posts[k]);
    for (int k = 0; k < 9; ++k) computeFree(&v->denoisePasses[k]);
    computeFree(&v->trace);
    computeFree(&v->probeStep);
    computeFree(&v->reflectionTraced);
    computeFree(&v->backgroundUser);
    computeFree(&v->bloomFirst);
    computeFree(&v->bloomNext);
    computeFree(&v->bloomUp);
    Buffer* more[] = {&v->backgroundBlock, &v->backgroundLight, &v->backgroundBloom, &v->backgroundData, &v->backgroundStream, &v->postLocks, &v->postGathered, &v->cloudField, &v->cloudGrid, &v->environment, &v->traceNodes, &v->traceTriangles, &v->traceVertices, &v->traceTop,
                      &v->traceInstances, &v->traceSum, &v->traceFrameUbo, &v->denoiseShift, &v->traceMotion, &v->probesNow, &v->probesNext};
    for (size_t i = 0; i < ARTF_COUNT(more); ++i) bufferFree(more[i]);
    for (int k = 0; k < 8; ++k) bufferFree(&v->post[k]);
    for (int k = 0; k < 7; ++k) bufferFree(&v->denoiseBuffers[k]);
    artfMotionFree(&v->motion);
    artfVecFree(&v->layers);
    shaderFree(&v->cullShader);
    const id others[] = {v->cull, v->depthOn, v->depthOff, v->depthNear, v->depthEqual, v->sampler, v->nearest, v->vertexDescriptor, v->blank, v->shadowAtlas, v->queue, v->device};
    for (size_t i = 0; i < ARTF_COUNT(others); ++i) release(others[i]);
    artfFree(v->lightScratch);
    artfFree(v->listScratch);
    artfVecFree(&v->resources);
    artfVecFree(&v->meshes);
    artfVecFree(&v->textures);
    artfVecFree(&v->textureMarks);
    artfVecFree(&v->copyStarts);
    artfVecFree(&v->overlayDamage);
    artfFree(v);
}

static const ArtfRendererVtbl metalVtbl = {metalDescription, metalResize, metalSetOverlay, metalDraw, metalReadPixels, metalPixels, metalStats, metalFree};

ArtfRenderer* artfMetalNew(const ArtfRendererOptions* o) {
    if (!runtimeOpen()) return NULL;
    id device = systemDevice();
    if (!device) return NULL;
    Metal* v = artfCalloc(1, sizeof *v);
    v->base.vt = &metalVtbl;
    v->base.backend = ARTF_BACKEND_METAL;
    v->device = device;
    v->queue = GET(id, device, "newCommandQueue");
    v->unified = GET(signed char, device, "hasUnifiedMemory");
    /* (argument buffers of tier 2 take textures by any index, and a thousand of them; tier 1, by constant ones) */
    v->uniformTextures = GET(NSUInteger, device, "argumentBuffersSupport") != ARGUMENT_TIER2 || getenv("ARTFUL_UNIFORM_TEXTURES");
    v->maxTextures = v->uniformTextures ? UNIFORM_TEXTURES : MAX_TEXTURES;
    v->width = o->width;
    v->height = o->height;
    v->samples = o->antialias ? 4 : 1;
    v->noVsync = o->noVsync;
    v->floats = o->hdr;
    v->scale = v->base.renderScale = o->hdr && o->renderScale > 0 && o->renderScale < 1.f ? artfMaxf(o->renderScale, .25f) : 1.f;
    v->base.renderScaleMost = v->scale;
    v->meshesUp = ~(size_t)0;
    snprintf(v->description, sizeof v->description, "%s (Metal%s)", GET(const char*, GET(id, device, "name"), "UTF8String"), v->uniformTextures ? ", textures by constant indices" : "");
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
    bool ok = bufferNew(v, &v->empty, BLANK_BYTES) && bufferNew(v, &v->frame, sizeof(ArtfFrameData)) && bufferNew(v, &v->shadowData, sizeof(ArtfShadowFrame)) &&
              bufferNew(v, &v->extraLights, (size_t)ARTF_MAX_LIGHTS * sizeof(ArtfLightData) + (size_t)ARTF_FRAME_LIGHTS * ARTF_PROFILE_FLOATS * sizeof(float)) && bufferNew(v, &v->tileLights, 64) &&
              bufferNew(v, &v->decals, (size_t)ARTF_MAX_DECALS * sizeof(ArtfDecalData)) && pipelinesMake(v) && targetsMake(v);
    poolPop(pool);
    if (!ok) {
        artfLog("artful: Metal could not be made ready");
        metalFree(&v->base);
        return NULL;
    }
    return &v->base;
}
