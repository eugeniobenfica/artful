/* artful — WebGPU's shading language from a shader's IR (wgsl.h). Its shape is msl.c's: the module's facts, its
 * types and constants, the code a block at a time from the structured merges, every value a variable of the function.
 * What differs is WGSL's: no implicit conversion between signed and unsigned (each operand is cast to what its
 * operation takes), textures and samplers apart, no arrays of textures, atomics by type, and no ?:. */
#include "shaderir/wgsl.h"

#include <stdarg.h>
#include <string.h>

typedef ARTF_VEC(char) Text;

static void say(Text* t, const char* format, ...) {
    va_list a;
    va_start(a, format);
    const int n = vsnprintf(NULL, 0, format, a);
    va_end(a);
    if (n <= 0) return;
    const size_t at = t->len;
    for (int i = 0; i <= n; ++i) artfVecPush(t, '\0');
    va_start(a, format);
    vsnprintf(t->items + at, (size_t)n + 1, format, a);
    va_end(a);
    t->len = at + (size_t)n;
}

static char* copyOf(const char* s) {
    const size_t n = strlen(s) + 1;
    char* c = artfMalloc(n);
    memcpy(c, s, n);
    return c;
}

static char* format(const char* f, ...) {
    va_list a;
    va_start(a, f);
    const int n = vsnprintf(NULL, 0, f, a);
    va_end(a);
    char* out = artfMalloc((size_t)(n > 0 ? n : 0) + 1);
    va_start(a, f);
    vsnprintf(out, (size_t)(n > 0 ? n : 0) + 1, f, a);
    va_end(a);
    return out;
}

static char* textOf(Text* t) {
    artfVecPush(t, '\0');
    char* out = copyOf(t->items);
    artfVecFree(t);
    return out;
}

enum { NO_VALUE = 0xFFFFFFFFu };

typedef struct Facts {
    uint32_t set, binding, location, builtin, arrayStride;
    bool flat, noPerspective, centroid, sample, block, nonWritable;
    uint32_t members[4]; /* of a struct: the builtin each of its first members is (gl_PerVertex's) */
    bool perVertex;
    bool invariant, invariantMember;
    bool atomic;       /* of a variable: atomics touch it, so it is declared of atomics */
    bool atomicType;   /* of a struct: one of atomics is declared too (Sn_a) */
    uint32_t textures; /* of an array of textures: the elements read, a bit each (up to 64) */
    uint64_t texturesUsed;
} Facts;

typedef struct Loop { IrId header, merge, cont; } Loop;

typedef struct Wgsl {
    const IrModule* m;
    const IrWgslOptions* o;
    uint32_t model; /* 0 vertex, 4 fragment, 5 compute */
    Facts* facts;
    char** types;
    char** expr;
    char** sampler;
    IrId* root;       /* of a pointer: the variable it points into */
    uint32_t* labelAt;
    uint32_t* code;
    uint32_t codeCount;
    bool hasOut;
    IrId split; /* (the output splitSurface splits: set whole, and its words go out as the function returns) */
    Text body;
    int indent;
    Loop loops[16];
    int loopCount;
    bool failed;
    char why[256];
} Wgsl;

static void fail(Wgsl* s, const char* format_, ...) {
    if (s->failed) return;
    s->failed = true;
    va_list a;
    va_start(a, format_);
    vsnprintf(s->why, sizeof s->why, format_, a);
    va_end(a);
}

static const IrInst* def(const Wgsl* s, IrId id) { return irDef(s->m, id); }
static const uint32_t* ops(const Wgsl* s, const IrInst* i) { return irOperands(s->m, i); }

static uint32_t constantValue(const Wgsl* s, IrId id) {
    const IrInst* c = def(s, id);
    return c && c->op == 43 && c->count ? ops(s, c)[0] : 0;
}
static bool isConstant(const Wgsl* s, IrId id) {
    const IrInst* c = def(s, id);
    return c && c->op == 43;
}

/* ---- types ---- */

static const char* typeName(Wgsl* s, IrId id);

static char* imageName(Wgsl* s, const uint32_t* w) {
    /* sampled type, dim, depth, arrayed, multisampled, sampled (1: with a sampler, 2: storage), format */
    const char* of = typeName(s, w[0]);
    if (w[1] != 1) { fail(s, "an image of dimension %u", w[1]); return copyOf("texture_2d<f32>"); }
    if (w[5] == 2) {
        const char* f = NULL;
        switch (w[6]) {
        case 1: f = "rgba32float"; break;
        case 2: f = "rgba16float"; break;
        case 3: f = "r32float"; break;
        case 4: f = "rgba8unorm"; break;
        case 6: f = "rg32float"; break;
        case 21: f = "rgba32sint"; break;
        case 24: f = "r32sint"; break;
        case 30: f = "rgba32uint"; break;
        case 33: f = "r32uint"; break;
        case 35: f = "rg32uint"; break;
        default: fail(s, "a storage image of format %u", w[6]); f = "rgba8unorm"; break;
        }
        return format("texture_storage_2d<%s, read_write>", f);
    }
    return w[4] ? format("texture_multisampled_2d<%s>", of) : format("texture_2d<%s>", of);
}

static const char* typeName(Wgsl* s, IrId id) {
    if (id <= s->m->bound && s->types[id]) return s->types[id];
    const IrInst* t = def(s, id);
    if (!t) {
        fail(s, "%%%u is no type", id);
        return "u32";
    }
    const uint32_t* w = ops(s, t);
    char* name = NULL;
    switch (t->op) {
    case 19: name = copyOf("void"); break;
    case 20: name = copyOf("bool"); break;
    case 21: name = copyOf(w[1] ? "i32" : "u32"); break;
    case 22: name = copyOf("f32"); break;
    case 23: name = format("vec%u<%s>", w[1], typeName(s, w[0])); break;
    case 28: name = format("array<%s, %u>", typeName(s, w[0]), constantValue(s, w[1])); break;
    case 29: name = format("array<%s>", typeName(s, w[0])); break;
    case 30: name = format("S%u", id); break;
    case 25: name = imageName(s, w); break;
    case 26: name = copyOf("sampler"); break;
    case 27: name = copyOf(typeName(s, w[0])); break;
    case 32: name = copyOf(typeName(s, w[1])); break;
    default:
        if (t->op == 4472 || t->op == 5341) fail(s, "ray queries (Op%u): not written for WGSL", t->op);
        else fail(s, "a type WGSL is not written for here: Op%u", t->op);
        name = copyOf("u32");
        break;
    }
    s->types[id] = name;
    return name;
}

/* The same type with its integers atomic: what a variable that atomics touch is declared as. */
static char* atomicName(Wgsl* s, IrId id) {
    const IrInst* t = def(s, id);
    if (!t) return copyOf("u32");
    const uint32_t* w = ops(s, t);
    switch (t->op) {
    case 21: return format("atomic<%s>", w[1] ? "i32" : "u32");
    case 28: { char* e = atomicName(s, w[0]); char* r = format("array<%s, %u>", e, constantValue(s, w[1])); artfFree(e); return r; }
    case 29: { char* e = atomicName(s, w[0]); char* r = format("array<%s>", e); artfFree(e); return r; }
    case 30: return format("S%u_a", id);
    default: return copyOf(typeName(s, id));
    }
}

static void markAtomic(Wgsl* s, IrId type) {
    const IrInst* t = def(s, type);
    if (!t) return;
    const uint32_t* w = ops(s, t);
    if (t->op == 28 || t->op == 29) markAtomic(s, w[0]);
    else if (t->op == 30) {
        s->facts[type].atomicType = true;
        for (uint32_t k = 0; k < t->count; ++k) markAtomic(s, w[k]);
    }
}

static IrId pointee(const Wgsl* s, IrId pointerType) {
    const IrInst* t = def(s, pointerType);
    return t && t->op == 32 ? ops(s, t)[1] : 0;
}

