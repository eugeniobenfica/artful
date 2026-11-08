/* artful — writing SPIR-V. Vulkan takes shaders as SPIR-V, which is what a shader compiler makes; to
 * need none, the library builds its few shaders itself, as SSA, with these calls. */
#ifndef ARTFUL_SPIRV_H
#define ARTFUL_SPIRV_H

#include "artful/core.h"

typedef uint32_t SpvId;
typedef ARTF_VEC(uint32_t) SpvWords;

typedef struct Spv {
    SpvWords head;        /* capabilities, extensions, imports, the memory model, the entry point */
    SpvWords annotations;
    SpvWords types;       /* types, constants and global variables */
    SpvWords code;
    SpvId bound;
    SpvId glsl;           /* the GLSL.std.450 set */
    SpvId voidType, boolType, uintType, intType, floatType, vec2, vec3, vec4;
} Spv;

enum {
    SPV_OP_ExtInst = 12, SPV_OP_Load = 61, SPV_OP_Store = 62,
    SPV_OP_ConvertFToU = 109, SPV_OP_ConvertFToS = 110, SPV_OP_ConvertUToF = 112, SPV_OP_Bitcast = 124, SPV_OP_FNegate = 127, SPV_OP_IAdd = 128, SPV_OP_FAdd = 129, SPV_OP_ISub = 130, SPV_OP_FSub = 131,
    SPV_OP_IMul = 132, SPV_OP_FMul = 133, SPV_OP_UDiv = 134, SPV_OP_FDiv = 136, SPV_OP_UMod = 137, SPV_OP_VectorTimesScalar = 142, SPV_OP_Dot = 148,
    SPV_OP_LogicalAnd = 167, SPV_OP_LogicalNot = 168, SPV_OP_Select = 169, SPV_OP_INotEqual = 171, SPV_OP_ULessThan = 176, SPV_OP_FOrdLessThan = 184, SPV_OP_FOrdGreaterThan = 186, SPV_OP_FOrdLessThanEqual = 188,
    SPV_OP_FOrdGreaterThanEqual = 190, SPV_OP_ShiftRightLogical = 194, SPV_OP_ShiftLeftLogical = 196, SPV_OP_BitwiseXor = 198, SPV_OP_BitwiseAnd = 199
};

enum { SPV_STORAGE_UniformConstant = 0, SPV_STORAGE_Input = 1, SPV_STORAGE_Uniform = 2, SPV_STORAGE_Output = 3, SPV_STORAGE_Function = 7, SPV_STORAGE_PushConstant = 9, SPV_STORAGE_StorageBuffer = 12 };
enum { SPV_DECORATE_Block = 2, SPV_DECORATE_ArrayStride = 6, SPV_DECORATE_BuiltIn = 11, SPV_DECORATE_Flat = 14, SPV_DECORATE_Invariant = 18, SPV_DECORATE_NonWritable = 24, SPV_DECORATE_Location = 30,
       SPV_DECORATE_Binding = 33, SPV_DECORATE_DescriptorSet = 34, SPV_DECORATE_Offset = 35, SPV_DECORATE_NonUniform = 5300 };
enum { SPV_BUILTIN_Position = 0, SPV_BUILTIN_FragCoord = 15, SPV_BUILTIN_FrontFacing = 17, SPV_BUILTIN_GlobalInvocationId = 28, SPV_BUILTIN_VertexIndex = 42, SPV_BUILTIN_InstanceIndex = 43 };
enum { SPV_MODEL_Vertex = 0, SPV_MODEL_Fragment = 4, SPV_MODEL_GLCompute = 5 };
enum { SPV_GLSL_UnpackUnorm4x8 = 64, SPV_GLSL_FAbs = 4, SPV_GLSL_Floor = 8, SPV_GLSL_Sin = 13, SPV_GLSL_Cos = 14, SPV_GLSL_Atan2 = 25, SPV_GLSL_Sqrt = 31, SPV_GLSL_FMin = 37, SPV_GLSL_UMin = 38, SPV_GLSL_FMax = 40, SPV_GLSL_UMax = 41, SPV_GLSL_Length = 66, SPV_GLSL_Cross = 68, SPV_GLSL_Normalize = 69, SPV_GLSL_FindUMsb = 75 };

void spvInit(Spv* s);
void spvFini(Spv* s);
SpvId spvNewId(Spv* s);
/* An instruction with n operands; result 0 and type 0 are left out. */
void spvEmit(SpvWords* to, uint32_t op, const uint32_t* operands, uint32_t n);
#define SPV_WORDS(...) (const uint32_t[]){__VA_ARGS__}, (uint32_t)(sizeof((const uint32_t[]){__VA_ARGS__}) / 4)

