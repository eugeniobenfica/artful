/* artful — writing SPIR-V. What is written goes into an IR (shaderir/ir.h), its four sections apart; irSpirv joins them. */
#include "vulkan/spirv.h"
#include "core/color.h"

enum {
    OP_Extension = 10, OP_ExtInstImport = 11, OP_MemoryModel = 14, OP_EntryPoint = 15, OP_ExecutionMode = 16, OP_Capability = 17,
    OP_TypeVoid = 19, OP_TypeBool = 20, OP_TypeInt = 21, OP_TypeFloat = 22, OP_TypeVector = 23, OP_TypeImage = 25, OP_TypeSampledImage = 27,
    OP_TypeArray = 28, OP_TypeRuntimeArray = 29, OP_TypeStruct = 30, OP_TypePointer = 32, OP_TypeFunction = 33, OP_Constant = 43,
    OP_Function = 54, OP_FunctionEnd = 56, OP_Variable = 59, OP_AccessChain = 65, OP_Decorate = 71, OP_MemberDecorate = 72,
    OP_VectorShuffle = 79, OP_CompositeConstruct = 80, OP_CompositeExtract = 81, OP_ImageSampleImplicitLod = 87, OP_ImageFetch = 95, OP_Image = 100,
    OP_AtomicIAdd = 234, OP_LoopMerge = 246, OP_SelectionMerge = 247, OP_Label = 248, OP_Branch = 249, OP_BranchConditional = 250, OP_Return = 253
};

void spvEmit(SpvWords* to, uint32_t op, const uint32_t* operands, uint32_t n) { irAppend(to->m, to->section, op, operands, n); }

SpvId spvNewId(Spv* s) { return irNewId(&s->ir); }

/* A string's words, NUL included. */
static uint32_t packString(const char* text, uint32_t* out) {
    const size_t len = strlen(text) + 1;
    const uint32_t words = (uint32_t)((len + 3) / 4);
    memset(out, 0, (size_t)words * 4);
    memcpy(out, text, len);
    return words;
}

/* A type made of operands, once each. (The list is small, and read when a shader is built.) */
static SpvId typeOf(Spv* s, uint32_t op, const uint32_t* operands, uint32_t n);
SpvId spvTypeOpaque(Spv* s, uint32_t op) { return typeOf(s, op, NULL, 0); }

static SpvId typeOf(Spv* s, uint32_t op, const uint32_t* operands, uint32_t n) {
    for (size_t k = 0; k < s->ir.insts.len; ++k) {
        const IrInst* i = &s->ir.insts.items[k];
        if (i->section == IR_TYPES && i->op == op && i->count == n && (!n || !memcmp(irOperands(&s->ir, i), operands, (size_t)n * 4))) return i->result;
    }
    uint32_t words[16];
    words[0] = spvNewId(s);
    if (n) memcpy(words + 1, operands, (size_t)n * 4);
    spvEmit(&s->types, op, words, n + 1);
    return words[0];
}

bool artfShaderDebugInfo = false;

void spvInitAt(Spv* s, const char* file, int line, const char* function) {
    memset(s, 0, sizeof *s);
    s->file = file;
    s->line = line;
    s->function = function;
    irInit(&s->ir);
    s->head = (SpvWords){&s->ir, IR_HEAD};
    s->annotations = (SpvWords){&s->ir, IR_ANNOTATIONS};
    s->types = (SpvWords){&s->ir, IR_TYPES};
    s->code = (SpvWords){&s->ir, IR_CODE};
    spvCapability(s, 1); /* Shader */
    s->glsl = spvNewId(s); /* (the import itself comes after the extensions: spvEntryPoint) */
    s->voidType = typeOf(s, OP_TypeVoid, NULL, 0);
    s->boolType = typeOf(s, OP_TypeBool, NULL, 0);
    s->uintType = typeOf(s, OP_TypeInt, SPV_WORDS(32, 0));
    s->intType = typeOf(s, OP_TypeInt, SPV_WORDS(32, 1));
    s->floatType = typeOf(s, OP_TypeFloat, SPV_WORDS(32));
    s->vec2 = typeOf(s, OP_TypeVector, SPV_WORDS(s->floatType, 2));
    s->vec3 = typeOf(s, OP_TypeVector, SPV_WORDS(s->floatType, 3));
    s->vec4 = typeOf(s, OP_TypeVector, SPV_WORDS(s->floatType, 4));
}

