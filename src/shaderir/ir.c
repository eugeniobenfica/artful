/* artful — shaders as an IR (ir.h): instructions with their kinds, what reads them, and SPIR-V's words from them. */
#include "shaderir/ir.h"

#include <string.h>

void irInit(IrModule* m) { memset(m, 0, sizeof *m); }

void irFree(IrModule* m) {
    artfVecFree(&m->insts);
    artfVecFree(&m->words);
    artfVecFree(&m->kinds);
    artfVecFree(&m->defs);
    memset(m, 0, sizeof *m);
}

IrId irNewId(IrModule* m) { return ++m->bound; }

/* ---- the grammar, for the operations shaders here use (and their kin): shape and operands' kinds ---- */

static uint8_t shapeOf(uint32_t op) {
    switch (op) {
    /* neither type nor result */
    case 10: case 14: case 15: case 16: case 17: case 56: case 62: case 71: case 72: case 224: case 225: case 246: case 247: case 249: case 250:
    case 251: case 252: case 253: case 254: case 255: case 4473: case 4478:
        return 0;
    /* a result alone: an import, the types, a label */
    case 11: case 248: case 4472: case 5341:
        return IR_HAS_RESULT;
    default:
        return op >= 19 && op <= 39 ? IR_HAS_RESULT : IR_HAS_TYPE | IR_HAS_RESULT;
    }
}

/* (a string's words end at the first that holds a 0 byte) */
static uint32_t stringWords(const uint32_t* w, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        if (!(w[i] & 0xFF) || !(w[i] & 0xFF00) || !(w[i] & 0xFF0000) || !(w[i] & 0xFF000000u)) return i + 1;
    return n;
}

static void kindsOf(uint32_t op, const uint32_t* w, uint32_t n, uint8_t* k) {
    for (uint32_t i = 0; i < n; ++i) k[i] = IR_OPERAND_ID;
#define FROM(first, kind) for (uint32_t i = (first); i < n; ++i) k[i] = (kind)
#define AT(i, kind) if ((i) < n) k[i] = (kind)
    switch (op) {
    case 10: case 11: FROM(0, IR_OPERAND_STRING); break;           /* Extension, ExtInstImport */
    case 12: AT(1, IR_OPERAND_LITERAL); break;                      /* ExtInst: set, instruction, operands */
    case 14: case 17: case 21: case 22: case 43: FROM(0, IR_OPERAND_LITERAL); break;
    case 15: {                                                      /* EntryPoint: model, function, name, interface */
        AT(0, IR_OPERAND_LITERAL);
        const uint32_t words = n > 2 ? stringWords(w + 2, n - 2) : 0;
        for (uint32_t i = 2; i < 2 + words; ++i) k[i] = IR_OPERAND_STRING;
        break;
    }
    case 16: case 25: case 71: case 72: case 81: FROM(1, IR_OPERAND_LITERAL); break;
    case 23: AT(1, IR_OPERAND_LITERAL); break;                      /* TypeVector: component, count */
    case 32: case 54: case 59: AT(0, IR_OPERAND_LITERAL); break;    /* TypePointer, Function, Variable: storage or control first */
    case 79: case 82: case 246: FROM(2, IR_OPERAND_LITERAL); break; /* VectorShuffle, CompositeInsert, LoopMerge */
    case 247: AT(1, IR_OPERAND_LITERAL); break;                     /* SelectionMerge */
    case 250: FROM(3, IR_OPERAND_LITERAL); break;                   /* BranchConditional: its weights */
    case 87: case 88: case 89: case 90: case 95: case 96: case 98:  /* images: image, coordinates, the operands' mask, ids */
        AT(2, IR_OPERAND_LITERAL);
        break;
    default: break;
    }
#undef FROM
#undef AT
}

uint32_t irAppend(IrModule* m, int section, uint32_t op, const uint32_t* words, uint32_t n) {
    IrInst i;
    memset(&i, 0, sizeof i);
    i.op = (uint16_t)op;
    i.section = (uint8_t)section;
    i.shape = shapeOf(op);
    uint32_t used = 0;
    if ((i.shape & IR_HAS_TYPE) && used < n) i.type = words[used++];
    if ((i.shape & IR_HAS_RESULT) && used < n) i.result = words[used++];
    i.at = (uint32_t)m->words.len;
    i.count = n - used;
    for (uint32_t k = used; k < n; ++k) artfVecPush(&m->words, words[k]);
    for (uint32_t k = 0; k < i.count; ++k) artfVecPush(&m->kinds, (uint8_t)IR_OPERAND_ID);
    kindsOf(op, m->words.items + i.at, i.count, m->kinds.items + i.at);
    const uint32_t index = (uint32_t)m->insts.len;
    artfVecPush(&m->insts, i);
    if (i.result) {
        while (m->defs.len <= i.result) artfVecPush(&m->defs, 0u);
        if (!m->defs.items[i.result]) m->defs.items[i.result] = index + 1;
    }
    if (i.result > m->bound) m->bound = i.result;
    return index;
}