/* An integer type's other signedness (scalar or vector), by name: what a value is cast to for an operation that takes
 * one or the other. NULL: not an integer. */
static const char* intVariant(Wgsl* s, IrId type, bool signed_) {
    const IrInst* t = def(s, type);
    if (!t) return NULL;
    const uint32_t* w = ops(s, t);
    if (t->op == 21) return signed_ ? "i32" : "u32";
    if (t->op == 23) {
        const IrInst* e = def(s, w[0]);
        if (!e || e->op != 21) return NULL;
        static const char* names[2][5] = {{"", "", "vec2<u32>", "vec3<u32>", "vec4<u32>"}, {"", "", "vec2<i32>", "vec3<i32>", "vec4<i32>"}};
        return w[1] <= 4 ? names[signed_][w[1]] : NULL;
    }
    return NULL;
}

/* WGSL's size and alignment of a type in a buffer (host-shareable), to match Vulkan's offsets. */
static void sizeOf(Wgsl* s, IrId type, uint32_t* size, uint32_t* align) {
    const IrInst* t = def(s, type);
    *size = *align = 4;
    if (!t) return;
    const uint32_t* w = ops(s, t);
    switch (t->op) {
    case 23: {
        uint32_t es, ea;
        sizeOf(s, w[0], &es, &ea);
        *size = es * w[1];
        *align = es * (w[1] == 3 ? 4 : w[1]);
        break;
    }
    case 28: case 29: {
        uint32_t es, ea;
        sizeOf(s, w[0], &es, &ea);
        const uint32_t stride = (es + ea - 1) / ea * ea;
        *size = stride * (t->op == 28 ? constantValue(s, w[1]) : 1);
        *align = ea;
        break;
    }
    case 30: {
        uint32_t at = 0, most = 4;
        for (uint32_t k = 0; k < t->count; ++k) {
            uint32_t ms, ma;
            sizeOf(s, w[k], &ms, &ma);
            at = (at + ma - 1) / ma * ma + ms;
            if (ma > most) most = ma;
        }
        *size = (at + most - 1) / most * most;
        *align = most;
        break;
    }
    default: break;
    }
}

static uint32_t memberOffset(const Wgsl* s, IrId structType, uint32_t member) {
    for (size_t a = 0; a < s->m->insts.len; ++a) {
        const IrInst* d = &s->m->insts.items[a];
        if (d->op == 72 && d->count >= 4 && ops(s, d)[0] == structType && ops(s, d)[1] == member && ops(s, d)[2] == 35) return ops(s, d)[3];
    }
    return NO_VALUE;
}

static void declareStruct(Wgsl* s, Text* out, const IrInst* t, bool atomics) {
    const uint32_t* w = ops(s, t);
    say(out, "struct S%u%s {\n", t->result, atomics ? "_a" : "");
    uint32_t at = 0;
    for (uint32_t i = 0; i < t->count; ++i) {
        uint32_t ms, ma;
        sizeOf(s, w[i], &ms, &ma);
        at = (at + ma - 1) / ma * ma;
        const uint32_t offset = memberOffset(s, t->result, i);
        if (offset != NO_VALUE && offset != at) {
            if (offset < at || offset % ma || (offset - at) % 4) { fail(s, "member %u of struct %%%u is at %u in Vulkan, %u in WGSL", i, t->result, offset, at); return; }
            /* (further on in Vulkan: words of nothing before it, as many as it is further) */
            for (uint32_t k = 0; at < offset; ++k, at += 4) say(out, "    pad%u_%u: u32,\n", i, k);
        }
        say(out, "    ");
        const uint32_t stride = s->facts[w[i]].arrayStride;
        const IrInst* member = def(s, w[i]);
        if (stride && member && (member->op == 28 || member->op == 29)) {
            uint32_t es, ea;
            sizeOf(s, ops(s, member)[0], &es, &ea);
            if (stride != (es + ea - 1) / ea * ea) fail(s, "an array of stride %u, where WGSL's elements are %u apart", stride, (es + ea - 1) / ea * ea);
        }
        if (atomics) {
            char* name = atomicName(s, w[i]);
            say(out, " m%u: %s,\n", i, name);
            artfFree(name);
        } else say(out, " m%u: %s,\n", i, typeName(s, w[i]));
        at += ms;
    }
    say(out, "};\n\n");
}

static void declareStructs(Wgsl* s, Text* out) {
    for (size_t k = 0; k < s->m->insts.len; ++k) {
        const IrInst* t = &s->m->insts.items[k];
        if (t->section != IR_TYPES || t->op != 30) continue;
        declareStruct(s, out, t, false);
        if (s->facts[t->result].atomicType) declareStruct(s, out, t, true);
    }
}

/* ---- values ---- */

static const char* valueOf(Wgsl* s, IrId id) {
    if (id <= s->m->bound && s->expr[id]) return s->expr[id];
    if (id > s->m->bound) return "?";
    s->expr[id] = format("_%u", id);
    return s->expr[id];
}

static char* floatText(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    if (f != f || f - f != 0) return format("bitcast<f32>(0x%08Xu)", bits);
    char text[48];
    snprintf(text, sizeof text, "%.9g", (double)f);
    if (!strpbrk(text, ".eE")) strcat(text, ".0");
    return format("%sf", text);
}

static void constants(Wgsl* s) {
    for (size_t k = 0; k < s->m->insts.len; ++k) {
        const IrInst* c = &s->m->insts.items[k];
        if (c->section != IR_TYPES) continue;
        const uint32_t* w = ops(s, c);
        if (c->op == 41 || c->op == 42) s->expr[c->result] = copyOf(c->op == 41 ? "true" : "false");
        else if (c->op == 43) {
            const IrInst* t = def(s, c->type);
            if (t && t->op == 22) s->expr[c->result] = floatText(w[0]);
            else if (t && t->op == 21 && ops(s, t)[1]) s->expr[c->result] = (int32_t)w[0] == INT32_MIN ? copyOf("i32(-2147483647 - 1)") : format("%di", (int32_t)w[0]);
            else s->expr[c->result] = format("%uu", w[0]);
        } else if (c->op == 44) {
            Text t = {0};
            say(&t, "%s(", typeName(s, c->type));
            for (uint32_t i = 0; i < c->count; ++i) say(&t, "%s%s", i ? ", " : "", valueOf(s, w[i]));
            say(&t, ")");
            s->expr[c->result] = textOf(&t);
        } else if (c->op == 46) s->expr[c->result] = format("%s()", typeName(s, c->type));
    }
}

/* A value as the type named takes it: as it is, or its bits as that (integers of the other signedness). */
static char* asType(Wgsl* s, IrId value, const char* want) {
    const IrId type = irTypeOf(s->m, value);
    const char* have = type ? typeName(s, type) : want;
    if (!want || !strcmp(have, want)) return copyOf(valueOf(s, value));
    return format("bitcast<%s>(%s)", want, valueOf(s, value));
}
static char* asTypeOf(Wgsl* s, IrId value, IrId type) { return asType(s, value, typeName(s, type)); }

/* ---- the code ---- */

static void line(Wgsl* s, const char* format_, ...) {
    for (int i = 0; i < s->indent; ++i) say(&s->body, "    ");
    va_list a;
    va_start(a, format_);
    const int n = vsnprintf(NULL, 0, format_, a);
    va_end(a);
    char* text = artfMalloc((size_t)(n > 0 ? n : 0) + 1);
    va_start(a, format_);
    vsnprintf(text, (size_t)(n > 0 ? n : 0) + 1, format_, a);
    va_end(a);
    say(&s->body, "%s\n", text);
    artfFree(text);
}

