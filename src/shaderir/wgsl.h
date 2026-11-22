/* artful — WebGPU's shading language (WGSL) from a shader's IR (ir.h).
 *
 * Laid out for webgpu.c, which binds it: what Vulkan binds at set S, binding N is @group(S) @binding(N); a texture
 * and its sampler, which Vulkan binds as one, are two, the sampler at N + IR_WGSL_SAMPLER_AT; an array of textures (WGSL
 * has none) is a texture for each element k read, at IR_WGSL_ARRAY_AT + k, and one sampler for all at the binding before, and must be read
 * by constant indices (built with ARTF_TEXTURES_UNIFORM); push constants are a uniform buffer at group 0, binding
 * IR_WGSL_PUSH_AT. The entry point is main. A buffer that atomics touch is an array of atomic<u32>, every access to it
 * atomic. Control is the IR's, structured, as Metal's is written (msl.h): an if from its merge, a loop from its merge
 * and continue, a phi a variable each edge into its block sets; every value a variable of the function. Derivatives
 * and samples are written where the IR has them, which WGSL's uniformity analysis would refuse: it is turned off. */
#ifndef ARTFUL_SHADER_WGSL_H
#define ARTFUL_SHADER_WGSL_H

#include "shaderir/ir.h"

#define IR_WGSL_SAMPLER_AT 128
#define IR_WGSL_ARRAY_AT 256
#define IR_WGSL_PUSH_AT 999 /* (the last binding WebGPU has) */

typedef struct IrWgslOptions {
    bool flipY; /* a vertex shader's position upside down (as Metal's: msl.h) */
    /* a pixel shader's four words at location 1 go out as three, at 1, 2 and 3, each in two halves (the low first; the
     * fourth word is unused): WebGPU has no 32-bit integer target with samples, and RG16Uint has (webgpu.c) */
    bool splitSurface;
} IrWgslOptions;

/* What the shader binds, for a backend to make its bind groups from: each a group and binding of WGSL's, what it is,
 * and the Vulkan binding it stands for (with its element, of an array of textures). */
enum { IR_WGSL_UNIFORM, IR_WGSL_STORAGE_READ, IR_WGSL_STORAGE, IR_WGSL_TEXTURE, IR_WGSL_TEXTURE_UINT, IR_WGSL_TEXTURE_MULTISAMPLED, IR_WGSL_STORAGE_TEXTURE, IR_WGSL_SAMPLER };
typedef struct IrWgslBinding { uint32_t group, binding, kind, source, element; } IrWgslBinding;
#define IR_WGSL_BINDINGS 128
typedef struct IrWgslBindings { IrWgslBinding items[IR_WGSL_BINDINGS]; int count; } IrWgslBindings;

/* The shader as WGSL source (artfFree), or NULL and why (what it cannot write: ray queries, an array of textures by
 * an index that varies); and, where bindings is given, what it binds. */
char* irWgsl(const IrModule* m, const IrWgslOptions* options, IrWgslBindings* bindings, char* why, size_t size);

#endif