void spvCapability(Spv* s, uint32_t capability);
void spvExtension(Spv* s, const char* name);
/* Written last: all the shader's Input and Output variables go here. */
void spvEntryPoint(Spv* s, uint32_t model, SpvId function, const SpvId* interface, uint32_t count);
void spvExecutionMode(Spv* s, SpvId function, uint32_t mode, uint32_t a, uint32_t b, uint32_t c, uint32_t n);
void spvDecorate(Spv* s, SpvId id, uint32_t decoration, int value); /* value < 0: none */
void spvMemberDecorate(Spv* s, SpvId id, uint32_t member, uint32_t decoration, int value);

SpvId spvTypePointer(Spv* s, uint32_t storage, SpvId type);
SpvId spvTypeArray(Spv* s, SpvId element, uint32_t count, uint32_t stride); /* count 0: runtime; stride 0: none */
SpvId spvTypeStruct(Spv* s, const SpvId* members, uint32_t count);
SpvId spvTypeFunction(Spv* s, SpvId result);
SpvId spvTypeSampledImage2D(Spv* s, SpvId* imageType);
SpvId spvTypeSampledImage2DMS(Spv* s, SpvId* imageType); /* of several samples a pixel */
SpvId spvConstFloat(Spv* s, float v);
SpvId spvConstUint(Spv* s, uint32_t v);
SpvId spvConstInt(Spv* s, int32_t v);
SpvId spvConstBool(Spv* s, bool v);
SpvId spvVariable(Spv* s, SpvId pointerType, uint32_t storage);
/* A variable for a descriptor. */
SpvId spvResource(Spv* s, SpvId type, uint32_t storage, uint32_t binding);

/* The body of main: a function with no arguments. */
SpvId spvBeginMain(Spv* s);
void spvEndMain(Spv* s);
SpvId spvLabel(Spv* s, SpvId label); /* 0: a new one */

/* Code. */
SpvId spvOp2(Spv* s, uint32_t op, SpvId type, SpvId a, SpvId b);
SpvId spvOp1(Spv* s, uint32_t op, SpvId type, SpvId a);
SpvId spvSelect(Spv* s, SpvId type, SpvId condition, SpvId a, SpvId b);
SpvId spvGlsl1(Spv* s, uint32_t inst, SpvId type, SpvId a);
SpvId spvGlsl2(Spv* s, uint32_t inst, SpvId type, SpvId a, SpvId b);
SpvId spvCompose(Spv* s, SpvId type, const SpvId* parts, uint32_t count);
SpvId spvExtract(Spv* s, SpvId type, SpvId v, uint32_t index);
SpvId spvShuffle3(Spv* s, SpvId v); /* the xyz of a vec4 */
SpvId spvAccess(Spv* s, SpvId pointerType, SpvId base, const SpvId* indices, uint32_t count);
SpvId spvLoad(Spv* s, SpvId type, SpvId pointer);
void spvStore(Spv* s, SpvId pointer, SpvId value);
SpvId spvAtomicAdd(Spv* s, SpvId pointer, SpvId value);
SpvId spvImageSample(Spv* s, SpvId sampledImage, SpvId uv);
SpvId spvImageSampleLod(Spv* s, SpvId sampledImage, SpvId uv, SpvId lod); /* outside fragment shaders */
SpvId spvImageFetchAs(Spv* s, SpvId resultType, SpvId imageType, SpvId sampledImage, SpvId coords); /* level 0, of any type */
SpvId spvTypeSampledImage2DOf(Spv* s, SpvId sampledType, SpvId* imageType); /* of uints, say */
SpvId spvTypeVector(Spv* s, SpvId element, uint32_t count);
SpvId spvImageFetch(Spv* s, SpvId imageType, SpvId sampledImage, SpvId coords);
SpvId spvImageFetchSample(Spv* s, SpvId imageType, SpvId sampledImage, SpvId coords, SpvId sample);
void spvBranch(Spv* s, SpvId label);
void spvIfBegin(Spv* s, SpvId condition, SpvId* merge); /* an if with no else; ends at spvIfEnd */
void spvIfEnd(Spv* s, SpvId merge);
void spvKillIf(Spv* s, SpvId condition); /* a fragment shader leaves this pixel */
void spvLoopMerge(Spv* s, SpvId merge, SpvId next);
void spvBranchIf(Spv* s, SpvId condition, SpvId yes, SpvId no);

/* The module's words, the caller's (artfFree). */
uint32_t* spvFinish(Spv* s, size_t* wordCount);

#endif