static const char* glslOf(uint32_t inst, int* args) {
    *args = 1;
    switch (inst) {
    case 1: case 2: return "round";
    case 3: return "trunc";
    case 4: case 5: return "abs";
    case 6: case 7: return "sign";
    case 8: return "floor";
    case 9: return "ceil";
    case 10: return "fract";
    case 11: return "radians";
    case 12: return "degrees";
    case 13: return "sin";
    case 14: return "cos";
    case 15: return "tan";
    case 16: return "asin";
    case 17: return "acos";
    case 18: return "atan";
    case 19: return "sinh";
    case 20: return "cosh";
    case 21: return "tanh";
    case 25: *args = 2; return "atan2";
    case 26: *args = 2; return "pow";
    case 27: return "exp";
    case 28: return "log";
    case 29: return "exp2";
    case 30: return "log2";
    case 31: return "sqrt";
    case 32: return "inverseSqrt";
    case 37: case 38: case 39: case 79: *args = 2; return "min";
    case 40: case 41: case 42: case 80: *args = 2; return "max";
    case 43: case 44: case 45: case 81: *args = 3; return "clamp";
    case 46: *args = 3; return "mix";
    case 48: *args = 2; return "step";
    case 49: *args = 3; return "smoothstep";
    case 50: *args = 3; return "fma";
    case 54: return "pack4x8snorm";
    case 55: return "pack4x8unorm";
    case 56: return "pack2x16snorm";
    case 57: return "pack2x16unorm";
    case 58: return "pack2x16float";
    case 60: return "unpack2x16snorm";
    case 61: return "unpack2x16unorm";
    case 62: return "unpack2x16float";
    case 63: return "unpack4x8snorm";
    case 64: return "unpack4x8unorm";
    case 66: return "length";
    case 67: *args = 2; return "distance";
    case 68: *args = 2; return "cross";
    case 69: return "normalize";
    case 70: *args = 3; return "faceForward";
    case 71: *args = 2; return "reflect";
    case 72: *args = 3; return "refract";
    case 73: return "firstTrailingBit";
    case 74: case 75: return "firstLeadingBit";
    default: return NULL;
    }
}

/* A pointer's expression, an index at a time. */
static char* chainOf(Wgsl* s, const IrInst* i) {
    const uint32_t* w = ops(s, i);
    IrId type = pointee(s, irTypeOf(s->m, w[0]));
    if (!type) {
        const IrInst* base = def(s, w[0]);
        type = base ? pointee(s, base->type) : 0;
    }
    if (w[0] <= s->m->bound && s->facts[w[0]].builtin == 20) return copyOf(valueOf(s, w[0]));
    Text t = {0};
    uint32_t from = 1;
    if (w[0] <= s->m->bound && s->facts[w[0]].perVertex && i->count > 1) {
        /* (gl_PerVertex's position goes out; its other members, as the point's size, into a variable of nothing) */
        const IrInst* block = def(s, type);
        const uint32_t member = constantValue(s, w[1]);
        const uint32_t builtin = member < 4 ? s->facts[type].members[member] : NO_VALUE;
        say(&t, "%s", builtin == 0 ? "o.gl_Position" : "gl_unused");
        type = block && member < block->count ? ops(s, block)[member] : 0;
        from = 2;
    } else say(&t, "%s", valueOf(s, w[0]));
    for (uint32_t k = from; k < i->count; ++k) {
        const IrInst* at = def(s, type);
        if (!at) break;
        if (at->op == 30) {
            const uint32_t member = constantValue(s, w[k]);
            say(&t, ".m%u", member);
            type = ops(s, at)[member];
        } else if (at->op == 28 || at->op == 29 || at->op == 23) {
            say(&t, "[%s]", valueOf(s, w[k]));
            type = ops(s, at)[0];
        }
    }
    return textOf(&t);
}

static void phisFrom(Wgsl* s, IrId block) {
    for (size_t k = 0; k < s->m->insts.len; ++k) {
        const IrInst* p = &s->m->insts.items[k];
        if (p->op != 245) continue;
        const uint32_t* w = ops(s, p);
        for (uint32_t i = 0; i + 1 < p->count; i += 2)
            if (w[i + 1] == block) line(s, "_%u = %s;", p->result, valueOf(s, w[i]));
    }
}

static bool isVector(Wgsl* s, IrId value) {
    const IrInst* t = def(s, irTypeOf(s->m, value));
    return t && t->op == 23;
}

static bool atomicRoot(const Wgsl* s, IrId pointer) {
    const IrId r = pointer <= s->m->bound ? s->root[pointer] : 0;
    return r && s->facts[r].atomic;
}

/* An integer operation: its operands cast to what it takes (signed_: -1 as the result is, 0 unsigned, 1 signed),
 * the result cast back. */
static void integerOp(Wgsl* s, const IrInst* i, const char* op, int signed_) {
    const uint32_t* w = ops(s, i);
    const char* result = typeName(s, i->type);
    const char* work = signed_ < 0 ? result : intVariant(s, i->type, signed_ == 1);
    if (!work) work = result;
    char* a = asType(s, w[0], work);
    char* b = asType(s, w[1], work);
    if (!strcmp(work, result)) line(s, "_%u = %s %s %s;", i->result, a, op, b);
    else line(s, "_%u = bitcast<%s>(%s %s %s);", i->result, result, a, op, b);
    artfFree(a);
    artfFree(b);
}

static void compareOp(Wgsl* s, const IrInst* i, const char* op, int signed_) {
    const uint32_t* w = ops(s, i);
    const IrId type = irTypeOf(s->m, w[0]);
    const char* work = signed_ < 0 ? typeName(s, type) : intVariant(s, type, signed_ == 1);
    char* a = asType(s, w[0], work);
    char* b = asType(s, w[1], work);
    line(s, "_%u = %s %s %s;", i->result, a, op, b);
    artfFree(a);
    artfFree(b);
}

static void shiftOp(Wgsl* s, const IrInst* i, const char* op, int signed_) {
    const uint32_t* w = ops(s, i);
    const char* result = typeName(s, i->type);
    const char* work = signed_ < 0 ? result : intVariant(s, i->type, signed_ == 1);
    char* a = asType(s, w[0], work);
    char* b = asType(s, w[1], intVariant(s, irTypeOf(s->m, w[1]), false));
    if (!strcmp(work, result)) line(s, "_%u = %s %s %s;", i->result, a, op, b);
    else line(s, "_%u = bitcast<%s>(%s %s %s);", i->result, result, a, op, b);
    artfFree(a);
    artfFree(b);
}