void spvFini(Spv* s) { irFree(&s->ir); }

void spvCapability(Spv* s, uint32_t capability) { spvEmit(&s->head, OP_Capability, SPV_WORDS(capability)); }

void spvExtension(Spv* s, const char* name) {
    uint32_t words[16];
    uint32_t n = packString(name, words);
    spvEmit(&s->head, OP_Extension, words, n);
}

void spvEntryPoint(Spv* s, uint32_t model, SpvId function, const SpvId* interface, uint32_t count) {
    uint32_t words[32];
    uint32_t n = 0;
    /* the import and the memory model go first: nothing may come between them and the capabilities */
    words[0] = s->glsl;
    n = packString("GLSL.std.450", words + 1);
    spvEmit(&s->head, OP_ExtInstImport, words, n + 1);
    spvEmit(&s->head, OP_MemoryModel, SPV_WORDS(0, 1)); /* Logical, GLSL450 */
    words[0] = model;
    words[1] = function;
    n = 2 + packString("main", words + 2);
    for (uint32_t i = 0; i < count; ++i) words[n++] = interface[i];
    spvEmit(&s->head, OP_EntryPoint, words, n);
}

void spvExecutionMode(Spv* s, SpvId function, uint32_t mode, uint32_t a, uint32_t b, uint32_t c, uint32_t n) {
    const uint32_t words[5] = {function, mode, a, b, c};
    spvEmit(&s->head, OP_ExecutionMode, words, 2 + n);
}

void spvDecorate(Spv* s, SpvId id, uint32_t decoration, int value) {
    const uint32_t words[3] = {id, decoration, (uint32_t)value};
    spvEmit(&s->annotations, OP_Decorate, words, value < 0 ? 2 : 3);
}

void spvMemberDecorate(Spv* s, SpvId id, uint32_t member, uint32_t decoration, int value) {
    const uint32_t words[4] = {id, member, decoration, (uint32_t)value};
    spvEmit(&s->annotations, OP_MemberDecorate, words, value < 0 ? 3 : 4);
}

SpvId spvTypePointer(Spv* s, uint32_t storage, SpvId type) { return typeOf(s, OP_TypePointer, SPV_WORDS(storage, type)); }

SpvId spvTypeArray(Spv* s, SpvId element, uint32_t count, uint32_t stride) {
    SpvId id;
    if (count) id = typeOf(s, OP_TypeArray, SPV_WORDS(element, spvConstUint(s, count)));
    else id = typeOf(s, OP_TypeRuntimeArray, SPV_WORDS(element));
    if (stride) spvDecorate(s, id, SPV_DECORATE_ArrayStride, (int)stride);
    return id;
}

SpvId spvTypeStruct(Spv* s, const SpvId* members, uint32_t count) {
    /* (never shared: two of the same may be decorated apart) */
    uint32_t words[9];
    words[0] = spvNewId(s);
    if (count) memcpy(words + 1, members, (size_t)count * 4);
    spvEmit(&s->types, OP_TypeStruct, words, count + 1);
    return words[0];
}

SpvId spvTypeFunction(Spv* s, SpvId result) { return typeOf(s, OP_TypeFunction, SPV_WORDS(result)); }

SpvId spvTypeSampledImage2D(Spv* s, SpvId* imageType) {
    /* float, 2D, not depth, not arrayed, single sample, sampled, no format */
    *imageType = typeOf(s, OP_TypeImage, SPV_WORDS(s->floatType, 1, 0, 0, 0, 1, 0));
    return typeOf(s, OP_TypeSampledImage, SPV_WORDS(*imageType));
}

SpvId spvTypeSampledImage2DMS(Spv* s, SpvId* imageType) {
    *imageType = typeOf(s, OP_TypeImage, SPV_WORDS(s->floatType, 1, 0, 0, 1, 1, 0));
    return typeOf(s, OP_TypeSampledImage, SPV_WORDS(*imageType));
}

