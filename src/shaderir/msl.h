/* artful — Metal's shading language from a shader's IR (ir.h).
 *
 * Laid out as SPIRV-Cross lays out what it writes, as metal.c's shaders once were: what descriptor set 0
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

/* What a shader's argument buffers hold, for a backend to fill them (metal.c): each resource's set, Vulkan binding, its
 * id in the set's buffer, what it is, and how many ids it takes (an array's elements); and the push constants' buffer
 * index, where it reads them. */
enum { IR_MSL_BUFFER, IR_MSL_TEXTURE, IR_MSL_SAMPLER };
typedef struct IrMslArgument { uint32_t set, binding, id, kind, count; } IrMslArgument;
#define IR_MSL_ARGUMENTS 128
typedef struct IrMslArguments {
    IrMslArgument items[IR_MSL_ARGUMENTS];
    int count;
    int pushAt; /* -1: none */
} IrMslArguments;

/* The shader as Metal source (artfFree), or NULL and why (what it cannot write: ray queries, say); and, where
 * arguments is given, what its argument buffers hold. */
char* irMsl(const IrModule* m, const IrMslOptions* options, IrMslArguments* arguments, char* why, size_t size);

#endif