static void instruction(Wgsl* s, const IrInst* i) {
    const uint32_t* w = ops(s, i);
    const IrId r = i->result;
    switch (i->op) {
    case 59: case 246: case 247: case 1: case 8: case 317: return;
    /* float arithmetic */
    case 129: line(s, "_%u = %s + %s;", r, valueOf(s, w[0]), valueOf(s, w[1])); return;
    case 131: line(s, "_%u = %s - %s;", r, valueOf(s, w[0]), valueOf(s, w[1])); return;
    case 133: case 142: line(s, "_%u = %s * %s;", r, valueOf(s, w[0]), valueOf(s, w[1])); return;
    case 136: line(s, "_%u = %s / %s;", r, valueOf(s, w[0]), valueOf(s, w[1])); return;
    case 140: line(s, "_%u = %s %% %s;", r, valueOf(s, w[0]), valueOf(s, w[1])); return;
    case 141: line(s, "_%u = %s - %s * floor(%s / %s);", r, valueOf(s, w[0]), valueOf(s, w[1]), valueOf(s, w[0]), valueOf(s, w[1])); return;
    /* integer arithmetic */
    case 128: integerOp(s, i, "+", -1); return;
    case 130: integerOp(s, i, "-", -1); return;
    case 132: integerOp(s, i, "*", -1); return;
    case 134: integerOp(s, i, "/", 0); return;
    case 135: integerOp(s, i, "/", 1); return;
    case 137: integerOp(s, i, "%", 0); return;
    case 138: case 139: integerOp(s, i, "%", 1); return;
    case 197: integerOp(s, i, "|", -1); return;
    case 198: integerOp(s, i, "^", -1); return;
    case 199: integerOp(s, i, "&", -1); return;
    case 194: shiftOp(s, i, ">>", 0); return;
    case 195: shiftOp(s, i, ">>", 1); return;
    case 196: shiftOp(s, i, "<<", -1); return;
    /* comparisons */
    case 164: compareOp(s, i, "==", -1); return;
    case 165: compareOp(s, i, "!=", -1); return;
    case 166: line(s, "_%u = %s %s %s;", r, valueOf(s, w[0]), isVector(s, w[0]) ? "|" : "||", valueOf(s, w[1])); return;
    case 167: line(s, "_%u = %s %s %s;", r, valueOf(s, w[0]), isVector(s, w[0]) ? "&" : "&&", valueOf(s, w[1])); return;
    case 170: compareOp(s, i, "==", -1); return;
    case 171: compareOp(s, i, "!=", -1); return;
    case 172: compareOp(s, i, ">", 0); return;
    case 173: compareOp(s, i, ">", 1); return;
    case 174: compareOp(s, i, ">=", 0); return;
    case 175: compareOp(s, i, ">=", 1); return;
    case 176: compareOp(s, i, "<", 0); return;
    case 177: compareOp(s, i, "<", 1); return;
    case 178: compareOp(s, i, "<=", 0); return;
    case 179: compareOp(s, i, "<=", 1); return;
    case 180: case 181: compareOp(s, i, "==", -1); return;
    case 182: case 183: compareOp(s, i, "!=", -1); return;
    case 184: case 185: compareOp(s, i, "<", -1); return;
    case 186: case 187: compareOp(s, i, ">", -1); return;
    case 188: case 189: compareOp(s, i, "<=", -1); return;
    case 190: case 191: compareOp(s, i, ">=", -1); return;
    case 61: {
        const IrInst* t = def(s, i->type);
        if (t && (t->op == 27 || t->op == 25 || t->op == 26)) {
            s->expr[r] = copyOf(valueOf(s, w[0]));
            if (s->sampler[w[0]]) s->sampler[r] = copyOf(s->sampler[w[0]]);
            return;
        }
        if (atomicRoot(s, w[0])) line(s, "_%u = atomicLoad(&%s);", r, valueOf(s, w[0]));
        else line(s, "_%u = %s;", r, valueOf(s, w[0]));
        return;
    }
    case 62: {
        if (atomicRoot(s, w[0])) { line(s, "atomicStore(&%s, %s);", valueOf(s, w[0]), valueOf(s, w[1])); return; }
        /* (the sample mask: an int in SPIR-V, a u32 in WGSL) */
        const IrId target = w[0] <= s->m->bound ? s->root[w[0]] : 0;
        if (target && s->facts[target].builtin == 20) {
            char* v = asType(s, w[1], "u32");
            line(s, "%s = %s;", valueOf(s, w[0]), v);
            artfFree(v);
            return;
        }
        line(s, "%s = %s;", valueOf(s, w[0]), valueOf(s, w[1]));
        return;
    }
    case 65: case 66: {
        const IrId base = w[0];
        const IrInst* v = def(s, base);
        const IrId type = v && v->op == 59 ? pointee(s, v->type) : 0;
        const IrInst* t = type ? def(s, type) : NULL;
        if (t && t->op == 28 && s->sampler[base]) {
            /* an element of an array of textures: by a constant index, its own texture and sampler */
            if (!isConstant(s, w[1])) { fail(s, "an array of textures read by an index that varies (ARTF_TEXTURES_UNIFORM)"); return; }
            const uint32_t k = constantValue(s, w[1]);
            s->expr[r] = format("%s_%u", valueOf(s, base), k);
            s->sampler[r] = format("%s_s", valueOf(s, base));
            return;
        }
        s->expr[r] = chainOf(s, i);
        return;
    }
    case 79: {
        const IrInst* a = def(s, irTypeOf(s->m, w[0]));
        const uint32_t n = a && a->op == 23 ? ops(s, a)[1] : 4;
        static const char xyzw[] = "xyzw";
        if (w[0] == w[1]) {
            char swizzle[8] = {0};
            for (uint32_t k = 2; k < i->count && k < 6; ++k) swizzle[k - 2] = w[k] < n ? xyzw[w[k]] : xyzw[w[k] - n];
            line(s, "_%u = %s.%s;", r, valueOf(s, w[0]), swizzle);
            return;
        }
        Text t = {0};
        for (uint32_t k = 2; k < i->count; ++k) say(&t, "%s%s.%c", k > 2 ? ", " : "", valueOf(s, w[k] < n ? w[0] : w[1]), xyzw[w[k] < n ? w[k] : w[k] - n]);
        char* parts = textOf(&t);
        line(s, "_%u = %s(%s);", r, typeName(s, i->type), parts);
        artfFree(parts);
        return;
    }
    case 80: {
        Text t = {0};
        for (uint32_t k = 0; k < i->count; ++k) say(&t, "%s%s", k ? ", " : "", valueOf(s, w[k]));
        char* parts = textOf(&t);
        line(s, "_%u = %s(%s);", r, typeName(s, i->type), parts);
        artfFree(parts);
        return;
    }
    case 81: case 82: {
        const bool insert = i->op == 82;
        const IrId whole = insert ? w[1] : w[0];
        IrId type = irTypeOf(s->m, whole);
        Text t = {0};
        if (insert) {
            line(s, "_%u = %s;", r, valueOf(s, whole));
            say(&t, "_%u", r);
        } else say(&t, "%s", valueOf(s, whole));
        for (uint32_t k = insert ? 2 : 1; k < i->count; ++k) {
            const IrInst* at = def(s, type);
            if (!at) break;
            if (at->op == 23) { say(&t, ".%c", "xyzw"[w[k] & 3]); type = ops(s, at)[0]; }
            else if (at->op == 30) { say(&t, ".m%u", w[k]); type = ops(s, at)[w[k]]; }
            else { say(&t, "[%u]", w[k]); type = ops(s, at)[0]; }
        }
        char* path = textOf(&t);
        if (insert) line(s, "%s = %s;", path, valueOf(s, w[0]));
        else line(s, "_%u = %s;", r, path);
        artfFree(path);
        return;
    }
    case 77: line(s, "_%u = %s[%s];", r, valueOf(s, w[0]), valueOf(s, w[1])); return;
    case 78: line(s, "_%u = %s;", r, valueOf(s, w[0])); line(s, "_%u[%s] = %s;", r, valueOf(s, w[2]), valueOf(s, w[1])); return;
    case 83: line(s, "_%u = %s;", r, valueOf(s, w[0])); return;
    case 12: {
        const IrInst* set = irDef(s->m, w[0]);
        if (set && set->op == 11 && irOperands(s->m, set)[0] == 0x536E6F4Eu /* "NonS" */) return;
        int args;
        const char* f = glslOf(w[1], &args);
        if (!f) { fail(s, "GLSL.std.450's instruction %u", w[1]); return; }
        const char* result = typeName(s, i->type);
        /* (integers: what the function takes, by its signedness, and back) */
        const bool unsignedFn = w[1] == 38 || w[1] == 41 || w[1] == 44 || w[1] == 75, signedFn = w[1] == 5 || w[1] == 7 || w[1] == 39 || w[1] == 42 || w[1] == 45 || w[1] == 74;
        const char* work = unsignedFn ? intVariant(s, i->type, false) : signedFn ? intVariant(s, i->type, true) : NULL;
        if (w[1] == 75 || w[1] == 74 || w[1] == 73) work = intVariant(s, irTypeOf(s->m, w[2]), w[1] == 74);
        char* a[3] = {NULL, NULL, NULL};
        for (int k = 0; k < args; ++k) a[k] = work ? asType(s, w[2 + k], work) : copyOf(valueOf(s, w[2 + k]));
        char* call = args == 1 ? format("%s(%s)", f, a[0]) : args == 2 ? format("%s(%s, %s)", f, a[0], a[1]) : format("%s(%s, %s, %s)", f, a[0], a[1], a[2]);
        if (work && strcmp(work, result)) line(s, "_%u = bitcast<%s>(%s);", r, result, call);
        else line(s, "_%u = %s;", r, call);
        for (int k = 0; k < 3; ++k) artfFree(a[k]);
        artfFree(call);
        return;
    }
    case 87: case 88: {
        const char* smp = s->sampler[w[0]];
        if (!smp) { fail(s, "a sample with no sampler (%%%u)", w[0]); return; }
        const uint32_t mask = i->count > 2 ? w[2] : 0;
        if (i->op == 87) {
            if (mask & 1) line(s, "_%u = textureSampleBias(%s, %s, %s, %s);", r, valueOf(s, w[0]), smp, valueOf(s, w[1]), valueOf(s, w[3]));
            else line(s, "_%u = textureSample(%s, %s, %s);", r, valueOf(s, w[0]), smp, valueOf(s, w[1]));
        } else if (mask == 4) line(s, "_%u = textureSampleGrad(%s, %s, %s, %s, %s);", r, valueOf(s, w[0]), smp, valueOf(s, w[1]), valueOf(s, w[3]), valueOf(s, w[4]));
        else line(s, "_%u = textureSampleLevel(%s, %s, %s, %s);", r, valueOf(s, w[0]), smp, valueOf(s, w[1]), i->count > 3 ? valueOf(s, w[3]) : "0.0f");
        return;
    }
    case 100: s->expr[r] = copyOf(valueOf(s, w[0])); return;
    case 95: case 98: {
        const uint32_t mask = i->count > 2 ? w[2] : 0;
        const IrInst* image = def(s, irTypeOf(s->m, w[0]));
        const bool storage = image && image->op == 25 && ops(s, image)[5] == 2;
        char* at = format("vec2<i32>(%s)", valueOf(s, w[1]));
        char* got;
        if (storage) got = format("textureLoad(%s, %s)", valueOf(s, w[0]), at);
        else if (mask & 0x40) got = format("textureLoad(%s, %s, %s)", valueOf(s, w[0]), at, valueOf(s, w[3]));
        else got = format("textureLoad(%s, %s, %s)", valueOf(s, w[0]), at, mask & 2 ? valueOf(s, w[3]) : "0");
        const char* result = typeName(s, i->type);
        const char* of = image ? typeName(s, ops(s, image)[0]) : "f32";
        char texel[32];
        snprintf(texel, sizeof texel, "vec4<%s>", of);
        if (strcmp(result, texel)) line(s, "_%u = bitcast<%s>(%s);", r, result, got);
        else line(s, "_%u = %s;", r, got);
        artfFree(at);
        artfFree(got);
        return;
    }
    case 99: line(s, "textureStore(%s, vec2<i32>(%s), %s);", valueOf(s, w[0]), valueOf(s, w[1]), valueOf(s, w[2])); return;
    case 103: line(s, "_%u = %s(textureDimensions(%s, u32(%s)));", r, typeName(s, i->type), valueOf(s, w[0]), valueOf(s, w[1])); return;
    case 104: line(s, "_%u = %s(textureDimensions(%s));", r, typeName(s, i->type), valueOf(s, w[0])); return;
    case 106: line(s, "_%u = %s(textureNumLevels(%s));", r, typeName(s, i->type), valueOf(s, w[0])); return;
    case 107: line(s, "_%u = %s(textureNumSamples(%s));", r, typeName(s, i->type), valueOf(s, w[0])); return;
    case 109: case 110: case 115: line(s, "_%u = %s(%s);", r, typeName(s, i->type), valueOf(s, w[0])); return;
    case 111: case 112: case 113: case 114: {
        /* (from an integer of the signedness the operation says) */
        const IrId from = irTypeOf(s->m, w[0]);
        const char* as = intVariant(s, from, i->op == 111 || i->op == 114);
        char* v = asType(s, w[0], as);
        line(s, "_%u = %s(%s);", r, typeName(s, i->type), v);
        artfFree(v);
        return;
    }
    case 124: line(s, "_%u = bitcast<%s>(%s);", r, typeName(s, i->type), valueOf(s, w[0])); return;
    case 126: { char* v = asTypeOf(s, w[0], i->type); line(s, "_%u = -%s;", r, v); artfFree(v); return; }
    case 127: line(s, "_%u = -%s;", r, valueOf(s, w[0])); return;
    case 148: line(s, "_%u = dot(%s, %s);", r, valueOf(s, w[0]), valueOf(s, w[1])); return;
    case 154: line(s, "_%u = any(%s);", r, valueOf(s, w[0])); return;
    case 155: line(s, "_%u = all(%s);", r, valueOf(s, w[0])); return;
    case 156: line(s, "_%u = %s != %s;", r, valueOf(s, w[0]), valueOf(s, w[0])); return;
    case 157: line(s, "_%u = abs(%s) > %s(3.40282347e38f);", r, valueOf(s, w[0]), typeName(s, irTypeOf(s->m, w[0]))); return;
    case 168: line(s, "_%u = !%s;", r, valueOf(s, w[0])); return;
    case 169: {
        char* a = asTypeOf(s, w[1], i->type);
        char* b = asTypeOf(s, w[2], i->type);
        line(s, "_%u = select(%s, %s, %s);", r, b, a, valueOf(s, w[0]));
        artfFree(a);
        artfFree(b);
        return;
    }
    case 200: { char* v = asTypeOf(s, w[0], i->type); line(s, "_%u = ~%s;", r, v); artfFree(v); return; }
    case 204: { char* v = asTypeOf(s, w[0], i->type); line(s, "_%u = reverseBits(%s);", r, v); artfFree(v); return; }
    case 205: {
        const char* result = typeName(s, i->type);
        const char* of = typeName(s, irTypeOf(s->m, w[0]));
        if (strcmp(result, of)) line(s, "_%u = bitcast<%s>(countOneBits(%s));", r, result, valueOf(s, w[0]));
        else line(s, "_%u = countOneBits(%s);", r, valueOf(s, w[0]));
        return;
    }
    case 207: line(s, "_%u = dpdx(%s);", r, valueOf(s, w[0])); return;
    case 208: line(s, "_%u = dpdy(%s);", r, valueOf(s, w[0])); return;
    case 209: line(s, "_%u = fwidth(%s);", r, valueOf(s, w[0])); return;
    case 210: line(s, "_%u = dpdxFine(%s);", r, valueOf(s, w[0])); return;
    case 211: line(s, "_%u = dpdyFine(%s);", r, valueOf(s, w[0])); return;
    case 212: line(s, "_%u = fwidthFine(%s);", r, valueOf(s, w[0])); return;
    case 213: line(s, "_%u = dpdxCoarse(%s);", r, valueOf(s, w[0])); return;
    case 214: line(s, "_%u = dpdyCoarse(%s);", r, valueOf(s, w[0])); return;
    case 215: line(s, "_%u = fwidthCoarse(%s);", r, valueOf(s, w[0])); return;
    case 224: case 225: {
        const uint32_t semantics = constantValue(s, i->op == 224 ? w[2] : w[1]);
        if (semantics & 0x40) line(s, "storageBarrier();");
        if (semantics & 0x800) line(s, "textureBarrier();");
        line(s, "workgroupBarrier();");
        return;
    }
    case 227: line(s, "_%u = atomicLoad(&%s);", r, valueOf(s, w[0])); return;
    case 228: { char* v = asType(s, w[3], NULL); line(s, "atomicStore(&%s, %s);", valueOf(s, w[0]), v); artfFree(v); return; }
    case 230: {
        char* v = asTypeOf(s, w[4], i->type);
        char* c = asTypeOf(s, w[5], i->type);
        line(s, "_%u = atomicCompareExchangeWeak(&%s, %s, %s).old_value;", r, valueOf(s, w[0]), c, v);
        artfFree(v);
        artfFree(c);
        return;
    }
    case 232: line(s, "_%u = atomicAdd(&%s, 1);", r, valueOf(s, w[0])); return;
    case 233: line(s, "_%u = atomicSub(&%s, 1);", r, valueOf(s, w[0])); return;
    case 229: case 234: case 235: case 236: case 237: case 238: case 239: case 240: case 241: case 242: {
        static const char* names[] = {"atomicExchange", NULL, NULL, NULL, NULL, "atomicAdd", "atomicSub", "atomicMin", "atomicMin", "atomicMax", "atomicMax", "atomicAnd", "atomicOr", "atomicXor"};
        char* v = asTypeOf(s, w[3], i->type);
        line(s, "_%u = %s(&%s, %s);", r, names[i->op - 229], valueOf(s, w[0]), v);
        artfFree(v);
        return;
    }
    case 245: return;
    default:
        if (i->op >= 4472 || i->op == 5341) fail(s, "ray queries (Op%u): not written for WGSL", i->op);
        else fail(s, "Op%u: not written for WGSL", i->op);
        return;
    }
}