/* (OpConstant has its type before its id, unlike a type: so it has its own search) */
static SpvId constantOf(Spv* s, SpvId type, uint32_t value) {
    for (size_t k = 0; k < s->ir.insts.len; ++k) {
        const IrInst* i = &s->ir.insts.items[k];
        if (i->section == IR_TYPES && i->op == OP_Constant && i->type == type && i->count == 1 && irOperands(&s->ir, i)[0] == value) return i->result;
    }
    SpvId id = spvNewId(s);
    spvEmit(&s->types, OP_Constant, SPV_WORDS(type, id, value));
    return id;
}

SpvId spvConstFloat(Spv* s, float v) {
    uint32_t bits;
    memcpy(&bits, &v, 4);
    return constantOf(s, s->floatType, bits);
}
SpvId spvConstUint(Spv* s, uint32_t v) { return constantOf(s, s->uintType, v); }
SpvId spvConstInt(Spv* s, int32_t v) { return constantOf(s, s->intType, (uint32_t)v); }

SpvId spvConstBool(Spv* s, bool v) {
    SpvId id = spvNewId(s);
    spvEmit(&s->types, v ? 41 /* OpConstantTrue */ : 42 /* OpConstantFalse */, SPV_WORDS(s->boolType, id));
    return id;
}

SpvId spvVariable(Spv* s, SpvId pointerType, uint32_t storage) {
    SpvId id = spvNewId(s);
    SpvWords* to = storage == SPV_STORAGE_Function ? &s->code : &s->types;
    spvEmit(to, OP_Variable, SPV_WORDS(pointerType, id, storage));
    return id;
}

SpvId spvResource(Spv* s, SpvId type, uint32_t storage, uint32_t binding) {
    SpvId id = spvVariable(s, spvTypePointer(s, storage, type), storage);
    spvDecorate(s, id, SPV_DECORATE_DescriptorSet, 0);
    spvDecorate(s, id, SPV_DECORATE_Binding, (int)binding);
    return id;
}

SpvId spvBeginMain(Spv* s) {
    SpvId id = spvNewId(s);
    spvEmit(&s->code, OP_Function, SPV_WORDS(s->voidType, id, 0, spvTypeFunction(s, s->voidType)));
    spvLabel(s, 0);
    return id;
}

void spvEndMain(Spv* s) {
    spvEmit(&s->code, OP_Return, NULL, 0);
    spvEmit(&s->code, OP_FunctionEnd, NULL, 0);
}

SpvId spvLabel(Spv* s, SpvId label) {
    if (!label) label = spvNewId(s);
    spvEmit(&s->code, OP_Label, SPV_WORDS(label));
    s->block = label;
    return label;
}

SpvId spvPhi(Spv* s, SpvId type, SpvId a, SpvId from, SpvId b, SpvId other) {
    SpvId id = spvNewId(s);
    spvEmit(&s->code, 245 /* OpPhi */, SPV_WORDS(type, id, a, from, b, other));
    return id;
}

SpvId spvOp2(Spv* s, uint32_t op, SpvId type, SpvId a, SpvId b) {
    SpvId id = spvNewId(s);
    spvEmit(&s->code, op, SPV_WORDS(type, id, a, b));
    return id;
}

SpvId spvOp1(Spv* s, uint32_t op, SpvId type, SpvId a) {
    SpvId id = spvNewId(s);
    spvEmit(&s->code, op, SPV_WORDS(type, id, a));
    return id;
}

SpvId spvSelect(Spv* s, SpvId type, SpvId condition, SpvId a, SpvId b) {
    SpvId id = spvNewId(s);
    spvEmit(&s->code, SPV_OP_Select, SPV_WORDS(type, id, condition, a, b));
    return id;
}

SpvId spvGlsl1(Spv* s, uint32_t inst, SpvId type, SpvId a) {
    SpvId id = spvNewId(s);
    spvEmit(&s->code, SPV_OP_ExtInst, SPV_WORDS(type, id, s->glsl, inst, a));
    return id;
}

SpvId spvGlsl2(Spv* s, uint32_t inst, SpvId type, SpvId a, SpvId b) {
    SpvId id = spvNewId(s);
    spvEmit(&s->code, SPV_OP_ExtInst, SPV_WORDS(type, id, s->glsl, inst, a, b));
    return id;
}