const IrInst* irDef(const IrModule* m, IrId id) {
    return id < m->defs.len && m->defs.items[id] ? &m->insts.items[m->defs.items[id] - 1] : NULL;
}

IrId irTypeOf(const IrModule* m, IrId id) {
    const IrInst* i = irDef(m, id);
    return i ? i->type : 0;
}

/* ---- what is wrong with a module ---- */

static bool isType(const IrInst* i) { return i && ((i->op >= 19 && i->op <= 39) || i->op == 4472 || i->op == 5341); }
static bool ends(uint32_t op) { return op == 249 || op == 250 || op == 251 || op == 252 || op == 253 || op == 254 || op == 255; }

bool irCheck(const IrModule* m, char* why, size_t size) {
#define WRONG(...) do { if (why && size) snprintf(why, size, __VA_ARGS__); return false; } while (0)
    uint8_t* made = artfCalloc((size_t)m->bound + 2, 1);
    for (size_t k = 0; k < m->insts.len; ++k) {
        const IrInst* i = &m->insts.items[k];
        if (!i->result) continue;
        if (i->result > m->bound) { artfFree(made); WRONG("instruction %zu makes %%%u, past the bound %u", k, i->result, m->bound); }
        if (made[i->result]) { artfFree(made); WRONG("%%%u is made twice (instruction %zu, Op%u)", i->result, k, i->op); }
        made[i->result] = 1;
    }
    bool inFunction = false, inBlock = false;
    for (size_t k = 0; k < m->insts.len; ++k) {
        const IrInst* i = &m->insts.items[k];
        if (i->type && !isType(irDef(m, i->type))) { artfFree(made); WRONG("instruction %zu (Op%u): its type %%%u is not a type", k, i->op, i->type); }
        const uint32_t* w = irOperands(m, i);
        const uint8_t* kind = irKinds(m, i);
        for (uint32_t o = 0; o < i->count; ++o)
            if (kind[o] == IR_OPERAND_ID && (w[o] > m->bound || !made[w[o]])) { artfFree(made); WRONG("instruction %zu (Op%u): operand %u, %%%u, is made by nothing", k, i->op, o, w[o]); }
        if (i->section != IR_CODE) continue;
        if (i->op == 54) { inFunction = true; inBlock = false; continue; }
        if (!inFunction) { artfFree(made); WRONG("instruction %zu (Op%u) is code outside a function", k, i->op); }
        if (i->op == 56) {
            if (inBlock) { artfFree(made); WRONG("a function ends inside a block (instruction %zu)", k); }
            inFunction = false;
            continue;
        }
        if (i->op == 248) {
            if (inBlock) { artfFree(made); WRONG("block %%%u starts before the one before it ended (instruction %zu)", i->result, k); }
            inBlock = true;
            continue;
        }
        if (!inBlock && i->op != 59) { artfFree(made); WRONG("instruction %zu (Op%u) is in no block", k, i->op); }
        if (ends(i->op)) inBlock = false;
    }
    artfFree(made);
    if (inFunction) WRONG("a function does not end");
    if (why && size) why[0] = 0;
    return true;
#undef WRONG
}

void irPrint(const IrModule* m, FILE* out) {
    static const char* const names[IR_SECTIONS] = {"head", "annotations", "types", "code"};
    for (int section = 0; section < IR_SECTIONS; ++section) {
        fprintf(out, "; %s\n", names[section]);
        for (size_t k = 0; k < m->insts.len; ++k) {
            const IrInst* i = &m->insts.items[k];
            if (i->section != section) continue;
            if (i->result) fprintf(out, "%%%u = ", i->result);
            fprintf(out, "Op%u", i->op);
            if (i->type) fprintf(out, " %%%u", i->type);
            const uint32_t* w = irOperands(m, i);
            const uint8_t* kind = irKinds(m, i);
            for (uint32_t o = 0; o < i->count;) {
                if (kind[o] == IR_OPERAND_STRING) {
                    fputs(" \"", out);
                    for (; o < i->count && kind[o] == IR_OPERAND_STRING; ++o)
                        for (int b = 0; b < 32; b += 8)
                            if (w[o] >> b & 0xFF) fputc((int)(w[o] >> b & 0xFF), out);
                    fputc('"', out);
                    continue;
                }
                fprintf(out, kind[o] == IR_OPERAND_ID ? " %%%u" : " %u", w[o]);
                ++o;
            }
            fputc('\n', out);
        }
    }
}