static void blockFrom(Wgsl* s, IrId label, IrId stop);

static bool jump(Wgsl* s, IrId target) {
    for (int k = s->loopCount - 1; k >= 0; --k) {
        const Loop* l = &s->loops[k];
        if (target != l->merge && target != l->header && target != l->cont) continue;
        if (k != s->loopCount - 1) { fail(s, "a branch out of more than one loop"); return true; }
        if (target == l->merge) line(s, "break;");
        else if (target == l->header) line(s, "continue;");
        else blockFrom(s, l->cont, 0);
        return true;
    }
    return false;
}

static void blockFrom(Wgsl* s, IrId label, IrId stop) {
    for (int guard = 0; label && label != stop && guard < 100000 && !s->failed; ++guard) {
        if (label > s->m->bound || !s->labelAt[label]) { fail(s, "a branch to %%%u, which is no block", label); return; }
#define CODE(k) (&s->m->insts.items[s->code[k]])
        const uint32_t at = s->labelAt[label] - 1;
        uint32_t end = at + 1;
        while (end < s->codeCount && CODE(end)->op != 249 && CODE(end)->op != 250 && CODE(end)->op != 252 && CODE(end)->op != 253) ++end;
        if (end == s->codeCount) { fail(s, "block %%%u does not end", label); return; }
        const IrInst* merge = end > at + 1 ? CODE(end - 1) : NULL;
        const bool header = merge && merge->op == 246 && !(s->loopCount && s->loops[s->loopCount - 1].header == label);
        if (header) {
            if (s->loopCount == 16) { fail(s, "loops nested past 16"); return; }
            const Loop l = {label, ops(s, merge)[0], ops(s, merge)[1]};
            s->loops[s->loopCount++] = l;
            line(s, "loop {");
            ++s->indent;
            blockFrom(s, label, 0);
            --s->indent;
            line(s, "}");
            --s->loopCount;
            label = l.merge;
            continue;
        }
        for (uint32_t k = at + 1; k < end; ++k) instruction(s, CODE(k));
        const IrInst* t = CODE(end);
#undef CODE
        const uint32_t* w = ops(s, t);
        phisFrom(s, label);
        if (t->op == 252) { line(s, "discard;"); return; }
        if (t->op == 253) {
            if (s->o && s->o->flipY && s->model == 0) line(s, "o.gl_Position.y = -o.gl_Position.y;");
            if (s->split) {
                char words[160];
                static const char* const parts[3] = {"", "_1", "_2"};
                for (int k = 0; k < 3; ++k) {
                    snprintf(words, sizeof words, "o.m_%u%s = vec2<u32>(w_%u.%c & 0xFFFFu, w_%u.%c >> 16u);", s->split, parts[k], s->split, "xyz"[k], s->split, "xyz"[k]);
                    line(s, words);
                }
            }
            line(s, s->hasOut ? "return o;" : "return;");
            return;
        }
        if (t->op == 249) {
            if (jump(s, w[0])) return;
            label = w[0];
            continue;
        }
        const bool selection = merge && merge->op == 247;
        const IrId yes = w[1], no = w[2], meet = selection ? ops(s, merge)[0] : 0;
        if (selection && yes == meet) {
            line(s, "if (!(%s)) {", valueOf(s, w[0]));
            ++s->indent;
            blockFrom(s, no, meet);
            --s->indent;
            line(s, "}");
        } else {
            line(s, "if (%s) {", valueOf(s, w[0]));
            ++s->indent;
            if (!(!selection && jump(s, yes))) blockFrom(s, yes, meet);
            --s->indent;
            if (no != meet) {
                line(s, "} else {");
                ++s->indent;
                if (!(!selection && jump(s, no))) blockFrom(s, no, meet);
                --s->indent;
            }
            line(s, "}");
        }
        if (!selection) return;
        label = meet;
    }
}