/* ---- core/color.h's arithmetic ---- */

#define C_F(v) spvConstFloat(s, v)
#define C_U(v) spvConstUint(s, v)
#define F_ADD(a, b) spvOp2(s, SPV_OP_FAdd, s->floatType, a, b)
#define F_SUB(a, b) spvOp2(s, SPV_OP_FSub, s->floatType, a, b)
#define F_MUL(a, b) spvOp2(s, SPV_OP_FMul, s->floatType, a, b)
#define F_DIV(a, b) spvOp2(s, SPV_OP_FDiv, s->floatType, a, b)

SpvId spvLog2Fast(Spv* s, SpvId x) {
    SpvId bits = spvOp1(s, SPV_OP_Bitcast, s->uintType, x);
    SpvId e = F_SUB(spvOp1(s, SPV_OP_ConvertUToF, s->floatType, spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, spvOp2(s, SPV_OP_ShiftRightLogical, s->uintType, bits, C_U(23)), C_U(255))), C_F(127.f));
    SpvId m = spvOp1(s, SPV_OP_Bitcast, s->floatType, spvOp2(s, 197 /* OpBitwiseOr */, s->uintType, spvOp2(s, SPV_OP_BitwiseAnd, s->uintType, bits, C_U(0x7FFFFFu)), C_U(0x3F800000u)));
    SpvId z = F_DIV(F_SUB(m, C_F(1.f)), F_ADD(m, C_F(1.f))), z2 = F_MUL(z, z);
    SpvId series = F_ADD(C_F(1.f), F_MUL(z2, F_ADD(C_F(.33333333f), F_MUL(z2, F_ADD(C_F(.2f), F_MUL(z2, C_F(.14285714f)))))));
    return F_ADD(e, F_MUL(F_MUL(z, C_F(2.8853901f)), series));
}

SpvId spvExp2Fast(Spv* s, SpvId x) {
    x = spvGlsl2(s, SPV_GLSL_FMin, s->floatType, spvGlsl2(s, SPV_GLSL_FMax, s->floatType, x, C_F(-100.f)), C_F(100.f));
    SpvId whole = spvGlsl1(s, SPV_GLSL_Floor, s->floatType, x);
    SpvId u = F_MUL(F_SUB(x, whole), C_F(.69314718f));
    SpvId f = C_F(.0013888889f);
    const float c[6] = {.0083333333f, .041666668f, .16666667f, .5f, 1.f, 1.f};
    for (int k = 0; k < 6; ++k) f = F_ADD(C_F(c[k]), F_MUL(u, f));
    SpvId i = spvOp1(s, SPV_OP_ConvertFToS, s->intType, whole);
    SpvId bits = spvOp2(s, SPV_OP_ShiftLeftLogical, s->uintType, spvOp1(s, SPV_OP_Bitcast, s->uintType, spvOp2(s, SPV_OP_IAdd, s->intType, i, spvConstInt(s, 127))), C_U(23));
    return F_MUL(f, spvOp1(s, SPV_OP_Bitcast, s->floatType, bits));
}

SpvId spvSrgbDecode(Spv* s, SpvId shown) {
    static const float c[7] = ARTF_SRGB_DECODE;
    SpvId p = C_F(c[6]);
    for (int k = 5; k >= 0; --k) p = F_ADD(F_MUL(p, shown), C_F(c[k]));
    SpvId above = spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, shown, C_F(.04045f));
    return spvSelect(s, s->floatType, above, p, F_MUL(shown, C_F(1.f / 12.92f)));
}

SpvId spvSrgbEncode(Spv* s, SpvId light) {
    static const float c[6] = ARTF_SRGB_ENCODE;
    SpvId floor0 = spvGlsl2(s, SPV_GLSL_FMax, s->floatType, light, C_F(0.f));
    SpvId t = spvGlsl1(s, SPV_GLSL_Sqrt, s->floatType, spvGlsl1(s, SPV_GLSL_Sqrt, s->floatType, floor0));
    SpvId p = C_F(c[5]);
    for (int k = 4; k >= 0; --k) p = F_ADD(F_MUL(p, t), C_F(c[k]));
    SpvId above = spvOp2(s, SPV_OP_FOrdGreaterThan, s->boolType, light, C_F(.0031308f));
    return spvSelect(s, s->floatType, above, p, F_MUL(floor0, C_F(12.92f)));
}

