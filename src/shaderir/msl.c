/* artful — Metal's shading language from a shader's IR (msl.h). */
#include "shaderir/msl.h"

#include <stdarg.h>
#include <string.h>

/* ---- text ---- */

typedef ARTF_VEC(char) Text;

static void say(Text* t, const char* format, ...) {
    char line[1024];
    va_list a;
    va_start(a, format);
    int n = vsnprintf(line, sizeof line, format, a);
    va_end(a);
    if (n < 0) return;
    if ((size_t)n >= sizeof line) {
        char* big = artfMalloc((size_t)n + 1);
        va_start(a, format);
        vsnprintf(big, (size_t)n + 1, format, a);
        va_end(a);
        for (int i = 0; i < n; ++i) artfVecPush(t, big[i]);
        artfFree(big);
        return;
    }
    for (int i = 0; i < n; ++i) artfVecPush(t, line[i]);
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

/* ---- what the module says of its ids ---- */

enum { NO_VALUE = 0xFFFFFFFFu };

typedef struct Facts {
    uint32_t set, binding, location, builtin, arrayStride;
    bool flat, block, nonWritable; /* (nonWritable: of a struct's first member) */
    uint32_t members[4];  /* of a struct: the builtin each of its first members is (gl_PerVertex's) */
    bool perVertex;       /* of a variable going out: a struct of builtins, each its own in main0_out */
    bool invariant, invariantMember; /* (of a struct: a member is) */
} Facts;

typedef struct Loop { IrId header, merge, cont; } Loop;

typedef struct Msl {
    const IrModule* m;
    const IrMslOptions* o;
    uint32_t model; /* 0 vertex, 4 fragment, 5 compute */
    Facts* facts;
    char** types;   /* each type's name */
    char** expr;    /* what a value is written as where it is used; NULL: its variable, _id */
    char** sampler; /* of a sampled image: its sampler */
    uint32_t* labelAt; /* of a label, its OpLabel's place in code, + 1 */
    uint32_t* code;    /* the code's instructions, in order (others come between them: constants made as it was built) */
    uint32_t codeCount;
    bool hasOut; /* main0_out is returned */
    Text body;
    int indent;
    Loop loops[16];
    int loopCount;
    bool failed;
    char why[256];
} Msl;

static void fail(Msl* s, const char* format_, ...) {
    if (s->failed) return;
    s->failed = true;
    va_list a;
    va_start(a, format_);
    vsnprintf(s->why, sizeof s->why, format_, a);
    va_end(a);
}

static const IrInst* def(const Msl* s, IrId id) { return irDef(s->m, id); }
static const uint32_t* ops(const Msl* s, const IrInst* i) { return irOperands(s->m, i); }

static uint32_t constantValue(const Msl* s, IrId id) {
    const IrInst* c = def(s, id);
    return c && c->op == 43 && c->count ? ops(s, c)[0] : 0;
}

/* ---- types ---- */

static const char* typeName(Msl* s, IrId id) {
    if (id < s->m->bound + 1 && s->types[id]) return s->types[id];
    const IrInst* t = def(s, id);
    if (!t) {
        fail(s, "%%%u is no type", id);
        return "void";
    }
    const uint32_t* w = ops(s, t);
    char* name = NULL;
    switch (t->op) {
    case 19: name = copyOf("void"); break;
    case 20: name = copyOf("bool"); break;
    case 21: name = copyOf(w[1] ? "int" : "uint"); break;
    case 22: name = copyOf("float"); break;
    case 23: name = format("%s%u", typeName(s, w[0]), w[1]); break;
    case 28: name = format("array<%s, %u>", typeName(s, w[0]), constantValue(s, w[1])); break;
    case 29: name = copyOf(typeName(s, w[0])); break; /* (a runtime array: only as a struct's last member, [1]) */
    case 30: name = format("_%u", id); break;
    case 25: {
        /* sampled type, dim, depth, arrayed, multisampled, sampled, format */
        const char* of = typeName(s, w[0]);
        name = w[4] ? format("texture2d_ms<%s>", of) : format("texture2d<%s>", of);
        break;
    }
    case 27: name = copyOf(typeName(s, w[0])); break;
    case 32: name = copyOf(typeName(s, w[1])); break;
    default:
        if (t->op == 4472 || t->op == 5341) fail(s, "ray queries (Op%u): not written for Metal", t->op);
        else fail(s, "a type Metal is not written for here: Op%u", t->op);
        name = copyOf("void");
        break;
    }
    s->types[id] = name;
    return name;
}

/* What a pointer points at. */
static IrId pointee(const Msl* s, IrId pointerType) {
    const IrInst* t = def(s, pointerType);
    return t && t->op == 32 ? ops(s, t)[1] : 0;
}
static uint32_t storageOf(const Msl* s, IrId pointerType) {
    const IrInst* t = def(s, pointerType);
    return t && t->op == 32 ? ops(s, t)[0] : NO_VALUE;
}

/* Metal's size and alignment, for checking a struct's offsets against Vulkan's. */
static void sizeOf(Msl* s, IrId type, uint32_t* size, uint32_t* align) {
    const IrInst* t = def(s, type);
    *size = *align = 4;
    if (!t) return;
    const uint32_t* w = ops(s, t);
    switch (t->op) {
    case 23: {
        uint32_t es, ea;
        sizeOf(s, w[0], &es, &ea);
        const uint32_t n = w[1] == 3 ? 4 : w[1];
        *size = *align = es * n;
        break;
    }
    case 28: case 29: {
        uint32_t es, ea;
        sizeOf(s, w[0], &es, &ea);
        *size = es * (t->op == 28 ? constantValue(s, w[1]) : 1);
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

static void declareStructs(Msl* s, Text* out) {
    for (size_t k = 0; k < s->m->insts.len; ++k) {
        const IrInst* t = &s->m->insts.items[k];
        if (t->section != IR_TYPES || t->op != 30) continue;
        const uint32_t* w = ops(s, t);
        say(out, "struct _%u\n{\n", t->result);
        uint32_t at = 0;
        for (uint32_t i = 0; i < t->count; ++i) {
            const IrInst* member = def(s, w[i]);
            /* Vulkan's offset, from the annotations; Metal's, by its own rules: they must be one */
            uint32_t offset = NO_VALUE;
            for (size_t a = 0; a < s->m->insts.len; ++a) {
                const IrInst* d = &s->m->insts.items[a];
                if (d->op == 72 && d->count >= 4 && ops(s, d)[0] == t->result && ops(s, d)[1] == i && ops(s, d)[2] == 35) offset = ops(s, d)[3];
            }
            uint32_t ms, ma;
            sizeOf(s, w[i], &ms, &ma);
            at = (at + ma - 1) / ma * ma;
            if (offset != NO_VALUE && offset > at) {
                /* (further on in Vulkan: bytes before it, as SPIRV-Cross pads) */
                say(out, "    char m%u_pad[%u];\n", i, offset - at);
                at = offset;
            }
            if (offset != NO_VALUE && offset != at) fail(s, "member %u of struct %%%u is at %u in Vulkan, %u in Metal", i, t->result, offset, at);
            if (member && (member->op == 28 || member->op == 29)) {
                const IrId element = ops(s, member)[0];
                uint32_t es, ea;
                sizeOf(s, element, &es, &ea);
                const uint32_t stride = s->facts[w[i]].arrayStride;
                if (stride && stride != es) fail(s, "an array of stride %u, where Metal's elements are %u apart", stride, es);
                say(out, "    %s m%u[%u];\n", typeName(s, element), i, member->op == 28 ? constantValue(s, ops(s, member)[1]) : 1u);
            } else {
                say(out, "    %s m%u;\n", typeName(s, w[i]), i);
            }
            at += ms;
        }
        say(out, "};\n\n");
    }
}

/* ---- values ---- */

static const char* valueOf(Msl* s, IrId id) {
    if (id <= s->m->bound && s->expr[id]) return s->expr[id];
    if (id > s->m->bound) return "?";
    s->expr[id] = format("_%u", id);
    return s->expr[id];
}

static char* floatText(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    if (f != f || f - f != 0) return format("as_type<float>(0x%08Xu)", bits); /* (NaN and infinities) */
    char text[48];
    snprintf(text, sizeof text, "%.9g", (double)f);
    if (!strpbrk(text, ".eE")) strcat(text, ".0");
    return format("%sf", text);
}

/* Constants, written where they are used. */
static void constants(Msl* s) {
    for (size_t k = 0; k < s->m->insts.len; ++k) {
        const IrInst* c = &s->m->insts.items[k];
        if (c->section != IR_TYPES) continue;
        const uint32_t* w = ops(s, c);
        if (c->op == 41 || c->op == 42) s->expr[c->result] = copyOf(c->op == 41 ? "true" : "false");
        else if (c->op == 43) {
            const IrInst* t = def(s, c->type);
            if (t && t->op == 22) s->expr[c->result] = floatText(w[0]);
            else if (t && t->op == 21 && ops(s, t)[1]) s->expr[c->result] = (int32_t)w[0] == INT32_MIN ? copyOf("(-2147483647 - 1)") : format("%d", (int32_t)w[0]);
            else s->expr[c->result] = format("%uu", w[0]);
        } else if (c->op == 44) {
            /* (an array or a struct by its members, in braces; a vector by its constructor) */
            const IrInst* type = def(s, c->type);
            const bool braces = type && (type->op == 28 || type->op == 30);
            Text t = {0};
            say(&t, "%s%s", typeName(s, c->type), braces ? "{" : "(");
            for (uint32_t i = 0; i < c->count; ++i) say(&t, "%s%s", i ? ", " : "", valueOf(s, w[i]));
            say(&t, braces ? "}" : ")");
            artfVecPush(&t, '\0');
            s->expr[c->result] = copyOf(t.items);
            artfVecFree(&t);
        }
    }
}

/* ---- the code ---- */

static void line(Msl* s, const char* format_, ...) {
    for (int i = 0; i < s->indent; ++i) say(&s->body, "    ");
    char text[2048];
    va_list a;
    va_start(a, format_);
    vsnprintf(text, sizeof text, format_, a);
    va_end(a);
    say(&s->body, "%s\n", text);
}

static const char* binaryOf(uint32_t op) {
    switch (op) {
    case 128: case 129: return "+";
    case 130: case 131: return "-";
    case 132: case 133: case 142: return "*";
    case 134: case 135: case 136: return "/";
    case 137: case 139: return "%";
    case 164: case 170: case 180: return "==";
    case 165: case 171: case 182: return "!=";
    case 166: return "||";
    case 167: return "&&";
    case 172: case 173: case 186: return ">";
    case 174: case 175: case 190: return ">=";
    case 176: case 177: case 184: return "<";
    case 178: case 179: case 188: return "<=";
    case 194: case 195: return ">>";
    case 196: return "<<";
    case 197: return "|";
    case 198: return "^";
    case 199: return "&";
    default: return NULL;
    }
}

static const char* glslOf(uint32_t inst, int* args) {
    *args = 1;
    switch (inst) {
    case 4: return "abs";
    case 8: return "floor";
    case 9: return "ceil";
    case 10: return "fract";
    case 13: return "sin";
    case 14: return "cos";
    case 25: *args = 2; return "atan2";
    case 26: *args = 2; return "pow";
    case 27: return "exp";
    case 28: return "log";
    case 29: return "exp2";
    case 30: return "log2";
    case 31: return "sqrt";
    case 32: return "rsqrt";
    case 37: *args = 2; return "fmin";
    case 38: case 39: *args = 2; return "min";
    case 40: *args = 2; return "fmax";
    case 41: case 42: *args = 2; return "max";
    case 43: case 44: case 45: *args = 3; return "clamp";
    case 46: *args = 3; return "mix";
    case 50: *args = 3; return "fma";
    case 58: return "spvPackHalf2x16";
    case 62: return "spvUnpackHalf2x16";
    case 64: return "unpack_unorm4x8_to_float";
    case 66: return "length";
    case 67: *args = 2; return "distance";
    case 68: *args = 2; return "cross";
    case 69: return "normalize";
    default: return NULL;
    }
}

/* A pointer's expression, an index into it at a time. */
static char* chainOf(Msl* s, const IrInst* i) {
    const uint32_t* w = ops(s, i);
    IrId type = pointee(s, irTypeOf(s->m, w[0]));
    if (!type) {
        /* (a global variable: its pointer type is its instruction's) */
        const IrInst* base = def(s, w[0]);
        type = base ? pointee(s, base->type) : 0;
    }
    if (w[0] <= s->m->bound && s->facts[w[0]].builtin == 20) return copyOf(valueOf(s, w[0]));
    Text t = {0};
    say(&t, "%s", valueOf(s, w[0]));
    uint32_t from = 1;
    if (w[0] <= s->m->bound && s->facts[w[0]].perVertex && i->count > 1) {
        /* (gl_PerVertex's member: the builtin out's own) */
        const IrInst* block = def(s, type);
        const uint32_t member = constantValue(s, w[1]);
        const uint32_t builtin = member < 4 ? s->facts[type].members[member] : NO_VALUE;
        say(&t, ".%s", builtin == 0 ? "gl_Position" : "gl_unused");
        type = block && member < block->count ? ops(s, block)[member] : 0;
        from = 2;
    }
    for (uint32_t k = from; k < i->count; ++k) {
        const IrInst* at = def(s, type);
        if (!at) break;
        if (at->op == 30) {
            const uint32_t member = constantValue(s, w[k]);
            say(&t, ".m%u", member);
            type = ops(s, at)[member];
        } else if (at->op == 28 || at->op == 29) {
            say(&t, "[%s]", valueOf(s, w[k]));
            type = ops(s, at)[0];
        } else if (at->op == 23) {
            say(&t, "[%s]", valueOf(s, w[k]));
            type = ops(s, at)[0];
        }
    }
    artfVecPush(&t, '\0');
    char* out = copyOf(t.items);
    artfVecFree(&t);
    return out;
}

static void phisFrom(Msl* s, IrId block) {
    for (size_t k = 0; k < s->m->insts.len; ++k) {
        const IrInst* p = &s->m->insts.items[k];
        if (p->op != 245) continue;
        const uint32_t* w = ops(s, p);
        for (uint32_t i = 0; i + 1 < p->count; i += 2)
            if (w[i + 1] == block) line(s, "_%u = %s;", p->result, valueOf(s, w[i]));
    }
}

static void instruction(Msl* s, const IrInst* i) {
    const uint32_t* w = ops(s, i);
    const IrId r = i->result;
    const char* binary = binaryOf(i->op);
    if (binary) {
        line(s, "_%u = %s %s %s;", r, valueOf(s, w[0]), binary, valueOf(s, w[1]));
        return;
    }
    switch (i->op) {
    case 59: case 246: case 247: return; /* (variables are declared at the top; merges are read by blocks) */
    case 61: {
        const IrInst* t = def(s, i->type);
        if (t && (t->op == 27 || t->op == 25)) {
            /* a texture: what the load names, written where it is used */
            s->expr[r] = copyOf(valueOf(s, w[0]));
            if (s->sampler[w[0]]) s->sampler[r] = copyOf(s->sampler[w[0]]);
            return;
        }
        line(s, "_%u = %s;", r, valueOf(s, w[0]));
        return;
    }
    case 62: line(s, "%s = %s;", valueOf(s, w[0]), valueOf(s, w[1])); return;
    case 65: {
        s->expr[r] = chainOf(s, i);
        /* (into an array of textures: its sampler too) */
        if (s->sampler[w[0]]) s->sampler[r] = i->count > 1 ? format("%s[%s]", s->sampler[w[0]], valueOf(s, w[1])) : copyOf(s->sampler[w[0]]);
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
        artfVecPush(&t, '\0');
        line(s, "_%u = %s(%s);", r, typeName(s, i->type), t.items);
        artfVecFree(&t);
        return;
    }
    case 80: {
        Text t = {0};
        for (uint32_t k = 0; k < i->count; ++k) say(&t, "%s%s", k ? ", " : "", valueOf(s, w[k]));
        artfVecPush(&t, '\0');
        line(s, "_%u = %s(%s);", r, typeName(s, i->type), t.items);
        artfVecFree(&t);
        return;
    }
    case 81: {
        IrId type = irTypeOf(s->m, w[0]);
        Text t = {0};
        say(&t, "%s", valueOf(s, w[0]));
        for (uint32_t k = 1; k < i->count; ++k) {
            const IrInst* at = def(s, type);
            if (!at) break;
            if (at->op == 23) { say(&t, ".%c", "xyzw"[w[k] & 3]); type = ops(s, at)[0]; }
            else if (at->op == 30) { say(&t, ".m%u", w[k]); type = ops(s, at)[w[k]]; }
            else { say(&t, "[%u]", w[k]); type = ops(s, at)[0]; }
        }
        artfVecPush(&t, '\0');
        line(s, "_%u = %s;", r, t.items);
        artfVecFree(&t);
        return;
    }
    case 12: {
        int args;
        /* (debugging information, NonSemantic's: nothing in Metal) */
        const IrInst* set = irDef(s->m, w[0]);
        if (set && set->op == 11 && irOperands(s->m, set)[0] == 0x536E6F4Eu /* "NonS" */) return;
        const char* f = glslOf(w[1], &args);
        if (w[1] == 75) { /* FindUMsb: -1 for 0, as clz gives 32 */
            line(s, "_%u = %s(31 - int(clz(%s)));", r, typeName(s, i->type), valueOf(s, w[2]));
            return;
        }
        if (!f) { fail(s, "GLSL.std.450's instruction %u", w[1]); return; }
        if (args == 1) line(s, "_%u = %s(%s);", r, f, valueOf(s, w[2]));
        else if (args == 2) line(s, "_%u = %s(%s, %s);", r, f, valueOf(s, w[2]), valueOf(s, w[3]));
        else line(s, "_%u = %s(%s, %s, %s);", r, f, valueOf(s, w[2]), valueOf(s, w[3]), valueOf(s, w[4]));
        return;
    }
    case 87: case 88: {
        const char* smp = s->sampler[w[0]];
        if (!smp) { fail(s, "a sample with no sampler (%%%u)", w[0]); return; }
        if (i->op == 87) line(s, "_%u = %s.sample(%s, %s);", r, valueOf(s, w[0]), smp, valueOf(s, w[1]));
        else if (i->count > 4 && w[2] == 4) /* Grad */
            line(s, "_%u = %s.sample(%s, %s, gradient2d(%s, %s));", r, valueOf(s, w[0]), smp, valueOf(s, w[1]), valueOf(s, w[3]), valueOf(s, w[4]));
        else line(s, "_%u = %s.sample(%s, %s, level(%s));", r, valueOf(s, w[0]), smp, valueOf(s, w[1]), i->count > 3 ? valueOf(s, w[3]) : "0.0f");
        return;
    }
    case 100: s->expr[r] = copyOf(valueOf(s, w[0])); return;
    case 95: {
        const uint32_t mask = i->count > 2 ? w[2] : 0;
        line(s, "_%u = %s(%s.read(uint2(%s), uint(%s)));", r, typeName(s, i->type), valueOf(s, w[0]), valueOf(s, w[1]), i->count > 3 ? valueOf(s, w[3]) : "0u");
        (void)mask; /* (Lod or Sample: both the second argument of read) */
        return;
    }
    case 103: line(s, "_%u = %s(int(%s.get_width(uint(%s))), int(%s.get_height(uint(%s))));", r, typeName(s, i->type), valueOf(s, w[0]), valueOf(s, w[1]), valueOf(s, w[0]), valueOf(s, w[1])); return;
    case 106: line(s, "_%u = %s(%s.get_num_mip_levels());", r, typeName(s, i->type), valueOf(s, w[0])); return;
    case 109: case 110: case 111: case 112: case 113: case 114: line(s, "_%u = %s(%s);", r, typeName(s, i->type), valueOf(s, w[0])); return;
    case 124: line(s, "_%u = as_type<%s>(%s);", r, typeName(s, i->type), valueOf(s, w[0])); return;
    case 126: case 127: line(s, "_%u = -%s;", r, valueOf(s, w[0])); return;
    case 148: line(s, "_%u = dot(%s, %s);", r, valueOf(s, w[0]), valueOf(s, w[1])); return;
    case 168: line(s, "_%u = !%s;", r, valueOf(s, w[0])); return;
    case 169: {
        const IrInst* c = def(s, irTypeOf(s->m, w[0]));
        if (c && c->op == 23) line(s, "_%u = select(%s, %s, %s);", r, valueOf(s, w[2]), valueOf(s, w[1]), valueOf(s, w[0]));
        else line(s, "_%u = %s ? %s : %s;", r, valueOf(s, w[0]), valueOf(s, w[1]), valueOf(s, w[2]));
        return;
    }
    case 200: line(s, "_%u = ~%s;", r, valueOf(s, w[0])); return;
    case 204: line(s, "_%u = reverse_bits(%s);", r, valueOf(s, w[0])); return;
    case 205: line(s, "_%u = %s(popcount(%s));", r, typeName(s, i->type), valueOf(s, w[0])); return;
    case 207: line(s, "_%u = dfdx(%s);", r, valueOf(s, w[0])); return;
    case 208: line(s, "_%u = dfdy(%s);", r, valueOf(s, w[0])); return;
    case 209: line(s, "_%u = fwidth(%s);", r, valueOf(s, w[0])); return;
    case 224: case 225: line(s, "threadgroup_barrier(mem_flags::mem_device | mem_flags::mem_threadgroup);"); return;
    case 234: {
        /* (in the group's memory or a buffer's, as the pointer is: Apple's compiler takes no cast across the two) */
        const bool group = storageOf(s, irTypeOf(s->m, w[0])) == 4;
        line(s, "_%u = atomic_fetch_add_explicit((%s atomic_uint*)&%s, %s, memory_order_relaxed);", r, group ? "threadgroup" : "device", valueOf(s, w[0]), valueOf(s, w[3]));
        return;
    }
    case 245: return; /* (set by the edges into its block) */
    default:
        if (i->op >= 4472 || i->op == 5341) fail(s, "ray queries (Op%u): not written for Metal", i->op);
        else fail(s, "Op%u: not written for Metal", i->op);
        return;
    }
}

static void blockFrom(Msl* s, IrId label, IrId stop);

/* Control going to target: out of a loop, around it again, or on in the same block of code. true: it is taken care of. */
static bool jump(Msl* s, IrId target) {
    for (int k = s->loopCount - 1; k >= 0; --k) {
        const Loop* l = &s->loops[k];
        if (target != l->merge && target != l->header && target != l->cont) continue;
        if (k != s->loopCount - 1) { fail(s, "a branch out of more than one loop"); return true; }
        if (target == l->merge) line(s, "break;");
        else if (target == l->header) line(s, "continue;");
        else blockFrom(s, l->cont, 0); /* (its end goes back to the header: continue) */
        return true;
    }
    return false;
}

/* The code from a block on, to stop (a merge the caller writes after), or to the end of a path. */
static void blockFrom(Msl* s, IrId label, IrId stop) {
    for (int guard = 0; label && label != stop && guard < 100000 && !s->failed; ++guard) {
        if (label > s->m->bound || !s->labelAt[label]) { fail(s, "a branch to %%%u, which is no block", label); return; }
#define CODE(k) (&s->m->insts.items[s->code[k]])
        const uint32_t at = s->labelAt[label] - 1;
        /* a loop's header: the loop around it */
        uint32_t end = at + 1;
        while (end < s->codeCount && CODE(end)->op != 249 && CODE(end)->op != 250 && CODE(end)->op != 252 && CODE(end)->op != 253) ++end;
        if (end == s->codeCount) { fail(s, "block %%%u does not end", label); return; }
        const IrInst* merge = end > at + 1 ? CODE(end - 1) : NULL;
        const bool header = merge && merge->op == 246 && !(s->loopCount && s->loops[s->loopCount - 1].header == label);
        if (header) {
            if (s->loopCount == 16) { fail(s, "loops nested past 16"); return; }
            const Loop l = {label, ops(s, merge)[0], ops(s, merge)[1]};
            s->loops[s->loopCount++] = l;
            line(s, "for (;;)");
            line(s, "{");
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
        if (t->op == 252) { line(s, "discard_fragment();"); return; }
        if (t->op == 253) {
            if (s->o && s->o->flipY && s->model == 0) line(s, "out.gl_Position.y = -out.gl_Position.y;");
            line(s, s->hasOut ? "return out;" : "return;");
            return;
        }
        if (t->op == 249) {
            if (jump(s, w[0])) return;
            label = w[0];
            continue;
        }
        /* a branch either way */
        const bool selection = merge && merge->op == 247;
        const IrId yes = w[1], no = w[2], meet = selection ? ops(s, merge)[0] : 0;
        if (selection && yes == meet) {
            line(s, "if (!(%s))", valueOf(s, w[0]));
            line(s, "{");
            ++s->indent;
            blockFrom(s, no, meet);
            --s->indent;
            line(s, "}");
        } else {
            line(s, "if (%s)", valueOf(s, w[0]));
            line(s, "{");
            ++s->indent;
            if (!(!selection && jump(s, yes))) blockFrom(s, yes, meet);
            --s->indent;
            line(s, "}");
            if (no != meet) {
                line(s, "else");
                line(s, "{");
                ++s->indent;
                if (!(!selection && jump(s, no))) blockFrom(s, no, meet);
                --s->indent;
                line(s, "}");
            }
        }
        if (!selection) return; /* (a loop's test: each way ends its path) */
        label = meet;
    }
}

/* ---- the module ---- */

typedef struct Resource { uint32_t set, binding; IrId variable; } Resource;

static const char* builtinName(uint32_t b) {
    switch (b) {
    case 0: return "gl_Position";
    case 7: return "gl_PrimitiveID";
    case 15: return "gl_FragCoord";
    case 17: return "gl_FrontFacing";
    case 18: return "gl_SampleID";
    case 20: return "gl_SampleMask";
    case 22: return "gl_FragDepth";
    case 26: return "gl_WorkGroupID";
    case 27: return "gl_LocalInvocationID";
    case 28: return "gl_GlobalInvocationID";
    case 42: return "gl_VertexIndex";
    case 43: return "gl_InstanceIndex";
    default: return NULL;
    }
}
/* Metal's type and attribute for a builtin that comes in */
static const char* builtinIn(uint32_t b, const char** type) {
    switch (b) {
    case 7: *type = "uint"; return "primitive_id";
    case 15: *type = "float4"; return "position";
    case 17: *type = "bool"; return "front_facing";
    case 18: *type = "uint"; return "sample_id";
    case 20: *type = "uint"; return "sample_mask";
    case 26: *type = "uint3"; return "threadgroup_position_in_grid";
    case 27: *type = "uint3"; return "thread_position_in_threadgroup";
    case 28: *type = "uint3"; return "thread_position_in_grid";
    case 42: *type = "uint"; return "vertex_id";
    case 43: *type = "uint"; return "instance_id";
    default: *type = NULL; return NULL;
    }
}

static void argumentSaid(IrMslArguments* a, uint32_t set, uint32_t binding, uint32_t id, uint32_t kind, uint32_t count) {
    if (!a || a->count >= IR_MSL_ARGUMENTS) return;
    const IrMslArgument e = {set, binding, id, kind, count};
    a->items[a->count++] = e;
}

char* irMsl(const IrModule* m, const IrMslOptions* options, IrMslArguments* arguments, char* why, size_t size) {
    if (arguments) {
        arguments->count = 0;
        arguments->pushAt = -1;
    }
    Msl s;
    memset(&s, 0, sizeof s);
    s.m = m;
    s.o = options;
    const size_t ids = (size_t)m->bound + 1;
    s.facts = artfCalloc(ids, sizeof *s.facts);
    s.types = artfCalloc(ids, sizeof *s.types);
    s.expr = artfCalloc(ids, sizeof *s.expr);
    s.sampler = artfCalloc(ids, sizeof *s.sampler);
    s.labelAt = artfCalloc(ids, sizeof *s.labelAt);
    ARTF_VEC(uint32_t) code = {0};
    for (size_t k = 0; k < ids; ++k) {
        s.facts[k].set = s.facts[k].binding = s.facts[k].location = s.facts[k].builtin = NO_VALUE;
        for (int b = 0; b < 4; ++b) s.facts[k].members[b] = NO_VALUE;
    }
    IrId entry = 0;
    for (size_t k = 0; k < m->insts.len; ++k) {
        const IrInst* i = &m->insts.items[k];
        const uint32_t* w = irOperands(m, i);
        if (i->op == 15) { s.model = w[0]; entry = w[1]; }
        if (i->section == IR_CODE) {
            if (i->op == 248) s.labelAt[i->result] = s.codeCount + 1;
            artfVecPush(&code, (uint32_t)k);
            ++s.codeCount;
        }
        if (i->op == 71 && i->count >= 2 && w[0] < ids) {
            Facts* f = &s.facts[w[0]];
            const uint32_t value = i->count > 2 ? w[2] : 0;
            switch (w[1]) {
            case 2: f->block = true; break;
            case 6: f->arrayStride = value; break;
            case 11: f->builtin = value; break;
            case 14: f->flat = true; break;
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
    (void)entry;
    s.code = code.items;
    constants(&s);

    Text head = {0}, out = {0};
    say(&head, "#include <metal_stdlib>\n#include <simd/simd.h>\n\nusing namespace metal;\n\n");
    /* (GLSL's half packing, as SPIRV-Cross writes it) */
    say(&head, "static inline __attribute__((always_inline)) uint spvPackHalf2x16(float2 v) { return as_type<uint>(half2(v)); }\n"
               "static inline __attribute__((always_inline)) float2 spvUnpackHalf2x16(uint v) { return float2(as_type<half2>(v)); }\n\n");
    declareStructs(&s, &head);

    /* what is global: resources, push constants, what comes in and goes out, what a group shares */
    Resource resources[96];
    int resourceCount = 0;
    IrId pushId = 0, pushType = 0;
    uint8_t* used = artfCalloc(ids, 1);
    for (uint32_t k = 0; k < s.codeCount; ++k) {
        const IrInst* i = &m->insts.items[s.code[k]];
        const uint32_t* w = irOperands(m, i);
        const uint8_t* kinds = irKinds(m, i);
        for (uint32_t o = 0; o < i->count; ++o)
            if (kinds[o] == IR_OPERAND_ID && w[o] < ids) used[w[o]] = 1;
    }
    Text in = {0}, outs = {0}, params = {0}, top = {0};
    for (size_t k = 0; k < m->insts.len; ++k) {
        const IrInst* v = &m->insts.items[k];
        if (v->op != 59 || v->section != IR_TYPES) continue;
        const IrId id = v->result, type = pointee(&s, v->type);
        const uint32_t storage = irOperands(m, v)[0];
        const Facts* f = &s.facts[id];
        switch (storage) {
        case 0: case 2: case 12:
            /* (only what the code uses, as SPIRV-Cross: what metal.c binds is what the argument buffer names) */
            if (used[id] && resourceCount < 96) resources[resourceCount++] = (Resource){f->set == NO_VALUE ? 0 : f->set, f->binding, id};
            break;
        case 9:
            pushId = id;
            pushType = type;
            s.expr[id] = format("_%u", id);
            break;
        case 1: {
            /* (a builtin the code never reads is left out, as SPIRV-Cross does: one a fragment cannot take, as the
             * post shaders' GlobalInvocationId that only their compute stages read, would not compile) */
            if (f->builtin != NO_VALUE && !used[id]) break;
            if (f->builtin != NO_VALUE) {
                const char* metalType;
                const char* attribute = builtinIn(f->builtin, &metalType);
                const char* name = builtinName(f->builtin);
                if (!attribute) { fail(&s, "builtin %u coming in", f->builtin); break; }
                say(&params, ", %s %s [[%s]]", metalType, name, attribute);
                const char* spirvType = typeName(&s, type);
                /* (SampleMask is an array of one in SPIR-V, a uint in Metal: chainOf leaves its index out) */
                s.expr[id] = strcmp(spirvType, metalType) && f->builtin != 20 ? format("%s(%s)", spirvType, name) : copyOf(name);
            } else {
                if (s.model == 0) say(&in, "    %s m_%u [[attribute(%u)]];\n", typeName(&s, type), id, f->location);
                else {
                    const char* name = typeName(&s, type);
                    const bool whole = !strncmp(name, "uint", 4) || !strncmp(name, "int", 3) || !strncmp(name, "bool", 4);
                    say(&in, "    %s m_%u [[user(locn%u)%s]];\n", name, id, f->location, f->flat && !whole ? ", flat" : "");
                }
                s.expr[id] = format("in.m_%u", id);
            }
            break;
        }
        case 3: {
            if (f->builtin != NO_VALUE) {
                const char* name = builtinName(f->builtin);
                if (f->builtin == 0) say(&outs, "    float4 gl_Position [[position%s]];\n", f->invariant ? ", invariant" : "");
                else if (f->builtin == 22) say(&outs, "    float gl_FragDepth [[depth(any)]];\n");
                else if (f->builtin == 20) say(&outs, "    uint gl_SampleMask [[sample_mask]];\n");
                else { fail(&s, "builtin %u going out", f->builtin); break; }
                s.expr[id] = format("out.%s", name);
            } else if (type < ids && s.facts[type].members[0] != NO_VALUE) {
                /* gl_PerVertex: its builtins, each its own (chainOf goes from the member to it) */
                for (int b = 0; b < 4; ++b) {
                    const uint32_t builtin = s.facts[type].members[b];
                    if (builtin == 0) say(&outs, "    float4 gl_Position [[position%s]];\n", f->invariant || s.facts[type].invariantMember ? ", invariant" : "");
                    else if (builtin != NO_VALUE && builtin != 1 && builtin != 3 && builtin != 4) fail(&s, "builtin %u of a block going out", builtin);
                }
                s.facts[id].perVertex = true;
                s.expr[id] = copyOf("out");
            } else {
                if (s.model == 4) say(&outs, "    %s m_%u [[color(%u)]];\n", typeName(&s, type), id, f->location);
                else say(&outs, "    %s m_%u [[user(locn%u)]];\n", typeName(&s, type), id, f->location);
                s.expr[id] = format("out.m_%u", id);
            }
            break;
        }
        case 4: say(&top, "    threadgroup %s v_%u;\n", typeName(&s, type), id); s.expr[id] = format("v_%u", id); break;
        case 6:
            if (v->count > 1) say(&top, "    %s v_%u = %s;\n", typeName(&s, type), id, valueOf(&s, irOperands(m, v)[1]));
            else say(&top, "    %s v_%u;\n", typeName(&s, type), id);
            s.expr[id] = format("v_%u", id);
            break;
        default: fail(&s, "a variable of storage %u", storage); break;
        }
    }
    /* an argument buffer for each descriptor set, at [[buffer(set)]]: its resources as the module declares them, a
     * texture's sampler after it, an array taking as many ids as it has; push constants after them */
    for (int a = 1; a < resourceCount; ++a)
        for (int b = a; b > 0 && resources[b - 1].set > resources[b].set; --b) {
            const Resource t = resources[b];
            resources[b] = resources[b - 1];
            resources[b - 1] = t;
        }
    Text sets = {0};
    uint32_t setCount = 0;
    for (int k = 0; k < resourceCount;) {
        const uint32_t set = resources[k].set;
        if (set > 7) { fail(&s, "descriptor set %u", set); break; }
        say(&head, "struct spvDescriptorSetBuffer%u\n{\n", set);
        say(&sets, ", constant spvDescriptorSetBuffer%u& spvDescriptorSet%u [[buffer(%u)]]", set, set, set);
        setCount = set + 1;
        uint32_t slot = 0;
        for (; k < resourceCount && resources[k].set == set; ++k) {
            const IrId id = resources[k].variable;
            const IrInst* v = def(&s, id);
            const IrId type = pointee(&s, v->type);
            const uint32_t storage = irOperands(m, v)[0], binding = resources[k].binding;
            const IrInst* t = def(&s, type);
            if (storage == 0) {
                /* textures: one, or an array; sampled ones with their samplers */
                const IrInst* element = t && t->op == 28 ? def(&s, ops(&s, t)[0]) : t;
                const bool many = t && t->op == 28;
                const uint32_t count = many ? constantValue(&s, ops(&s, t)[1]) : 1;
                const char* image = typeName(&s, element ? element->result : type);
                if (many) say(&head, "    array<%s, %u> b%u [[id(%u)]];\n", image, count, binding, slot);
                else say(&head, "    %s b%u [[id(%u)]];\n", image, binding, slot);
                argumentSaid(arguments, set, binding, slot, IR_MSL_TEXTURE, count);
                slot += count;
                s.expr[id] = format("spvDescriptorSet%u.b%u", set, binding);
                if (element && element->op == 27) {
                    if (many) say(&head, "    array<sampler, %u> b%uSmplr [[id(%u)]];\n", count, binding, slot);
                    else say(&head, "    sampler b%uSmplr [[id(%u)]];\n", binding, slot);
                    argumentSaid(arguments, set, binding, slot, IR_MSL_SAMPLER, count);
                    slot += count;
                    s.sampler[id] = format("spvDescriptorSet%u.b%uSmplr", set, binding);
                }
            } else {
                const char* space = storage == 2 ? "constant" : s.facts[type].nonWritable ? "const device" : "device";
                argumentSaid(arguments, set, binding, slot, IR_MSL_BUFFER, 1);
                say(&head, "    %s %s* b%u [[id(%u)]];\n", space, typeName(&s, type), binding, slot++);
                s.expr[id] = format("(*spvDescriptorSet%u.b%u)", set, binding);
            }
        }
        say(&head, "};\n\n");
    }
    if (pushId && used[pushId]) {
        say(&sets, ", constant %s& _%u [[buffer(%u)]]", typeName(&s, pushType), pushId, setCount ? setCount : 1u);
        if (arguments) arguments->pushAt = (int)(setCount ? setCount : 1u);
    }
    for (size_t k = 0; k < params.len; ++k) artfVecPush(&sets, params.items[k]);
    artfVecFree(&params);
    params = sets;
    if (in.len) {
        say(&head, "struct main0_in\n{\n");
        for (size_t k = 0; k < in.len; ++k) artfVecPush(&head, in.items[k]);
        say(&head, "};\n\n");
    }
    if (outs.len) {
        say(&head, "struct main0_out\n{\n");
        for (size_t k = 0; k < outs.len; ++k) artfVecPush(&head, outs.items[k]);
        say(&head, "};\n\n");
    }

    /* the function: its values declared at its top, then its blocks */
    IrId first = 0;
    for (size_t k = 0; k < m->insts.len; ++k) {
        const IrInst* i = &m->insts.items[k];
        if (i->section != IR_CODE) continue;
        if (i->op == 248 && !first) first = i->result;
        if (i->op == 59) {
            say(&top, "    %s v_%u;\n", typeName(&s, pointee(&s, i->type)), i->result);
            s.expr[i->result] = format("v_%u", i->result);
            continue;
        }
        if (!i->result || !i->type || i->op == 54 || i->op == 65 || i->op == 100) continue;
        const IrInst* t = def(&s, i->type);
        if (t && (t->op == 25 || t->op == 27)) continue; /* (textures are written where they are used) */
        say(&top, "    %s _%u;\n", typeName(&s, i->type), i->result);
    }
    s.indent = 1;
    s.hasOut = outs.len > 0;
    if (s.hasOut) line(&s, "main0_out out = {};");
    blockFrom(&s, first, 0);

    const char* kind = s.model == 5 ? "kernel" : s.model == 4 ? "fragment" : "vertex";
    const char* returns = s.model == 5 || !outs.len ? "void" : "main0_out";
    artfVecPush(&params, '\0');
    say(&out, "%s %s main0(%s%s)\n{\n", kind, returns, in.len ? "main0_in in [[stage_in]]" : "", params.items + (in.len ? 0 : (params.len > 2 ? 2 : 0)));
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
    artfFree(s.labelAt);
    artfVecFree(&code);
    artfFree(used);
    artfVecFree(&s.body);
    artfVecFree(&head);
    artfVecFree(&out);
    artfVecFree(&in);
    artfVecFree(&outs);
    artfVecFree(&params);
    artfVecFree(&top);
    return result;
}
