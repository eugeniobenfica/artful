/* artful — Metal's shading language from a shader's IR (ir.h).
 *
 * As tools/mkmetal.py's shaders were laid out by SPIRV-Cross, so that metal.c binds both alike: what descriptor set 0
 * binds goes into one argument buffer, spvDescriptorSetBuffer0, a member bN for binding N in their order (a texture's
 * sampler next to it, bNSmplr), at [[buffer(0)]]; push constants at [[buffer(1)]]; the entry point main0, what comes in
 * by location in main0_in ([[stage_in]]), what goes out in main0_out. Control is the IR's, structured: an if from its
 * merge, a loop from its merge and continue; a phi is a variable each edge into its block sets. Every value is a
 * variable of the function, declared at its top, but pointers, textures and constants, which are written where they
 * are used. */
#ifndef ARTFUL_SHADER_MSL_H
#define ARTFUL_SHADER_MSL_H

#include "shaderir/ir.h"

typedef struct IrMslOptions {
    bool flipY; /* a vertex shader's position upside down: Vulkan draws its maps with y the other way from a frame */
} IrMslOptions;

/* The shader as Metal source (artfFree), or NULL and why (what it cannot write: ray queries, say). */
char* irMsl(const IrModule* m, const IrMslOptions* options, char* why, size_t size);

#endif