/* ---- the module ---- */

static const char* builtinName(uint32_t b) {
    switch (b) {
    case 0: return "gl_Position";
    case 15: return "gl_FragCoord";
    case 17: return "gl_FrontFacing";
    case 18: return "gl_SampleID";
    case 20: return "gl_SampleMask";
    case 22: return "gl_FragDepth";
    case 26: return "gl_WorkGroupID";
    case 27: return "gl_LocalInvocationID";
    case 28: return "gl_GlobalInvocationID";
    case 29: return "gl_LocalInvocationIndex";
    case 42: return "gl_VertexIndex";
    case 43: return "gl_InstanceIndex";
    default: return NULL;
    }
}
static const char* builtinIn(uint32_t b, const char** type) {
    switch (b) {
    case 15: *type = "vec4<f32>"; return "position";
    case 17: *type = "bool"; return "front_facing";
    case 18: *type = "u32"; return "sample_index";
    case 20: *type = "u32"; return "sample_mask";
    case 26: *type = "vec3<u32>"; return "workgroup_id";
    case 27: *type = "vec3<u32>"; return "local_invocation_id";
    case 28: *type = "vec3<u32>"; return "global_invocation_id";
    case 29: *type = "u32"; return "local_invocation_index";
    case 42: *type = "u32"; return "vertex_index";
    case 43: *type = "u32"; return "instance_index";
    default: *type = NULL; return NULL;
    }
}

static const char* interpolation(const Facts* f, const char* type) {
    const bool whole = !strncmp(type, "u32", 3) || !strncmp(type, "i32", 3) || strstr(type, "<u32>") || strstr(type, "<i32>");
    if (whole || f->flat) return " @interpolate(flat)";
    if (f->noPerspective) return f->centroid ? " @interpolate(linear, centroid)" : f->sample ? " @interpolate(linear, sample)" : " @interpolate(linear)";
    if (f->centroid) return " @interpolate(perspective, centroid)";
    if (f->sample) return " @interpolate(perspective, sample)";
    return "";
}

static void bound(IrWgslBindings* b, uint32_t group, uint32_t binding, uint32_t kind, uint32_t source, uint32_t element) {
    if (!b || b->count >= IR_WGSL_BINDINGS) return;
    const IrWgslBinding e = {group, binding, kind, source, element};
    b->items[b->count++] = e;
}