bool irFromSpirv(IrModule* m, const uint32_t* words, size_t count) {
    if (count < 5 || words[0] != 0x07230203) return false;
    bool code = false;
    for (size_t at = 5; at < count;) {
        const uint32_t length = words[at] >> 16, op = words[at] & 0xFFFF;
        if (!length || at + length > count) return false;
        int section;
        if (op == 54) code = true; /* (OpFunction: code from here on) */
        if (op == 17 || op == 10 || op == 11 || op == 14 || op == 15 || op == 16) section = IR_HEAD;
        else if (op == 71 || op == 72 || (op >= 5 && op <= 7)) section = IR_ANNOTATIONS; /* (decorations, and names) */
        else section = code ? IR_CODE : IR_TYPES;
        irAppend(m, section, op, words + at + 1, length - 1);
        at += length;
    }
    if (words[3] > m->bound + 1) m->bound = words[3] - 1;
    return true;
}

int irUses(const IrModule* m, IrUse* out, int max) {
    const size_t ids = (size_t)m->bound + 1;
    uint32_t* root = artfCalloc(ids, 4);   /* of a pointer: the variable it is into, + 1 */
    uint32_t* set = artfCalloc(ids, 4), *binding = artfCalloc(ids, 4);
    uint8_t* bound = artfCalloc(ids, 1), *how = artfCalloc(ids, 1); /* (how: 1 read, 2 written) */
    for (size_t k = 0; k < m->insts.len; ++k) {
        const IrInst* i = &m->insts.items[k];
        const uint32_t* w = irOperands(m, i);
        if (i->op != 71 || i->count < 3 || w[0] >= ids) continue;
        if (w[1] == 33) { binding[w[0]] = w[2]; bound[w[0]] = 1; root[w[0]] = w[0] + 1; }
        if (w[1] == 34) set[w[0]] = w[2];
    }
    for (size_t k = 0; k < m->insts.len; ++k) {
        const IrInst* i = &m->insts.items[k];
        const uint32_t* w = irOperands(m, i);
        if (i->section != IR_CODE || !i->count || w[0] >= ids || !root[w[0]]) continue;
        const uint32_t variable = root[w[0]] - 1;
        if (i->op == 65 && i->result < ids) root[i->result] = root[w[0]]; /* (a chain into it) */
        else if (i->op == 61) how[variable] |= 1;
        else if (i->op == 62) how[variable] |= 2;
        else if (i->op >= 227 && i->op <= 242) how[variable] |= 3; /* (atomics) */
    }
    int n = 0;
    for (size_t v = 0; v < ids; ++v) {
        if (!bound[v] || !how[v]) continue;
        if (n < max) {
            const IrUse u = {set[v], binding[v], (how[v] & 1) != 0, (how[v] & 2) != 0};
            out[n] = u;
        }
        ++n;
    }
    artfFree(root);
    artfFree(set);
    artfFree(binding);
    artfFree(bound);
    artfFree(how);
    return n;
}

uint32_t* irSpirv(const IrModule* m, size_t* wordCount) {
    size_t total = 5;
    for (size_t k = 0; k < m->insts.len; ++k) {
        const IrInst* i = &m->insts.items[k];
        total += 1 + (i->shape & IR_HAS_TYPE ? 1 : 0) + (i->shape & IR_HAS_RESULT ? 1 : 0) + i->count;
    }
    uint32_t* out = artfMalloc(total * 4);
    out[0] = 0x07230203;
    out[1] = 0x00010300; /* SPIR-V 1.3: Vulkan 1.1 and up, StorageBuffer without an extension */
    out[2] = 0;
    out[3] = m->bound + 1;
    out[4] = 0;
    size_t at = 5;
    for (int section = 0; section < IR_SECTIONS; ++section)
        for (size_t k = 0; k < m->insts.len; ++k) {
            const IrInst* i = &m->insts.items[k];
            if (i->section != section) continue;
            const uint32_t length = 1 + (i->shape & IR_HAS_TYPE ? 1u : 0u) + (i->shape & IR_HAS_RESULT ? 1u : 0u) + i->count;
            out[at++] = length << 16 | i->op;
            if (i->shape & IR_HAS_TYPE) out[at++] = i->type;
            if (i->shape & IR_HAS_RESULT) out[at++] = i->result;
            if (i->count) memcpy(out + at, irOperands(m, i), (size_t)i->count * 4);
            at += i->count;
        }
    *wordCount = total;
    return out;
}