#undef C_F
#undef C_U
#undef F_ADD
#undef F_SUB
#undef F_MUL
#undef F_DIV

SpvId spvCompose(Spv* s, SpvId type, const SpvId* parts, uint32_t count) {
    uint32_t words[8] = {type, spvNewId(s)};
    if (count) memcpy(words + 2, parts, (size_t)count * 4);
    spvEmit(&s->code, OP_CompositeConstruct, words, count + 2);
    return words[1];
}

SpvId spvExtract(Spv* s, SpvId type, SpvId v, uint32_t index) {
    SpvId id = spvNewId(s);
    spvEmit(&s->code, OP_CompositeExtract, SPV_WORDS(type, id, v, index));
    return id;
}

SpvId spvShuffle3(Spv* s, SpvId v) {
    SpvId id = spvNewId(s);
    spvEmit(&s->code, OP_VectorShuffle, SPV_WORDS(s->vec3, id, v, v, 0, 1, 2));
    return id;
}

SpvId spvAccess(Spv* s, SpvId pointerType, SpvId base, const SpvId* indices, uint32_t count) {
    uint32_t words[8] = {pointerType, spvNewId(s), base};
    if (count) memcpy(words + 3, indices, (size_t)count * 4);
    spvEmit(&s->code, OP_AccessChain, words, count + 3);
    return words[1];
}

SpvId spvLoad(Spv* s, SpvId type, SpvId pointer) { return spvOp1(s, SPV_OP_Load, type, pointer); }
void spvStore(Spv* s, SpvId pointer, SpvId value) { spvEmit(&s->code, SPV_OP_Store, SPV_WORDS(pointer, value)); }

SpvId spvAtomicAdd(Spv* s, SpvId pointer, SpvId value) {
    SpvId id = spvNewId(s);
    spvEmit(&s->code, OP_AtomicIAdd, SPV_WORDS(s->uintType, id, pointer, spvConstUint(s, 1) /* Device */, spvConstUint(s, 0) /* Relaxed */, value));
    return id;
}

SpvId spvImageSample(Spv* s, SpvId sampledImage, SpvId uv) { return spvOp2(s, OP_ImageSampleImplicitLod, s->vec4, sampledImage, uv); }

SpvId spvImageFetch(Spv* s, SpvId imageType, SpvId sampledImage, SpvId coords) {
    SpvId image = spvOp1(s, OP_Image, imageType, sampledImage);
    SpvId id = spvNewId(s);
    spvEmit(&s->code, OP_ImageFetch, SPV_WORDS(s->vec4, id, image, coords, 2 /* Lod */, spvConstInt(s, 0)));
    return id;
}

SpvId spvImageFetchAs(Spv* s, SpvId resultType, SpvId imageType, SpvId sampledImage, SpvId coords) {
    SpvId image = spvOp1(s, OP_Image, imageType, sampledImage);
    SpvId id = spvNewId(s);
    spvEmit(&s->code, OP_ImageFetch, SPV_WORDS(resultType, id, image, coords, 2 /* Lod */, spvConstInt(s, 0)));
    return id;
}

SpvId spvTypeSampledImage2DMSOf(Spv* s, SpvId sampledType, SpvId* imageType) {
    *imageType = typeOf(s, OP_TypeImage, SPV_WORDS(sampledType, 1, 0, 0, 1, 1, 0));
    return typeOf(s, OP_TypeSampledImage, SPV_WORDS(*imageType));
}

SpvId spvImageFetchSampleAs(Spv* s, SpvId resultType, SpvId imageType, SpvId sampledImage, SpvId coords, SpvId sample) {
    SpvId image = spvOp1(s, OP_Image, imageType, sampledImage);
    SpvId id = spvNewId(s);
    spvEmit(&s->code, OP_ImageFetch, SPV_WORDS(resultType, id, image, coords, 0x40 /* Sample */, sample));
    return id;
}

SpvId spvTypeSampledImage2DOf(Spv* s, SpvId sampledType, SpvId* imageType) {
    *imageType = typeOf(s, OP_TypeImage, SPV_WORDS(sampledType, 1, 0, 0, 0, 1, 0));
    return typeOf(s, OP_TypeSampledImage, SPV_WORDS(*imageType));
}

