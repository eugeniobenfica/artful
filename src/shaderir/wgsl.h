/* artful — WebGPU's shading language (WGSL) from a shader's IR (ir.h).
 *
 * Laid out for webgpu.c, which binds it: what Vulkan binds at set S, binding N is @group(S) @binding(N); a texture
 * and its sampler, which Vulkan binds as one, are two, the sampler at N + IR_WGSL_SAMPLER; an array of textures (WGSL
 * has none) is one texture and one sampler for each element k, at IR_WGSL_ARRAY + 2k and the next, and must be read
 * by constant indices (built with ARTF_TEXTURES_UNIFORM); push constants are a uniform buffer at group 0, binding
 * IR_WGSL_PUSH. The entry point is main. A buffer that atomics touch is an array of atomic<u32>, every access to it
 * atomic. Control is the IR's, structured, as Metal's is written (msl.h): an if from its merge, a loop from its merge
 * and continue, a phi a variable each edge into its block sets; every value a variable of the function. Derivatives
 * and samples are written where the IR has them, which WGSL's uniformity analysis would refuse: it is turned off. */
#ifndef ARTFUL_SHADER_WGSL_H
#define ARTFUL_SHADER_WGSL_H

#include "shaderir/ir.h"

#define IR_WGSL_SAMPLER 128
#define IR_WGSL_ARRAY 256
#define IR_WGSL_PUSH 1000

typedef struct IrWgslOptions {
    bool flipY; /* a vertex shader's position upside down (as Metal's: msl.h) */
} IrWgslOptions;

/* The shader as WGSL source (artfFree), or NULL and why (what it cannot write: ray queries, an array of textures by
 * an index that varies). */
char* irWgsl(const IrModule* m, const IrWgslOptions* options, char* why, size_t size);

#endif
