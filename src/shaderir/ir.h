/* artful — shaders as an IR: what a shader is, apart from the words any one API takes.
 *
 * A module is a list of instructions in SSA, in four sections (what it needs, what is said of its ids, its types,
 * constants and variables, and its code), each instruction an operation, the type of what it makes, the id of what it
 * makes, and its operands, each known as an id, a literal or a word of a string. Control is structured: an if says
 * where it merges, a loop where it merges and where it continues, as SPIR-V and Metal both need. Resources are
 * variables with a set and a binding.
 *
 * Operations are named by SPIR-V's numbers: the shaders were written against them (src/vulkan/spirv.h builds them,
 * now into this), and SPIR-V's grammar says what each means. What reads a module for another language (Metal, WGSL)
 * goes operation by operation, asking a value's type (irTypeOf) and what defined it (irDef); writing SPIR-V is joining
 * the sections (irSpirv). irCheck says what is wrong with a module, and irPrint shows it as text. */
#ifndef ARTFUL_SHADER_IR_H
#define ARTFUL_SHADER_IR_H

#include "artful/core.h"

#include <stdio.h>

typedef uint32_t IrId;

enum { IR_HEAD, IR_ANNOTATIONS, IR_TYPES, IR_CODE, IR_SECTIONS };
enum { IR_OPERAND_ID = 0, IR_OPERAND_LITERAL = 1, IR_OPERAND_STRING = 2 };

typedef struct IrInst {
    uint16_t op;
    uint8_t section;
    uint8_t shape;   /* IR_HAS_TYPE | IR_HAS_RESULT */
    IrId type, result; /* 0 where it has none */
    uint32_t at, count; /* its operands, after type and result: words[at] ... and kinds[at] ... */
} IrInst;
enum { IR_HAS_TYPE = 1, IR_HAS_RESULT = 2 };

typedef struct IrModule {
    ARTF_VEC(IrInst) insts;
    ARTF_VEC(uint32_t) words;
    ARTF_VEC(uint8_t) kinds;
    ARTF_VEC(uint32_t) defs; /* of each id, its instruction + 1; 0: none yet */
    IrId bound;              /* the last id made */
} IrModule;

void irInit(IrModule* m);
void irFree(IrModule* m);
IrId irNewId(IrModule* m);

/* An instruction, as SPIR-V's words after its first: its type and result where the operation has them (its shape, by
 * the grammar), then its operands, whose kinds the grammar says too. Returns its index. */
uint32_t irAppend(IrModule* m, int section, uint32_t op, const uint32_t* words, uint32_t n);

/* What made an id (NULL: nothing yet), and the type of a value (0: it has none, or is a type). */
const IrInst* irDef(const IrModule* m, IrId id);
IrId irTypeOf(const IrModule* m, IrId id);
ARTF_INLINE const uint32_t* irOperands(const IrModule* m, const IrInst* i) { return m->words.items + i->at; }
ARTF_INLINE const uint8_t* irKinds(const IrModule* m, const IrInst* i) { return m->kinds.items + i->at; }

/* What is wrong with it, written to why (NULL where it is sound): an id made twice, an id used that nothing makes, a
 * type that is not a type, code outside a function, a block not ended. */
bool irCheck(const IrModule* m, char* why, size_t size);
/* As text, an instruction a line: %result = Op<number> %type operands... */
void irPrint(const IrModule* m, FILE* out);

/* What a shader does with each resource it binds (a set and binding): read (a load, a texture's sample or fetch) and
 * write (a store, an atomic), through any chain of indices; what a frame graph needs to know of a pass. Returns how
 * many there are (up to max written). */
typedef struct IrUse { uint32_t set, binding; bool read, write; } IrUse;
int irUses(const IrModule* m, IrUse* out, int max);

/* SPIR-V's words read into a module (m initialized): each instruction into its section, as the order of a module
 * puts them (what is needed and said of the module, the names and decorations, the types, constants and globals before
 * the first function, then the code). false: the words are not SPIR-V (m holds what was read). */
bool irFromSpirv(IrModule* m, const uint32_t* words, size_t count);

/* Debugging information (NonSemantic.Shader.DebugInfo.100, for RenderDoc and the like): the module's main is the
 * function named function at line of file, the C that builds it. The words change; a device needs
 * VK_KHR_shader_non_semantic_info (core in Vulkan 1.3). */
void irDebugInfo(IrModule* m, const char* file, const char* function, uint32_t line);

/* SPIR-V 1.3's words (artfFree), the sections in their order. */
uint32_t* irSpirv(const IrModule* m, size_t* wordCount);

#endif