SpvId spvTypeVector(Spv* s, SpvId element, uint32_t count) { return typeOf(s, 23 /* OpTypeVector */, SPV_WORDS(element, count)); }

SpvId spvImageSampleLod(Spv* s, SpvId sampledImage, SpvId uv, SpvId lod) {
    SpvId id = spvNewId(s);
    spvEmit(&s->code, 88 /* OpImageSampleExplicitLod */, SPV_WORDS(s->vec4, id, sampledImage, uv, 2 /* Lod */, lod));
    return id;
}

SpvId spvImageSampleGrad(Spv* s, SpvId sampledImage, SpvId uv, SpvId dx, SpvId dy) {
    SpvId id = spvNewId(s);
    spvEmit(&s->code, 88 /* OpImageSampleExplicitLod */, SPV_WORDS(s->vec4, id, sampledImage, uv, 4 /* Grad */, dx, dy));
    return id;
}

SpvId spvImageFetchSample(Spv* s, SpvId imageType, SpvId sampledImage, SpvId coords, SpvId sample) {
    SpvId image = spvOp1(s, OP_Image, imageType, sampledImage);
    SpvId id = spvNewId(s);
    spvEmit(&s->code, OP_ImageFetch, SPV_WORDS(s->vec4, id, image, coords, 0x40 /* Sample */, sample));
    return id;
}

void spvBranch(Spv* s, SpvId label) { spvEmit(&s->code, OP_Branch, SPV_WORDS(label)); }

void spvIfBegin(Spv* s, SpvId condition, SpvId* merge) {
    SpvId yes = spvNewId(s);
    *merge = spvNewId(s);
    spvEmit(&s->code, OP_SelectionMerge, SPV_WORDS(*merge, 0));
    spvEmit(&s->code, OP_BranchConditional, SPV_WORDS(condition, yes, *merge));
    spvLabel(s, yes);
}

void spvIfElseBegin(Spv* s, SpvId condition, SpvId* no, SpvId* merge) {
    SpvId yes = spvNewId(s);
    *no = spvNewId(s);
    *merge = spvNewId(s);
    spvEmit(&s->code, OP_SelectionMerge, SPV_WORDS(*merge, 0));
    spvEmit(&s->code, OP_BranchConditional, SPV_WORDS(condition, yes, *no));
    spvLabel(s, yes);
}

void spvIfEnd(Spv* s, SpvId merge) {
    spvBranch(s, merge);
    spvLabel(s, merge);
}

void spvKillIf(Spv* s, SpvId condition) {
    SpvId kill = spvNewId(s), merge = spvNewId(s);
    spvEmit(&s->code, OP_SelectionMerge, SPV_WORDS(merge, 0));
    spvEmit(&s->code, OP_BranchConditional, SPV_WORDS(condition, kill, merge));
    spvLabel(s, kill);
    spvEmit(&s->code, 252 /* OpKill */, NULL, 0);
    spvLabel(s, merge);
}

void spvLoopMerge(Spv* s, SpvId merge, SpvId next) { spvEmit(&s->code, OP_LoopMerge, SPV_WORDS(merge, next, 0)); }
void spvBranchIf(Spv* s, SpvId condition, SpvId yes, SpvId no) { spvEmit(&s->code, OP_BranchConditional, SPV_WORDS(condition, yes, no)); }

bool artfSpvCheck = false;
int artfSpvChecksFailed = 0;
void (*artfSpvFinished)(const IrModule* m) = NULL;

uint32_t* spvFinish(Spv* s, size_t* wordCount) {
    if (artfSpvCheck) {
        char why[256];
        if (!irCheck(&s->ir, why, sizeof why)) {
            ++artfSpvChecksFailed;
            artfLog("artful: a shader's IR is not sound: %s", why);
        }
    }
    if (artfShaderDebugInfo && s->file) irDebugInfo(&s->ir, s->file, s->function ? s->function : "main", (uint32_t)s->line);
    if (artfSpvFinished) artfSpvFinished(&s->ir);
    return irSpirv(&s->ir, wordCount);
}