char* irWgsl(const IrModule* m, const IrWgslOptions* options, IrWgslBindings* bindings, char* why, size_t size) {
    if (bindings) bindings->count = 0;
    Wgsl s;
    memset(&s, 0, sizeof s);
    s.m = m;
    s.o = options;
    const size_t ids = (size_t)m->bound + 1;
    s.facts = artfCalloc(ids, sizeof *s.facts);
    s.types = artfCalloc(ids, sizeof *s.types);
    s.expr = artfCalloc(ids, sizeof *s.expr);
    s.sampler = artfCalloc(ids, sizeof *s.sampler);
    s.root = artfCalloc(ids, sizeof *s.root);
    s.labelAt = artfCalloc(ids, sizeof *s.labelAt);
    ARTF_VEC(uint32_t) code = {0};
    for (size_t k = 0; k < ids; ++k) {
        s.facts[k].set = s.facts[k].binding = s.facts[k].location = s.facts[k].builtin = NO_VALUE;
        for (int b = 0; b < 4; ++b) s.facts[k].members[b] = NO_VALUE;
    }
    uint32_t groupSize[3] = {1, 1, 1};
    for (size_t k = 0; k < m->insts.len; ++k) {
        const IrInst* i = &m->insts.items[k];
        const uint32_t* w = irOperands(m, i);
        if (i->op == 15) s.model = w[0];
        if (i->op == 16 && i->count >= 5 && w[1] == 17) { groupSize[0] = w[2]; groupSize[1] = w[3]; groupSize[2] = w[4]; }
        if (i->section == IR_CODE) {
            if (i->op == 248) s.labelAt[i->result] = s.codeCount + 1;
            artfVecPush(&code, (uint32_t)k);
            ++s.codeCount;
            /* (what each pointer points into) */
            if (i->op == 59) s.root[i->result] = i->result;
            if ((i->op == 65 || i->op == 66) && w[0] < ids) s.root[i->result] = s.root[w[0]] ? s.root[w[0]] : w[0];
        }
        if (i->op == 59 && i->section == IR_TYPES) s.root[i->result] = i->result;
        if (i->op == 71 && i->count >= 2 && w[0] < ids) {
            Facts* f = &s.facts[w[0]];
            const uint32_t value = i->count > 2 ? w[2] : 0;
            switch (w[1]) {
            case 2: f->block = true; break;
            case 6: f->arrayStride = value; break;
            case 11: f->builtin = value; break;
            case 13: f->noPerspective = true; break;
            case 14: f->flat = true; break;
            case 16: f->centroid = true; break;
            case 17: f->sample = true; break;
            case 18: f->invariant = true; break;
            case 30: f->location = value; break;
            case 33: f->binding = value; break;
            case 34: f->set = value; break;
            default: break;
            }
        }
        if (i->op == 72 && i->count >= 3 && w[0] < ids && w[1] == 0 && w[2] == 24) s.facts[w[0]].nonWritable = true;
        if (i->op == 72 && i->count >= 4 && w[0] < ids && w[1] < 4 && w[2] == 11) s.facts[w[0]].members[w[1]] = w[3];
        if (i->op == 72 && i->count >= 3 && w[0] < ids && w[2] == 18) s.facts[w[0]].invariantMember = true;
    }
    s.code = code.items;
    /* what atomics touch: its variable, and its type's atomic twin */
    for (uint32_t k = 0; k < s.codeCount; ++k) {
        const IrInst* i = &m->insts.items[s.code[k]];
        if (i->op < 227 || i->op > 242) continue;
        const IrId p = irOperands(m, i)[0];
        const IrId root = p < ids ? s.root[p] : 0;
        if (!root) continue;
        s.facts[root].atomic = true;
        const IrInst* v = def(&s, root);
        if (v) markAtomic(&s, pointee(&s, v->type));
    }
    constants(&s);

    Text head = {0}, out = {0};
    /* (samples and derivatives where the IR has them: in control flow WGSL's analysis cannot call uniform) */
    say(&head, "diagnostic(off, derivative_uniformity);\n\n");
    declareStructs(&s, &head);

    uint8_t* used = artfCalloc(ids, 1);
    for (uint32_t k = 0; k < s.codeCount; ++k) {
        const IrInst* i = &m->insts.items[s.code[k]];
        const uint32_t* w = irOperands(m, i);
        const uint8_t* kinds = irKinds(m, i);
        for (uint32_t o = 0; o < i->count; ++o)
            if (kinds[o] == IR_OPERAND_ID && w[o] < ids) used[w[o]] = 1;
        /* (an array of textures: the elements read, each declared on its own) */
        if ((i->op == 65 || i->op == 66) && i->count > 1 && w[0] < ids && isConstant(&s, w[1])) {
            const uint32_t e = constantValue(&s, w[1]);
            if (e < 64) s.facts[w[0]].texturesUsed |= 1ull << e;
        }
    }
    IrUse uses[96];
    const int useCount = irUses(m, uses, 96);
    Text in = {0}, outs = {0}, params = {0}, top = {0};
    for (size_t k = 0; k < m->insts.len; ++k) {
        const IrInst* v = &m->insts.items[k];
        if (v->op != 59 || v->section != IR_TYPES) continue;
        const IrId id = v->result, type = pointee(&s, v->type);
        const uint32_t storage = irOperands(m, v)[0];
        const Facts* f = &s.facts[id];
        const uint32_t set = f->set == NO_VALUE ? 0 : f->set;
        switch (storage) {
        case 0: {
            if (!used[id]) break;
            const IrInst* t = def(&s, type);
            const bool many = t && t->op == 28;
            const IrInst* element = many ? def(&s, ops(&s, t)[0]) : t;
            const bool sampled = element && element->op == 27;
            const char* image = typeName(&s, element ? element->result : type);
            const uint32_t imageKind = !strncmp(image, "texture_multisampled", 20) ? IR_WGSL_TEXTURE_MULTISAMPLED : !strncmp(image, "texture_storage", 15) ? IR_WGSL_STORAGE_TEXTURE
                                       : strstr(image, "<u32>") || strstr(image, "<i32>") ? IR_WGSL_TEXTURE_UINT : IR_WGSL_TEXTURE;
            s.expr[id] = format("g%u_b%u", set, f->binding);
            if (many) {
                for (uint32_t e = 0; e < 64; ++e) {
                    if (!(f->texturesUsed >> e & 1)) continue;
                    say(&head, "@group(%u) @binding(%u) var g%u_b%u_%u: %s;\n", set, IR_WGSL_ARRAY_AT + e, set, f->binding, e, image);
                    bound(bindings, set, IR_WGSL_ARRAY_AT + e, imageKind, f->binding, e);
                }
                if (sampled && f->texturesUsed) {
                    /* (one sampler for them all, as Vulkan's array has: WebGPU has few) */
                    say(&head, "@group(%u) @binding(%u) var g%u_b%u_s: sampler;\n", set, IR_WGSL_ARRAY_AT - 1, set, f->binding);
                    bound(bindings, set, IR_WGSL_ARRAY_AT - 1, IR_WGSL_SAMPLER, f->binding, 0);
                }
                s.sampler[id] = copyOf("(array)");
            } else if (element && element->op == 26) {
                say(&head, "@group(%u) @binding(%u) var g%u_b%u: sampler;\n", set, f->binding, set, f->binding);
                bound(bindings, set, f->binding, IR_WGSL_SAMPLER, f->binding, 0);
            } else {
                say(&head, "@group(%u) @binding(%u) var g%u_b%u: %s;\n", set, f->binding, set, f->binding, image);
                bound(bindings, set, f->binding, imageKind, f->binding, 0);
                if (sampled) {
                    say(&head, "@group(%u) @binding(%u) var g%u_b%u_s: sampler;\n", set, f->binding + IR_WGSL_SAMPLER_AT, set, f->binding);
                    bound(bindings, set, f->binding + IR_WGSL_SAMPLER_AT, IR_WGSL_SAMPLER, f->binding, 0);
                    s.sampler[id] = format("g%u_b%u_s", set, f->binding);
                }
            }
            break;
        }
        case 2: case 12: {
            if (!used[id]) break;
            bool written = false;
            for (int u = 0; u < useCount; ++u)
                if (uses[u].set == set && uses[u].binding == f->binding && uses[u].write) written = true;
            char* typeText = f->atomic ? atomicName(&s, type) : copyOf(typeName(&s, type));
            if (storage == 2) say(&head, "@group(%u) @binding(%u) var<uniform> g%u_b%u: %s;\n", set, f->binding, set, f->binding, typeText);
            else say(&head, "@group(%u) @binding(%u) var<storage, %s> g%u_b%u: %s;\n", set, f->binding, written || f->atomic ? "read_write" : "read", set, f->binding, typeText);
            bound(bindings, set, f->binding, storage == 2 ? IR_WGSL_UNIFORM : written || f->atomic ? IR_WGSL_STORAGE : IR_WGSL_STORAGE_READ, f->binding, 0);
            artfFree(typeText);
            s.expr[id] = format("g%u_b%u", set, f->binding);
            break;
        }
        case 9:
            if (!used[id]) break;
            say(&head, "@group(0) @binding(%u) var<uniform> pc: %s;\n", IR_WGSL_PUSH_AT, typeName(&s, type));
            bound(bindings, 0, IR_WGSL_PUSH_AT, IR_WGSL_UNIFORM, IR_WGSL_PUSH_AT, 0);
            s.expr[id] = copyOf("pc");
            break;
        case 1: {
            if (f->builtin != NO_VALUE && !used[id]) break;
            if (f->builtin != NO_VALUE) {
                const char* wgslType;
                const char* attribute = builtinIn(f->builtin, &wgslType);
                const char* name = builtinName(f->builtin);
                if (!attribute) { fail(&s, "builtin %u coming in", f->builtin); break; }
                say(&params, "%s@builtin(%s) %s: %s", params.len ? ", " : "", attribute, name, wgslType);
                const char* spirvType = typeName(&s, type);
                if (f->builtin == 20) {
                    /* (an array of one int in SPIR-V; chainOf leaves its index out) */
                    const IrInst* t = def(&s, type);
                    spirvType = t && (t->op == 28 || t->op == 29) ? typeName(&s, ops(&s, t)[0]) : spirvType;
                }
                s.expr[id] = strcmp(spirvType, wgslType) ? format("bitcast<%s>(%s)", spirvType, name) : copyOf(name);
            } else {
                const char* name = typeName(&s, type);
                say(&params, "%s@location(%u)%s m_%u: %s", params.len ? ", " : "", f->location, s.model == 4 ? interpolation(f, name) : "", id, name);
                s.expr[id] = format("m_%u", id);
            }
            break;
        }
        case 3: {
            if (f->builtin != NO_VALUE) {
                const char* name = builtinName(f->builtin);
                if (f->builtin == 0) say(&outs, "    %s@builtin(position) gl_Position: vec4<f32>,\n", f->invariant ? "@invariant " : "");
                else if (f->builtin == 22) say(&outs, "    @builtin(frag_depth) gl_FragDepth: f32,\n");
                else if (f->builtin == 20) say(&outs, "    @builtin(sample_mask) gl_SampleMask: u32,\n");
                else { fail(&s, "builtin %u going out", f->builtin); break; }
                s.expr[id] = format("o.%s", name);
            } else if (type < ids && s.facts[type].members[0] != NO_VALUE) {
                for (int b = 0; b < 4; ++b) {
                    const uint32_t builtin = s.facts[type].members[b];
                    if (builtin == 0) say(&outs, "    %s@builtin(position) gl_Position: vec4<f32>,\n", f->invariant || s.facts[type].invariantMember ? "@invariant " : "");
                    else if (builtin != NO_VALUE && builtin != 1 && builtin != 3 && builtin != 4) fail(&s, "builtin %u of a block going out", builtin);
                }
                s.facts[id].perVertex = true;
                s.expr[id] = copyOf("o");
            } else if (s.o && s.o->splitSurface && s.model == 4 && f->location == 1 && !strcmp(typeName(&s, type), "vec4<u32>")) {
                say(&outs, "    @location(1) m_%u: vec2<u32>,\n    @location(2) m_%u_1: vec2<u32>,\n    @location(3) m_%u_2: vec2<u32>,\n", id, id, id);
                say(&top, "    var w_%u: vec4<u32>;\n", id);
                s.expr[id] = format("w_%u", id);
                s.split = id;
            } else {
                const char* name = typeName(&s, type);
                say(&outs, "    @location(%u)%s m_%u: %s,\n", f->location, s.model == 0 ? interpolation(f, name) : "", id, name);
                s.expr[id] = format("o.m_%u", id);
            }
            break;
        }
        case 4: {
            char* typeText = f->atomic ? atomicName(&s, type) : copyOf(typeName(&s, type));
            say(&head, "var<workgroup> v_%u: %s;\n", id, typeText);
            artfFree(typeText);
            s.expr[id] = format("v_%u", id);
            break;
        }
        case 6:
            if (v->count > 1) say(&head, "var<private> v_%u: %s = %s;\n", id, typeName(&s, type), valueOf(&s, irOperands(m, v)[1]));
            else say(&head, "var<private> v_%u: %s;\n", id, typeName(&s, type));
            s.expr[id] = format("v_%u", id);
            break;
        default: fail(&s, "a variable of storage %u", storage); break;
        }
    }
    bool perVertex = false;
    for (size_t k = 0; k < ids; ++k) perVertex |= s.facts[k].perVertex;
    if (perVertex) say(&top, "    var gl_unused: f32;\n");
    if (outs.len) {
        char* text = textOf(&outs);
        say(&head, "\nstruct Out {\n%s};\n", text);
        artfFree(text);
        s.hasOut = true;
    }
    say(&head, "\n");

    /* the function: its values declared at its top, then its blocks */
    IrId first = 0;
    for (size_t k = 0; k < m->insts.len; ++k) {
        const IrInst* i = &m->insts.items[k];
        if (i->section != IR_CODE) continue;
        if (i->op == 248 && !first) first = i->result;
        if (i->op == 59) {
            say(&top, "    var v_%u: %s;\n", i->result, typeName(&s, pointee(&s, i->type)));
            s.expr[i->result] = format("v_%u", i->result);
            continue;
        }
        if (!i->result || !i->type || i->op == 54 || i->op == 65 || i->op == 66 || i->op == 100) continue;
        const IrInst* t = def(&s, i->type);
        if (t && (t->op == 25 || t->op == 26 || t->op == 27 || t->op == 19)) continue;
        say(&top, "    var _%u: %s;\n", i->result, typeName(&s, i->type));
    }
    s.indent = 1;
    if (s.hasOut) line(&s, "var o: Out;");
    blockFrom(&s, first, 0);

    char* paramText = textOf(&params);
    if (s.model == 5) say(&out, "@compute @workgroup_size(%u, %u, %u)\nfn main(%s) {\n", groupSize[0], groupSize[1], groupSize[2], paramText);
    else say(&out, "@%s\nfn main(%s)%s {\n", s.model == 4 ? "fragment" : "vertex", paramText, s.hasOut ? " -> Out" : "");
    artfFree(paramText);
    for (size_t k = 0; k < top.len; ++k) artfVecPush(&out, top.items[k]);
    for (size_t k = 0; k < s.body.len; ++k) artfVecPush(&out, s.body.items[k]);
    say(&out, "}\n");

    char* result = NULL;
    if (!s.failed) {
        result = artfMalloc(head.len + out.len + 1);
        memcpy(result, head.items, head.len);
        memcpy(result + head.len, out.items, out.len);
        result[head.len + out.len] = 0;
        if (why && size) why[0] = 0;
    } else if (why && size) {
        snprintf(why, size, "%s", s.why);
    }
    for (size_t k = 0; k < ids; ++k) {
        artfFree(s.types[k]);
        artfFree(s.expr[k]);
        artfFree(s.sampler[k]);
    }
    artfFree(s.facts);
    artfFree(s.types);
    artfFree(s.expr);
    artfFree(s.sampler);
    artfFree(s.root);
    artfFree(s.labelAt);
    artfVecFree(&code);
    artfFree(used);
    artfVecFree(&s.body);
    artfVecFree(&head);
    artfVecFree(&out);
    artfVecFree(&in);
    artfVecFree(&outs);
    artfVecFree(&top);
    return result;
}
