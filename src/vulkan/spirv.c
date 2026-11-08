/* artful — writing SPIR-V. A module's sections go into their own lists, and are joined at the end. */
#include "vulkan/spirv.h"

enum {
    OP_Extension = 10, OP_ExtInstImport = 11, OP_MemoryModel = 14, OP_EntryPoint = 15, OP_ExecutionMode = 16, OP_Capability = 17,
    OP_TypeVoid = 19, OP_TypeBool = 20, OP_TypeInt = 21, OP_TypeFloat = 22, OP_TypeVector = 23, OP_TypeImage = 25, OP_TypeSampledImage = 27,
    OP_TypeArray = 28, OP_TypeRuntimeArray = 29, OP_TypeStruct = 30, OP_TypePointer = 32, OP_TypeFunction = 33, OP_Constant = 43,
    OP_Function = 54, OP_FunctionEnd = 56, OP_Variable = 59, OP_AccessChain = 65, OP_Decorate = 71, OP_MemberDecorate = 72,
    OP_VectorShuffle = 79, OP_CompositeConstruct = 80, OP_CompositeExtract = 81, OP_ImageSampleImplicitLod = 87, OP_ImageFetch = 95, OP_Image = 100,
    OP_AtomicIAdd = 234, OP_LoopMerge = 246, OP_SelectionMerge = 247, OP_Label = 248, OP_Branch = 249, OP_BranchConditional = 250, OP_Return = 253
};

void spvEmit(SpvWords* to, uint32_t op, const uint32_t* operands, uint32_t n) {
    artfVecPush(to, (n + 1) << 16 | op);
    for (uint32_t i = 0; i < n; ++i) artfVecPush(to, operands[i]);
}

SpvId spvNewId(Spv* s) { return ++s->bound; }

/* A string's words, NUL included. */
static uint32_t packString(const char* text, uint32_t* out) {
    const size_t len = strlen(text) + 1;
    const uint32_t words = (uint32_t)((len + 3) / 4);
    memset(out, 0, (size_t)words * 4);
    memcpy(out, text, len);
    return words;
}

/* A type made of operands, once each. (The list is small, and read when a shader is built.) */
static SpvId typeOf(Spv* s, uint32_t op, const uint32_t* operands, uint32_t n) {
    for (size_t i = 0; i < s->types.len;) {
        const uint32_t head = s->types.items[i], length = head >> 16;
        if ((head & 0xFFFF) == op && length == n + 2 && !memcmp(&s->types.items[i + 2], operands, (size_t)n * 4)) return s->types.items[i + 1];
        i += length;
    }
    uint32_t words[16];
    words[0] = spvNewId(s);
    memcpy(words + 1, operands, (size_t)n * 4);
    spvEmit(&s->types, op, words, n + 1);
    return words[0];
}

void spvInit(Spv* s) {
    memset(s, 0, sizeof *s);
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

void spvFini(Spv* s) {
    artfVecFree(&s->head);
    artfVecFree(&s->annotations);
    artfVecFree(&s->types);
    artfVecFree(&s->code);
}

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
    memcpy(words + 1, members, (size_t)count * 4);
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
    for (size_t i = 0; i < s->types.len; i += s->types.items[i] >> 16)
        if ((s->types.items[i] & 0xFFFF) == OP_Constant && s->types.items[i + 1] == type && s->types.items[i + 3] == value) return s->types.items[i + 2];
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
    return label;
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

SpvId spvCompose(Spv* s, SpvId type, const SpvId* parts, uint32_t count) {
    uint32_t words[8] = {type, spvNewId(s)};
    memcpy(words + 2, parts, (size_t)count * 4);
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
    memcpy(words + 3, indices, (size_t)count * 4);
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

SpvId spvImageSampleLod(Spv* s, SpvId sampledImage, SpvId uv, SpvId lod) {
    SpvId id = spvNewId(s);
    spvEmit(&s->code, 88 /* OpImageSampleExplicitLod */, SPV_WORDS(s->vec4, id, sampledImage, uv, 2 /* Lod */, lod));
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

uint32_t* spvFinish(Spv* s, size_t* wordCount) {
    const size_t total = 5 + s->head.len + s->annotations.len + s->types.len + s->code.len;
    uint32_t* out = artfMalloc(total * 4);
    out[0] = 0x07230203;
    out[1] = 0x00010300; /* SPIR-V 1.3: Vulkan 1.1 and up, StorageBuffer without an extension */
    out[2] = 0;
    out[3] = s->bound + 1;
    out[4] = 0;
    size_t at = 5;
    const SpvWords* sections[4] = {&s->head, &s->annotations, &s->types, &s->code};
    for (int i = 0; i < 4; ++i) {
        if (sections[i]->len) memcpy(out + at, sections[i]->items, sections[i]->len * 4);
        at += sections[i]->len;
    }
    *wordCount = total;
    return out;
}
