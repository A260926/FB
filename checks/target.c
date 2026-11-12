/*
 * Targeted checks for integer switch lowering through tsCompile.
 *
 * These checks compile shaders with the public library entry point and then
 * inspect the generated SPIR-V independently of the compiler internals:
 *
 *   - a structural pass checks OpSwitch labels and their real targets, the
 *     selection-merge construct, one terminator per block, and id integrity;
 *   - a small integer executor runs the decoded control-flow graph so that
 *     fallthrough, nesting, break/continue, returns, kills and counted side
 *     effects are verified by their actual results rather than by opcode
 *     presence.
 *
 * Only the C standard library and the bundled headers are used. No SPIR-V
 * SDK and no GPU are required.
 */
#include "tinyshader.h"
#include "spirv.h"
#include "suite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Not present in the bundled spirv.h, but a valid terminator value.
#define SPV_OP_TERMINATE_INVOCATION 4416u

// The driver owns unit_expect; count assertions inside this file as well so
// that each group returns exactly the number of checks that passed.
#ifdef UNIT_CHECK
#undef UNIT_CHECK
#endif
#define UNIT_CHECK(x)                                                                    \
    do                                                                                   \
    {                                                                                    \
        unit_expect(!!(x), #x, __FILE__, __LINE__);                                       \
        g_group_checks++;                                                                 \
    } while (0)

static int g_group_checks;

#define ARRAY_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

////////////////////////////////
//
// Compilation helper
//
////////////////////////////////

typedef struct Compiled
{
    int ok;             // 1 when there were no diagnostics
    int spirv_present;  // 1 when SPIR-V bytes were returned
    char errors[4096];
    uint32_t *words;
    size_t word_count;
} Compiled;

static Compiled compileShader(const char *src, TsShaderStage stage)
{
    Compiled c;
    memset(&c, 0, sizeof(c));

    TsCompilerOptions *o = tsCompilerOptionsCreate();
    tsCompilerOptionsSetStage(o, stage);
    tsCompilerOptionsSetEntryPoint(o, "main", 4);
    tsCompilerOptionsSetSource(o, src, strlen(src), NULL, 0);

    TsCompilerOutput *r = tsCompile(o);

    const char *e = tsCompilerOutputGetErrors(r);
    size_t n = 0;
    const unsigned char *s = tsCompilerOutputGetSpirv(r, &n);

    c.spirv_present = (s != NULL && n > 0);

    if (e)
    {
        snprintf(c.errors, sizeof(c.errors), "%s", e);
        c.ok = 0;
    }
    else if (!s || n < 20)
    {
        snprintf(c.errors, sizeof(c.errors), "no SPIR-V produced");
        c.ok = 0;
    }
    else
    {
        c.ok = 1;
        c.word_count = n / 4;
        c.words = (uint32_t *)malloc(n);
        memcpy(c.words, s, n);
    }

    tsCompilerOutputDestroy(r);
    tsCompilerOptionsDestroy(o);
    return c;
}

static void compiledDestroy(Compiled *c)
{
    free(c->words);
    c->words = NULL;
    c->word_count = 0;
}

////////////////////////////////
//
// SPIR-V decoding
//
////////////////////////////////

typedef enum InstRole
{
    ROLE_INST,
    ROLE_FUNCTION,
    ROLE_PARAM,
    ROLE_BLOCK,
    ROLE_VAR,
    ROLE_TERM,
} InstRole;

typedef struct Inst
{
    uint32_t op;
    uint32_t offset;
    uint32_t word_count;
    InstRole role;

    uint32_t result_id;
    uint32_t type_id;

    uint32_t block_id;   // ROLE_BLOCK
    uint32_t func_index; // enclosing function index, or (uint32_t)-1
} Inst;

typedef struct SpvType
{
    uint32_t id;
    uint32_t op;
    uint32_t bits;      // OpTypeInt width
    int is_signed;
    uint32_t sub;       // element / pointee / return type
    uint32_t count;
} SpvType;

typedef struct SpvConst
{
    uint32_t id;
    uint32_t type_id;
    uint32_t op;
    int64_t value;
} SpvConst;

typedef struct Block
{
    uint32_t label_id;
    uint32_t first; // index of first non-label instruction
    uint32_t count;
    uint32_t func_index;
} Block;

typedef struct Func
{
    uint32_t id;
    uint32_t first_block;
    uint32_t block_count;
} Func;

typedef struct Module
{
    const uint32_t *words;
    size_t word_count;
    uint32_t id_bound;
    uint32_t entry_func_id;

    Inst *insts;
    uint32_t inst_count;

    SpvType *types;
    uint32_t type_count;

    SpvConst *consts;
    uint32_t const_count;

    Block *blocks;
    uint32_t block_count;

    Func *funcs;
    uint32_t func_count;
} Module;

static int opHasTypeAndResult(uint32_t op)
{
    switch (op)
    {
    case SpvOpLoad:
    case SpvOpAccessChain:
    case SpvOpFunctionCall:
    case SpvOpCompositeConstruct:
    case SpvOpCompositeExtract:
    case SpvOpVectorShuffle:
    case SpvOpSelect:
    case SpvOpExtInst:
    case SpvOpIAdd:
    case SpvOpISub:
    case SpvOpIMul:
    case SpvOpUDiv:
    case SpvOpSDiv:
    case SpvOpFAdd:
    case SpvOpFSub:
    case SpvOpFMul:
    case SpvOpFDiv:
    case SpvOpVectorTimesScalar:
    case SpvOpShiftRightLogical:
    case SpvOpShiftRightArithmetic:
    case SpvOpShiftLeftLogical:
    case SpvOpBitwiseOr:
    case SpvOpBitwiseXor:
    case SpvOpBitwiseAnd:
    case SpvOpNot:
    case SpvOpLogicalOr:
    case SpvOpLogicalAnd:
    case SpvOpLogicalNot:
    case SpvOpLogicalEqual:
    case SpvOpLogicalNotEqual:
    case SpvOpIEqual:
    case SpvOpINotEqual:
    case SpvOpUGreaterThan:
    case SpvOpSGreaterThan:
    case SpvOpUGreaterThanEqual:
    case SpvOpSGreaterThanEqual:
    case SpvOpULessThan:
    case SpvOpSLessThan:
    case SpvOpULessThanEqual:
    case SpvOpSLessThanEqual:
    case SpvOpConvertFToS:
    case SpvOpConvertFToU:
    case SpvOpConvertSToF:
    case SpvOpConvertUToF:
    case SpvOpSNegate:
    case SpvOpFNegate:
    case SpvOpBitcast:
    case SpvOpImageSampleImplicitLod:
    case SpvOpImageSampleExplicitLod:
    case SpvOpImageQuerySizeLod:
    case SpvOpImageQueryLevels: return 1;
    default: return 0;
    }
}

static uint32_t opSimpleResult(uint32_t op, const uint32_t *w, uint32_t wc)
{
    switch (op)
    {
    // Result id is word 1 for type declarations.
    case SpvOpTypeVoid:
    case SpvOpTypeBool:
    case SpvOpTypeInt:
    case SpvOpTypeFloat:
    case SpvOpTypeVector:
    case SpvOpTypeMatrix:
    case SpvOpTypeImage:
    case SpvOpTypeSampledImage:
    case SpvOpTypeArray:
    case SpvOpTypeRuntimeArray:
    case SpvOpTypeStruct:
    case SpvOpTypePointer:
    case SpvOpTypeFunction:
    case SpvOpExtInstImport: return (wc >= 2) ? w[1] : 0;

    // These carry a result type in word 1 and the result id in word 2.
    case SpvOpFunction:
    case SpvOpFunctionParameter:
    case SpvOpVariable:
    case SpvOpConstant:
    case SpvOpConstantTrue:
    case SpvOpConstantFalse:
    case SpvOpConstantComposite: return (wc >= 3) ? w[2] : 0;

    default: return 0;
    }
}

static int opIsTerminator(uint32_t op)
{
    switch (op)
    {
    case SpvOpReturn:
    case SpvOpReturnValue:
    case SpvOpKill:
    case SpvOpUnreachable:
    case SPV_OP_TERMINATE_INVOCATION:
    case SpvOpBranch:
    case SpvOpBranchConditional:
    case SpvOpSwitch: return 1;
    default: return 0;
    }
}

static int opIsType(uint32_t op)
{
    switch (op)
    {
    case SpvOpTypeVoid:
    case SpvOpTypeBool:
    case SpvOpTypeInt:
    case SpvOpTypeFloat:
    case SpvOpTypeVector:
    case SpvOpTypeMatrix:
    case SpvOpTypeImage:
    case SpvOpTypeSampledImage:
    case SpvOpTypeArray:
    case SpvOpTypeRuntimeArray:
    case SpvOpTypeStruct:
    case SpvOpTypePointer:
    case SpvOpTypeFunction: return 1;
    default: return 0;
    }
}

static Module *moduleDecode(const uint32_t *words, size_t word_count)
{
    Module *m = (Module *)calloc(1, sizeof(Module));
    m->words = words;
    m->word_count = word_count;
    m->id_bound = word_count > 4 ? words[3] : 0;

    uint32_t count = 0;
    size_t i = 5;
    while (i < word_count)
    {
        uint32_t wc = words[i] >> 16;
        if (wc == 0) break;
        count++;
        i += wc;
    }

    m->insts = (Inst *)calloc(count + 1, sizeof(Inst));

    i = 5;
    while (i < word_count)
    {
        uint32_t wc = words[i] >> 16;
        uint32_t op = words[i] & 0xffff;
        if (wc == 0) break;

        Inst *inst = &m->insts[m->inst_count++];
        inst->op = op;
        inst->offset = (uint32_t)i;
        inst->word_count = wc;
        inst->func_index = (uint32_t)-1;

        if (opHasTypeAndResult(op) && wc >= 3)
        {
            inst->type_id = words[i + 1];
            inst->result_id = words[i + 2];
        }
        else
        {
            inst->result_id = opSimpleResult(op, &words[i], wc);
        }

        if (op == SpvOpLabel)
        {
            inst->role = ROLE_BLOCK;
            inst->block_id = (wc >= 2) ? words[i + 1] : 0;
        }
        else if (opIsTerminator(op))
        {
            inst->role = ROLE_TERM;
        }
        else if (op == SpvOpVariable)
        {
            inst->role = ROLE_VAR;
        }
        else if (op == SpvOpFunction)
        {
            inst->role = ROLE_FUNCTION;
        }
        else if (op == SpvOpFunctionParameter)
        {
            inst->role = ROLE_PARAM;
        }
        else
        {
            inst->role = ROLE_INST;
        }

        i += wc;
    }

    m->types = (SpvType *)calloc(m->inst_count + 1, sizeof(SpvType));
    for (uint32_t k = 0; k < m->inst_count; ++k)
    {
        Inst *inst = &m->insts[k];
        if (!opIsType(inst->op) || inst->result_id == 0) continue;

        SpvType *t = &m->types[m->type_count++];
        t->id = inst->result_id;
        t->op = inst->op;

        const uint32_t *w = &words[inst->offset];
        switch (inst->op)
        {
        case SpvOpTypeInt:
            t->bits = w[2];
            t->is_signed = (int)w[3];
            break;
        case SpvOpTypeVector:
        case SpvOpTypeMatrix:
            t->sub = w[2];
            t->count = w[3];
            break;
        case SpvOpTypeArray:
            t->sub = w[2];
            t->count = w[3];
            break;
        case SpvOpTypeRuntimeArray:
            t->sub = w[2];
            break;
        case SpvOpTypePointer:
            t->sub = w[3];
            break;
        case SpvOpTypeImage:
            t->sub = w[2];
            break;
        case SpvOpTypeSampledImage:
            t->sub = w[2];
            break;
        case SpvOpTypeFunction:
            t->sub = w[2];
            break;
        case SpvOpTypeStruct:
            t->count = inst->word_count - 1;
            break;
        default: break;
        }
    }

    m->consts = (SpvConst *)calloc(m->inst_count + 1, sizeof(SpvConst));
    for (uint32_t k = 0; k < m->inst_count; ++k)
    {
        Inst *inst = &m->insts[k];
        if (inst->result_id == 0) continue;
        if (inst->op != SpvOpConstant && inst->op != SpvOpConstantTrue &&
            inst->op != SpvOpConstantFalse && inst->op != SpvOpConstantComposite)
        {
            continue;
        }

        SpvConst *c = &m->consts[m->const_count++];
        c->id = inst->result_id;
        c->op = inst->op;

        const uint32_t *w = &words[inst->offset];
        c->type_id = w[1];

        if (inst->op == SpvOpConstantTrue)
        {
            c->value = 1;
        }
        else if (inst->op == SpvOpConstantFalse)
        {
            c->value = 0;
        }
        else if (inst->op == SpvOpConstant && inst->word_count >= 4)
        {
            int is_signed = 0;
            for (uint32_t t = 0; t < m->type_count; ++t)
            {
                if (m->types[t].id == c->type_id && m->types[t].op == SpvOpTypeInt)
                {
                    is_signed = m->types[t].is_signed;
                }
            }
            c->value = is_signed ? (int64_t)(int32_t)w[3] : (int64_t)w[3];
        }
    }

    m->blocks = (Block *)calloc(m->inst_count + 1, sizeof(Block));
    m->funcs = (Func *)calloc(m->inst_count + 1, sizeof(Func));

    uint32_t func_index = (uint32_t)-1;
    for (uint32_t k = 0; k < m->inst_count; ++k)
    {
        Inst *inst = &m->insts[k];

        if (inst->role == ROLE_FUNCTION)
        {
            func_index = m->func_count;
            m->funcs[func_index].id = inst->result_id;
            m->funcs[func_index].first_block = m->block_count;
            m->func_count++;
            inst->func_index = func_index;
        }
        else if (inst->op == SpvOpFunctionEnd)
        {
            func_index = (uint32_t)-1;
            inst->func_index = (uint32_t)-1;
        }
        else if (inst->role == ROLE_BLOCK)
        {
            Block *b = &m->blocks[m->block_count++];
            b->label_id = inst->block_id;
            b->func_index = func_index;
            b->first = k + 1;

            // A block runs until the next label, or until the end of the
            // enclosing function (FunctionEnd / the next OpFunction).
            uint32_t j = k + 1;
            while (j < m->inst_count && m->insts[j].role != ROLE_BLOCK &&
                   m->insts[j].op != SpvOpFunctionEnd &&
                   m->insts[j].op != SpvOpFunction)
            {
                j++;
            }
            b->count = j - (k + 1);
            inst->func_index = func_index;
        }
        else
        {
            inst->func_index = func_index;
        }
    }

    for (uint32_t f = 0; f < m->func_count; ++f)
    {
        uint32_t end = (f + 1 < m->func_count) ? m->funcs[f + 1].first_block
                                               : m->block_count;
        m->funcs[f].block_count = end - m->funcs[f].first_block;
    }

    for (uint32_t k = 0; k < m->inst_count; ++k)
    {
        if (m->insts[k].op == SpvOpEntryPoint)
        {
            m->entry_func_id = words[m->insts[k].offset + 2];
            break;
        }
    }

    return m;
}

static void moduleDestroy(Module *m)
{
    if (!m) return;
    free(m->insts);
    free(m->types);
    free(m->consts);
    free(m->blocks);
    free(m->funcs);
    free(m);
}

static SpvConst *findConst(Module *m, uint32_t id)
{
    for (uint32_t i = 0; i < m->const_count; ++i)
        if (m->consts[i].id == id) return &m->consts[i];
    return NULL;
}

static Inst *findInst(Module *m, uint32_t result_id)
{
    if (result_id == 0) return NULL;
    for (uint32_t i = 0; i < m->inst_count; ++i)
        if (m->insts[i].result_id == result_id) return &m->insts[i];
    return NULL;
}

static Block *findBlock(Module *m, uint32_t label_id)
{
    for (uint32_t i = 0; i < m->block_count; ++i)
        if (m->blocks[i].label_id == label_id) return &m->blocks[i];
    return NULL;
}

static Func *findFunc(Module *m, uint32_t id)
{
    for (uint32_t i = 0; i < m->func_count; ++i)
        if (m->funcs[i].id == id) return &m->funcs[i];
    return NULL;
}

////////////////////////////////
//
// Structural validation
//
////////////////////////////////

typedef struct StructureReport
{
    int ok;
    int switch_count;
    int selection_merge_count;
    char problem[256];
} StructureReport;

static void structureFail(StructureReport *r, const char *msg)
{
    if (r->ok)
    {
        snprintf(r->problem, sizeof(r->problem), "%s", msg);
    }
    r->ok = 0;
}

// Classifies operand word index `j` of instruction `inst`. Returns 1 if the
// operand is an id, 0 if it is a literal or a string.
static int operandIsId(Module *m, Inst *inst, uint32_t j)
{
    const uint32_t *w = &m->words[inst->offset];
    uint32_t wc = inst->word_count;

    switch (inst->op)
    {
    case SpvOpTypeVoid:
    case SpvOpTypeBool:
        return 0;

    case SpvOpTypeInt:
    case SpvOpTypeFloat:
        return j == 1; // result id

    case SpvOpTypeVector:
    case SpvOpTypeMatrix:
        return j == 1 || j == 2; // result, element type

    case SpvOpTypeArray:
        return j == 1 || j == 2 || j == 3; // result, element, length const

    case SpvOpTypeRuntimeArray:
        return j == 1 || j == 2;

    case SpvOpTypeStruct:
        return 1; // result and every member type

    case SpvOpTypePointer:
        return j == 1 || j == 3; // result, pointee (storage class is literal)

    case SpvOpTypeFunction:
        return 1; // result, return type, parameter types

    case SpvOpTypeImage:
        return j == 1 || j == 2;

    case SpvOpTypeSampledImage:
        return j == 1 || j == 2;

    case SpvOpConstant:
        return j == 1 || j == 2; // type, result (then literal value)

    case SpvOpConstantTrue:
    case SpvOpConstantFalse:
        return j == 1 || j == 2;

    case SpvOpConstantComposite:
        return 1; // type, result, elements -- all ids

    case SpvOpFunction:
        return j == 1 || j == 2 || j == 4;

    case SpvOpFunctionParameter:
        return j == 1 || j == 2;

    case SpvOpFunctionEnd:
    case SpvOpReturn:
    case SpvOpKill:
    case SpvOpUnreachable:
    case SPV_OP_TERMINATE_INVOCATION:
        return 0;

    case SpvOpVariable:
        return j == 1 || j == 2 || j == 4; // type, result, initializer

    case SpvOpLoad:
        return 1;

    case SpvOpStore:
        return 1;

    case SpvOpAccessChain:
        return 1;

    case SpvOpFunctionCall:
        return 1;

    case SpvOpExtInst:
        return j <= 3 || j >= 5; // type, result, set; then #4 literal

    case SpvOpBranch:
        return j == 1; // label

    case SpvOpBranchConditional:
        return j >= 1 && j <= 3;

    case SpvOpSwitch:
        return j == 1 || j == 2 || (j >= 4 && j % 2 == 0);

    case SpvOpSelectionMerge:
        return j == 1;

    case SpvOpLoopMerge:
        return j == 1 || j == 2;

    case SpvOpReturnValue:
        return j == 1;

    case SpvOpDecorate:
        return j == 1;

    case SpvOpMemberDecorate:
        return j == 1;

    case SpvOpName:
        return j == 1;

    case SpvOpMemberName:
        return j == 1;

    case SpvOpExtInstImport:
        return j == 1;

    case SpvOpCapability:
    case SpvOpMemoryModel:
        return 0;

    case SpvOpSource:
        // language and version are literals; an optional file id follows.
        return j == 3;

    case SpvOpExecutionMode:
    case SpvOpExecutionModeId:
        return j == 1;

    case SpvOpLabel:
        return j == 1; // label id definition

    case SpvOpEntryPoint:
    {
        if (j == 2) return 1; // function id
        if (j <= 3) return 0; // execution model literal, start of name

        // Skip the string, then interface ids.
        const char *name = (const char *)&w[3];
        uint32_t name_bytes = (uint32_t)strlen(name);
        uint32_t name_first_word = 3 + (name_bytes / 4);
        uint32_t after_name = name_first_word + 1;
        return j >= after_name;
    }

    default:
        break;
    }

    if (opHasTypeAndResult(inst->op))
    {
        return j >= 3; // type, result, then operand ids
    }

    (void)wc;
    return 0;
}

static void validateStructure(Module *m, StructureReport *r)
{
    memset(r, 0, sizeof(*r));
    r->ok = 1;

    // 1. Exactly one terminator per block, and it is the final instruction.
    for (uint32_t b = 0; b < m->block_count; ++b)
    {
        Block *block = &m->blocks[b];
        int terminators = 0;
        for (uint32_t i = 0; i < block->count; ++i)
        {
            if (m->insts[block->first + i].role == ROLE_TERM) terminators++;
        }

        if (terminators != 1)
        {
            structureFail(r, "block does not have exactly one terminator");
            continue;
        }

        Inst *last = &m->insts[block->first + block->count - 1];
        if (last->role != ROLE_TERM)
        {
            structureFail(r, "terminator is not the last instruction in its block");
        }

        if (block->count == 0)
        {
            structureFail(r, "block has no instructions");
        }
    }

    // 2. Branch and switch targets are real labels inside the same function.
    for (uint32_t k = 0; k < m->inst_count; ++k)
    {
        Inst *inst = &m->insts[k];
        const uint32_t *w = &m->words[inst->offset];
        uint32_t wc = inst->word_count;

        uint32_t targets[64];
        uint32_t target_count = 0;

        if (inst->op == SpvOpBranch && wc >= 2)
        {
            targets[target_count++] = w[1];
        }
        else if (inst->op == SpvOpBranchConditional && wc >= 4)
        {
            targets[target_count++] = w[2];
            targets[target_count++] = w[3];
        }
        else if (inst->op == SpvOpSwitch)
        {
            r->switch_count++;

            if (wc < 3)
            {
                structureFail(r, "switch instruction is malformed");
                continue;
            }
            if ((wc - 3) % 2 != 0)
            {
                structureFail(r, "switch instruction has an odd operand count");
                continue;
            }

            targets[target_count++] = w[2];
            for (uint32_t j = 4; j < wc && target_count < 64; j += 2)
            {
                targets[target_count++] = w[j];
            }
        }

        for (uint32_t t = 0; t < target_count; ++t)
        {
            Block *block = findBlock(m, targets[t]);
            if (!block)
            {
                structureFail(r, "control-flow target is not a block label");
                continue;
            }
            if (block->func_index != inst->func_index)
            {
                structureFail(r, "control-flow target leaves its function");
            }
        }
    }

    // 3. Merge structure: every switch is immediately preceded by a
    //    selection merge whose merge block is defined in the same function.
    for (uint32_t b = 0; b < m->block_count; ++b)
    {
        Block *block = &m->blocks[b];

        for (uint32_t i = 0; i < block->count; ++i)
        {
            Inst *inst = &m->insts[block->first + i];

            if (inst->op == SpvOpSelectionMerge)
            {
                r->selection_merge_count++;

                if (i + 1 >= block->count)
                {
                    structureFail(r, "selection merge is not followed by a branch");
                    continue;
                }

                Inst *next = &m->insts[block->first + i + 1];
                if (next->op != SpvOpSwitch && next->op != SpvOpBranchConditional)
                {
                    structureFail(r, "selection merge is not followed by a branch");
                }

                const uint32_t *w = &m->words[inst->offset];
                Block *merge = findBlock(m, w[1]);
                if (!merge)
                {
                    structureFail(r, "selection merge block is not defined");
                }
                else if (merge->func_index != block->func_index)
                {
                    structureFail(r, "selection merge block leaves its function");
                }
            }

            if (inst->op == SpvOpSwitch)
            {
                if (i == 0 || m->insts[block->first + i - 1].op != SpvOpSelectionMerge)
                {
                    structureFail(r, "switch is not preceded by a selection merge");
                }
            }
        }
    }

    // 4. Reference integrity: every id operand resolves to exactly one
    //    definition (a result id or a block label), and result ids are unique.
    for (uint32_t k = 0; k < m->inst_count; ++k)
    {
        Inst *inst = &m->insts[k];
        const uint32_t *w = &m->words[inst->offset];

        for (uint32_t j = 1; j < inst->word_count; ++j)
        {
            if (!operandIsId(m, inst, j)) continue;

            uint32_t id = w[j];
            if (id == 0)
            {
                structureFail(r, "an id operand is zero");
                continue;
            }

            if (inst->op == SpvOpLabel)
            {
                if (!findBlock(m, id))
                {
                    structureFail(r, "label definition is missing");
                }
                continue;
            }

            if (!findInst(m, id) && !findBlock(m, id))
            {
                structureFail(r, "dangling id reference");
                break;
            }
        }

        if (!r->ok) break;
    }

    if (r->ok)
    {
        for (uint32_t k = 0; k < m->inst_count; ++k)
        {
            Inst *inst = &m->insts[k];
            if (inst->result_id == 0) continue;

            uint32_t defs = 0;
            for (uint32_t j = 0; j < m->inst_count; ++j)
            {
                if (m->insts[j].result_id == inst->result_id) defs++;
            }
            if (defs != 1)
            {
                structureFail(r, "result id is defined more than once");
                break;
            }
        }

        for (uint32_t b = 0; b < m->block_count; ++b)
        {
            uint32_t defs = 0;
            for (uint32_t j = 0; j < m->block_count; ++j)
            {
                if (m->blocks[j].label_id == m->blocks[b].label_id) defs++;
            }
            if (defs != 1)
            {
                structureFail(r, "block label is defined more than once");
                break;
            }
        }
    }
}

static int countOp(Module *m, uint32_t op)
{
    int n = 0;
    for (uint32_t i = 0; i < m->inst_count; ++i)
        if (m->insts[i].op == op) n++;
    return n;
}

////////////////////////////////
//
// Small integer executor
//
////////////////////////////////

typedef struct Value
{
    int is_float;
    double f;
    int64_t i;
} Value;

typedef struct Exec
{
    Module *m;

    Value *slots;        // dense slots
    uint32_t *id_slot;   // id -> slot index + 1, 0 means absent
    uint32_t slot_count;

    int steps;
    int blocks_visited;
    int switch_executed;

    int finished;
    int discarded;
    int64_t ret;
} Exec;

static void execResetValues(Exec *e)
{
    e->slot_count = 0;
    memset(e->id_slot, 0, sizeof(uint32_t) * (e->m->id_bound + 2));
}

static Value execGet(Exec *e, uint32_t id)
{
    Value v;
    memset(&v, 0, sizeof(v));
    if (id == 0 || id > e->m->id_bound) return v;
    uint32_t slot = e->id_slot[id];
    if (slot == 0) return v;
    return e->slots[slot - 1];
}

static void execSet(Exec *e, uint32_t id, Value v)
{
    if (id == 0 || id > e->m->id_bound) return;
    uint32_t slot = e->id_slot[id];
    if (slot == 0)
    {
        e->slots[e->slot_count] = v;
        e->slot_count++;
        e->id_slot[id] = e->slot_count;
    }
    else
    {
        e->slots[slot - 1] = v;
    }
}

static int64_t valueI(const Value *v)
{
    return v->is_float ? (int64_t)v->f : v->i;
}

static int32_t valueI32(const Value *v)
{
    return (int32_t)valueI(v);
}

static int evalOperand(Exec *e, uint32_t id, Value *out)
{
    memset(out, 0, sizeof(*out));

    SpvConst *c = findConst(e->m, id);
    if (c)
    {
        out->i = c->value;
        return 1;
    }

    *out = execGet(e, id);
    return 1;
}

static int execBlock(Exec *e, uint32_t func_index, uint32_t block_label, int64_t *result);
static int execFunctionById(Exec *e, uint32_t func_index, const Value *args,
                            uint32_t arg_count, int64_t *result);

static int executeInst(Exec *e, Inst *inst, uint32_t *next_label, int *terminated,
                       int64_t *result)
{
    const uint32_t *w = &e->m->words[inst->offset];
    uint32_t wc = inst->word_count;

    Value a, b;

    switch (inst->op)
    {
    case SpvOpLabel:
    case SpvOpSelectionMerge:
    case SpvOpLoopMerge:
    case SpvOpVariable:
    case SpvOpFunction:
    case SpvOpFunctionEnd:
    case SpvOpFunctionParameter:
    case SpvOpName:
    case SpvOpMemberName:
    case SpvOpDecorate:
    case SpvOpCapability:
    case SpvOpMemoryModel:
    case SpvOpSource:
    case SpvOpExtInstImport:
    case SpvOpEntryPoint:
    case SpvOpExecutionMode: return 1;

    case SpvOpBranch:
        *next_label = w[1];
        *terminated = 1;
        return 1;

    case SpvOpBranchConditional:
        evalOperand(e, w[1], &a);
        *next_label = valueI32(&a) ? w[2] : w[3];
        *terminated = 1;
        return 1;

    case SpvOpSwitch:
    {
        e->switch_executed++;
        evalOperand(e, w[1], &a);
        int64_t selector = valueI(&a);

        uint32_t target = w[2];
        for (uint32_t j = 3; j + 1 < wc; j += 2)
        {
            if ((uint32_t)w[j] == (uint32_t)selector)
            {
                target = w[j + 1];
                break;
            }
        }

        *next_label = target;
        *terminated = 1;
        return 1;
    }

    case SpvOpReturn:
        e->finished = 1;
        e->ret = 0;
        *terminated = 1;
        return 1;

    case SpvOpReturnValue:
        evalOperand(e, w[1], &a);
        e->ret = valueI(&a);
        e->finished = 1;
        *terminated = 1;
        return 1;

    case SpvOpKill:
    case SPV_OP_TERMINATE_INVOCATION:
        e->discarded = 1;
        e->finished = 1;
        e->ret = 0;
        *terminated = 1;
        return 1;

    case SpvOpLoad:
        execSet(e, w[2], execGet(e, w[3]));
        return 1;

    case SpvOpStore:
        evalOperand(e, w[2], &a);
        execSet(e, w[1], a);
        return 1;

    case SpvOpIAdd:
    case SpvOpISub:
    case SpvOpIMul:
    case SpvOpSDiv:
    case SpvOpUDiv:
    case SpvOpSRem:
    case SpvOpSMod:
    case SpvOpShiftLeftLogical:
    case SpvOpShiftRightLogical:
    case SpvOpShiftRightArithmetic:
    case SpvOpBitwiseAnd:
    case SpvOpBitwiseOr:
    case SpvOpBitwiseXor:
    {
        evalOperand(e, w[3], &a);
        evalOperand(e, w[4], &b);

        int64_t x = valueI(&a);
        int64_t y = valueI(&b);
        int64_t r = 0;

        switch (inst->op)
        {
        case SpvOpIAdd: r = (int32_t)((uint32_t)x + (uint32_t)y); break;
        case SpvOpISub: r = (int32_t)((uint32_t)x - (uint32_t)y); break;
        case SpvOpIMul: r = (int32_t)((uint32_t)x * (uint32_t)y); break;
        case SpvOpSDiv: r = (int32_t)x / (y ? (int32_t)y : 1); break;
        case SpvOpUDiv:
            r = (int32_t)((uint32_t)x / (uint32_t)(y ? (uint32_t)y : 1));
            break;
        case SpvOpSRem: r = (int32_t)x % (y ? (int32_t)y : 1); break;
        case SpvOpSMod: r = (int32_t)x % (y ? (int32_t)y : 1); break;
        case SpvOpShiftLeftLogical:
            r = (int32_t)((uint32_t)x << ((uint32_t)y & 31));
            break;
        case SpvOpShiftRightLogical:
            r = (int32_t)((uint32_t)x >> ((uint32_t)y & 31));
            break;
        case SpvOpShiftRightArithmetic:
            r = (int32_t)x >> ((uint32_t)y & 31);
            break;
        case SpvOpBitwiseAnd: r = (int32_t)x & (int32_t)y; break;
        case SpvOpBitwiseOr: r = (int32_t)x | (int32_t)y; break;
        case SpvOpBitwiseXor: r = (int32_t)x ^ (int32_t)y; break;
        default: break;
        }

        Value rv;
        memset(&rv, 0, sizeof(rv));
        rv.i = r;
        execSet(e, w[2], rv);
        return 1;
    }

    case SpvOpSNegate:
    case SpvOpFNegate:
    {
        evalOperand(e, w[3], &a);
        Value rv;
        memset(&rv, 0, sizeof(rv));
        rv.i = -valueI(&a);
        execSet(e, w[2], rv);
        return 1;
    }

    case SpvOpNot:
    {
        evalOperand(e, w[3], &a);
        Value rv;
        memset(&rv, 0, sizeof(rv));
        rv.i = ~valueI32(&a);
        execSet(e, w[2], rv);
        return 1;
    }

    case SpvOpLogicalNot:
    {
        evalOperand(e, w[3], &a);
        Value rv;
        memset(&rv, 0, sizeof(rv));
        rv.i = valueI32(&a) == 0;
        execSet(e, w[2], rv);
        return 1;
    }

    case SpvOpLogicalOr:
    case SpvOpLogicalAnd:
    case SpvOpLogicalEqual:
    case SpvOpLogicalNotEqual:
    case SpvOpIEqual:
    case SpvOpINotEqual:
    case SpvOpUGreaterThan:
    case SpvOpSGreaterThan:
    case SpvOpUGreaterThanEqual:
    case SpvOpSGreaterThanEqual:
    case SpvOpULessThan:
    case SpvOpSLessThan:
    case SpvOpULessThanEqual:
    case SpvOpSLessThanEqual:
    {
        evalOperand(e, w[3], &a);
        evalOperand(e, w[4], &b);

        int64_t x = valueI(&a);
        int64_t y = valueI(&b);
        uint32_t ux = (uint32_t)x;
        uint32_t uy = (uint32_t)y;
        int64_t r = 0;

        switch (inst->op)
        {
        case SpvOpLogicalOr: r = (valueI32(&a) || valueI32(&b)); break;
        case SpvOpLogicalAnd: r = (valueI32(&a) && valueI32(&b)); break;
        case SpvOpLogicalEqual: r = (valueI32(&a) == valueI32(&b)); break;
        case SpvOpLogicalNotEqual: r = (valueI32(&a) != valueI32(&b)); break;
        case SpvOpIEqual: r = x == y; break;
        case SpvOpINotEqual: r = x != y; break;
        case SpvOpUGreaterThan: r = ux > uy; break;
        case SpvOpSGreaterThan: r = x > y; break;
        case SpvOpUGreaterThanEqual: r = ux >= uy; break;
        case SpvOpSGreaterThanEqual: r = x >= y; break;
        case SpvOpULessThan: r = ux < uy; break;
        case SpvOpSLessThan: r = x < y; break;
        case SpvOpULessThanEqual: r = ux <= uy; break;
        case SpvOpSLessThanEqual: r = x <= y; break;
        default: break;
        }

        Value rv;
        memset(&rv, 0, sizeof(rv));
        rv.i = r;
        execSet(e, w[2], rv);
        return 1;
    }

    case SpvOpBitcast:
    case SpvOpConvertSToF:
    case SpvOpConvertUToF:
    case SpvOpConvertFToS:
    case SpvOpConvertFToU:
    {
        evalOperand(e, w[3], &a);
        Value rv;
        memset(&rv, 0, sizeof(rv));
        rv.i = valueI(&a);
        execSet(e, w[2], rv);
        return 1;
    }

    case SpvOpCompositeConstruct:
    {
        // Only scalar-only composites need to be modelled for these tests;
        // keeping the first element is enough to carry loop-carried values.
        Value rv;
        memset(&rv, 0, sizeof(rv));
        if (wc >= 4)
        {
            evalOperand(e, w[3], &a);
            rv.i = valueI(&a);
        }
        execSet(e, w[2], rv);
        return 1;
    }

    case SpvOpCompositeExtract:
    {
        evalOperand(e, w[3], &a);
        Value rv;
        memset(&rv, 0, sizeof(rv));
        rv.i = valueI(&a);
        execSet(e, w[2], rv);
        return 1;
    }

    case SpvOpFunctionCall:
    {
        uint32_t callee_id = w[3];
        Func *callee = findFunc(e->m, callee_id);
        if (!callee)
        {
            return 0;
        }

        uint32_t arg_count = wc - 4;
        Value *args = (Value *)calloc(arg_count + 1, sizeof(Value));
        for (uint32_t i = 0; i < arg_count; ++i)
        {
            evalOperand(e, w[4 + i], &args[i]);
        }

        Exec sub;
        memset(&sub, 0, sizeof(sub));
        sub.m = e->m;
        sub.slots = (Value *)calloc(e->m->id_bound + 2, sizeof(Value));
        sub.id_slot = (uint32_t *)calloc(e->m->id_bound + 2, sizeof(uint32_t));

        int64_t ret = 0;
        int ok = execFunctionById(&sub, (uint32_t)(callee - e->m->funcs), args,
                                  arg_count, &ret);
        e->switch_executed += sub.switch_executed;
        e->blocks_visited += sub.blocks_visited;
        e->discarded |= sub.discarded;

        free(sub.slots);
        free(sub.id_slot);
        free(args);

        if (!ok) return 0;

        Value rv;
        memset(&rv, 0, sizeof(rv));
        rv.i = ret;
        execSet(e, w[2], rv);
        return 1;
    }

    case SpvOpSelect:
    {
        evalOperand(e, w[3], &a);
        Value tv, fv;
        evalOperand(e, w[4], &tv);
        evalOperand(e, w[5], &fv);
        execSet(e, w[2], valueI32(&a) ? tv : fv);
        return 1;
    }

    default:
        (void)result;
        return 0;
    }
}

static int execBlock(Exec *e, uint32_t func_index, uint32_t block_label, int64_t *result)
{
    Block *block = findBlock(e->m, block_label);
    if (!block || block->func_index != func_index) return 0;

    e->blocks_visited++;
    if (e->blocks_visited > 200000) return 0;

    for (uint32_t i = 0; i < block->count; ++i)
    {
        Inst *inst = &e->m->insts[block->first + i];

        e->steps++;
        if (e->steps > 2000000) return 0;

        uint32_t next_label = 0;
        int terminated = 0;

        if (!executeInst(e, inst, &next_label, &terminated, result)) return 0;

        if (terminated)
        {
            if (e->finished) return 1;
            return execBlock(e, func_index, next_label, result);
        }
    }

    return 0;
}

static int execFunctionById(Exec *e, uint32_t func_index, const Value *args,
                            uint32_t arg_count, int64_t *result)
{
    Func *func = &e->m->funcs[func_index];

    execResetValues(e);

    uint32_t bound = 0;
    for (uint32_t k = 0; k < e->m->inst_count; ++k)
    {
        Inst *inst = &e->m->insts[k];
        if (inst->op != SpvOpFunctionParameter) continue;
        if (inst->func_index != func_index) continue;
        if (bound >= arg_count) break;
        execSet(e, inst->result_id, args[bound]);
        bound++;
    }

    if (func->block_count == 0) return 0;

    Block *entry = &e->m->blocks[func->first_block];
    int ok = execBlock(e, func_index, entry->label_id, result);
    if (ok && result)
    {
        *result = e->ret;
    }
    return ok;
}

// Runs the *user* function behind the compiler-generated entry-point
// wrapper. The wrapper calls the user's function; executing that function
// directly gives us its return value as an integer.
static int runUserMain(Module *m, int64_t *result, int *discarded, int *saw_switch)
{
    Exec e;
    memset(&e, 0, sizeof(e));
    e.m = m;
    e.slots = (Value *)calloc(m->id_bound + 2, sizeof(Value));
    e.id_slot = (uint32_t *)calloc(m->id_bound + 2, sizeof(uint32_t));

    Func *entry = findFunc(m, m->entry_func_id);
    int ok = 0;

    if (entry)
    {
        uint32_t callee_id = 0;
        uint32_t arg_count = 0;

        // Locate the call the wrapper makes to the user function.
        for (uint32_t b = 0; b < entry->block_count && callee_id == 0; ++b)
        {
            Block *block = &m->blocks[entry->first_block + b];
            for (uint32_t i = 0; i < block->count; ++i)
            {
                Inst *inst = &m->insts[block->first + i];
                if (inst->op == SpvOpFunctionCall)
                {
                    callee_id = m->words[inst->offset + 3];
                    arg_count = inst->word_count - 4;
                    break;
                }
            }
        }

        uint32_t entry_index = (uint32_t)(entry - m->funcs);

        int64_t ret = 0;
        if (callee_id != 0)
        {
            Func *callee = findFunc(m, callee_id);
            if (callee)
            {
                // Wrapper inputs are stage inputs with no stored value, so
                // zeroed arguments are the correct model here.
                Value *args = (Value *)calloc(arg_count + 1, sizeof(Value));
                ok = execFunctionById(&e, (uint32_t)(callee - m->funcs), args,
                                      arg_count, &ret);
                free(args);
            }
        }
        else
        {
            ok = execFunctionById(&e, entry_index, NULL, 0, &ret);
        }

        if (ok) *result = ret;
    }

    if (discarded) *discarded = e.discarded;
    if (saw_switch) *saw_switch = e.switch_executed;

    free(e.slots);
    free(e.id_slot);
    return ok;
}

////////////////////////////////
//
// Shared check helpers
//
////////////////////////////////

typedef struct SwitchInfo
{
    int found;
    uint32_t selector;
    uint32_t default_label;
    uint32_t merge_label;
    uint32_t literals[64];
    uint32_t targets[64];
    int case_count;
} SwitchInfo;

// Finds the n-th OpSwitch (0-based) and decodes its labels, targets and the
// merge block declared immediately before it.
static int getSwitchInfo(Module *m, int wanted, SwitchInfo *info)
{
    memset(info, 0, sizeof(*info));

    int seen = 0;
    for (uint32_t k = 0; k < m->inst_count; ++k)
    {
        Inst *inst = &m->insts[k];
        if (inst->op != SpvOpSwitch) continue;

        if (seen++ != wanted) continue;

        const uint32_t *w = &m->words[inst->offset];
        info->found = 1;
        info->selector = w[1];
        info->default_label = w[2];

        for (uint32_t j = 3; j + 1 < inst->word_count && info->case_count < 64;
             j += 2)
        {
            info->literals[info->case_count] = w[j];
            info->targets[info->case_count] = w[j + 1];
            info->case_count++;
        }

        if (k > 0 && m->insts[k - 1].op == SpvOpSelectionMerge)
        {
            info->merge_label = m->words[m->insts[k - 1].offset + 1];
        }

        return 1;
    }

    return 0;
}

// Compiles an integer shader, checks the generic structure, and runs it.
// Returns the observable integer result in *value.
static int compileRunInt(const char *src, Compiled *c, Module **m,
                         StructureReport *r, int64_t *value, int *discarded)
{
    *c = compileShader(src, TS_SHADER_STAGE_FRAGMENT);
    if (!c->ok)
    {
        fprintf(stderr, "unexpected compile failure:\n%s\n", c->errors);
        return 0;
    }

    *m = moduleDecode(c->words, c->word_count);
    validateStructure(*m, r);
    if (!r->ok)
    {
        fprintf(stderr, "structure failure: %s\n", r->problem);
    }

    int ok = runUserMain(*m, value, discarded, NULL);
    if (!ok)
    {
        fprintf(stderr, "execution failed\n");
    }

    return 1;
}

static void finish(Compiled *c, Module *m)
{
    compiledDestroy(c);
    moduleDestroy(m);
}

////////////////////////////////
//
// Group: lowering
//
////////////////////////////////

// Basic dispatch, default placement, fallthrough, shared labels, empty
// switch, uint selectors, and statements after the switch.
static int groupLowering(void)
{
    g_group_checks = 0;

    // 1. Basic switch: case literals, distinct real targets, merge block,
    //    and the default chosen at runtime.
    {
        const char *src =
            "int main():SV_Target0 {\n"
            "  int x = 7;\n"
            "  int r = 0;\n"
            "  switch (x) {\n"
            "    case 1: r = 10; break;\n"
            "    case 2: r = 20; break;\n"
            "    default: r = 30;\n"
            "  }\n"
            "  return r;\n"
            "}\n";

        Compiled c;
        Module *m = NULL;
        StructureReport r;
        int64_t value = 0;

        UNIT_CHECK(compileRunInt(src, &c, &m, &r, &value, NULL));
        UNIT_CHECK(r.ok);
        UNIT_CHECK(r.switch_count == 1);
        UNIT_CHECK(r.selection_merge_count >= 1);

        SwitchInfo si;
        UNIT_CHECK(getSwitchInfo(m, 0, &si));
        UNIT_CHECK(si.case_count == 2);
        UNIT_CHECK(si.literals[0] == 1);
        UNIT_CHECK(si.literals[1] == 2);
        UNIT_CHECK(si.targets[0] != si.targets[1]);
        UNIT_CHECK(si.targets[0] != si.default_label);
        UNIT_CHECK(si.targets[1] != si.default_label);
        UNIT_CHECK(si.merge_label != 0);
        UNIT_CHECK(findBlock(m, si.merge_label) != NULL);
        UNIT_CHECK(findBlock(m, si.targets[0]) != NULL);
        UNIT_CHECK(findBlock(m, si.targets[1]) != NULL);
        UNIT_CHECK(findBlock(m, si.default_label) != NULL);
        UNIT_CHECK(value == 30);

        finish(&c, m);
    }

    // 2. Cases hit their own labels (not just the default).
    {
        const char *src =
            "int main():SV_Target0 {\n"
            "  int x = 2;\n"
            "  int r = 99;\n"
            "  switch (x) {\n"
            "    case 1: r = 11; break;\n"
            "    case 2: r = 22; break;\n"
            "    default: r = 33;\n"
            "  }\n"
            "  return r;\n"
            "}\n";

        Compiled c;
        Module *m = NULL;
        StructureReport r;
        int64_t value = 0;

        UNIT_CHECK(compileRunInt(src, &c, &m, &r, &value, NULL));
        UNIT_CHECK(value == 22);

        SwitchInfo si;
        UNIT_CHECK(getSwitchInfo(m, 0, &si));
        UNIT_CHECK(si.case_count == 2);
        UNIT_CHECK(findBlock(m, si.targets[1]) != NULL);

        finish(&c, m);
    }

    // 3. Constant expressions and hexadecimal labels.
    {
        const char *src =
            "int main():SV_Target0 {\n"
            "  int x = 3;\n"
            "  int r = 0;\n"
            "  switch (x) {\n"
            "    case 1 + 2: r = 5; break;\n"
            "    case 0x10:  r = 6; break;\n"
            "    default:   r = 7;\n"
            "  }\n"
            "  return r;\n"
            "}\n";

        Compiled c;
        Module *m = NULL;
        StructureReport r;
        int64_t value = 0;

        UNIT_CHECK(compileRunInt(src, &c, &m, &r, &value, NULL));
        UNIT_CHECK(value == 5);

        SwitchInfo si;
        UNIT_CHECK(getSwitchInfo(m, 0, &si));
        UNIT_CHECK(si.case_count == 2);
        UNIT_CHECK(si.literals[0] == 3);
        UNIT_CHECK(si.literals[1] == 16);

        finish(&c, m);
    }

    // 4. A default in the middle: the default region falls through to the
    //    next case, and code after the switch still runs.
    {
        const char *src =
            "int main():SV_Target0 {\n"
            "  int x = 5;\n"
            "  int r = 0;\n"
            "  switch (x) {\n"
            "    case 1: r = 1; break;\n"
            "    default: r = 2;\n"
            "    case 3: r = r + 3; break;\n"
            "  }\n"
            "  r = r + 4;\n"
            "  return r;\n"
            "}\n";

        Compiled c;
        Module *m = NULL;
        StructureReport r;
        int64_t value = 0;

        UNIT_CHECK(compileRunInt(src, &c, &m, &r, &value, NULL));
        UNIT_CHECK(value == 9); // default sets 2, falls into case 3, then +4

        SwitchInfo si;
        UNIT_CHECK(getSwitchInfo(m, 0, &si));
        UNIT_CHECK(si.case_count == 2);
        UNIT_CHECK(si.literals[0] == 1);
        UNIT_CHECK(si.literals[1] == 3);
        UNIT_CHECK(si.default_label != si.targets[0]);
        UNIT_CHECK(si.default_label != si.targets[1]);

        finish(&c, m);
    }

    // 5. Fallthrough across several labels, with no default and no match.
    {
        const char *src_a =
            "int main():SV_Target0 {\n"
            "  int x = 2;\n"
            "  int r = 0;\n"
            "  switch (x) {\n"
            "    case 0: r = r + 1; break;\n"
            "    case 1: r = r + 10;\n"
            "    case 2: r = r + 100;\n"
            "    default: r = r + 1000;\n"
            "  }\n"
            "  return r;\n"
            "}\n";

        const char *src_b =
            "int main():SV_Target0 {\n"
            "  int x = 1;\n"
            "  int r = 0;\n"
            "  switch (x) {\n"
            "    case 0: r = r + 1; break;\n"
            "    case 1: r = r + 10;\n"
            "    case 2: r = r + 100;\n"
            "    default: r = r + 1000;\n"
            "  }\n"
            "  return r;\n"
            "}\n";

        const char *src_c =
            "int main():SV_Target0 {\n"
            "  int x = 5;\n"
            "  int r = 0;\n"
            "  switch (x) {\n"
            "    case 1: r = 1; break;\n"
            "    case 2: r = 2; break;\n"
            "  }\n"
            "  r = r + 4;\n"
            "  return r;\n"
            "}\n";

        int64_t value = 0;
        Compiled c;
        Module *m;
        StructureReport r;

        UNIT_CHECK(compileRunInt(src_a, &c, &m, &r, &value, NULL));
        UNIT_CHECK(value == 1100); // case 2 flows into default
        finish(&c, m);

        UNIT_CHECK(compileRunInt(src_b, &c, &m, &r, &value, NULL));
        UNIT_CHECK(value == 1110); // case 1 flows through case 2 and default
        finish(&c, m);

        UNIT_CHECK(compileRunInt(src_c, &c, &m, &r, &value, NULL));
        UNIT_CHECK(value == 4); // no match, no default: continue after switch
        finish(&c, m);
    }

    // 6. Multiple labels sharing one statement block target the same label.
    {
        const char *src =
            "int main():SV_Target0 {\n"
            "  int x = 1;\n"
            "  int r = 0;\n"
            "  switch (x) {\n"
            "    case 1:\n"
            "    case 2: r = 9; break;\n"
            "    default: r = 4;\n"
            "  }\n"
            "  return r;\n"
            "}\n";

        Compiled c;
        Module *m = NULL;
        StructureReport r;
        int64_t value = 0;

        UNIT_CHECK(compileRunInt(src, &c, &m, &r, &value, NULL));
        UNIT_CHECK(value == 9);

        SwitchInfo si;
        UNIT_CHECK(getSwitchInfo(m, 0, &si));
        UNIT_CHECK(si.case_count == 2);
        UNIT_CHECK(si.targets[0] == si.targets[1]);

        finish(&c, m);
    }

    // 7. An empty switch is legal and execution continues after it.
    {
        const char *src =
            "int main():SV_Target0 {\n"
            "  int r = 3;\n"
            "  switch (r) { }\n"
            "  r = r + 1;\n"
            "  return r;\n"
            "}\n";

        Compiled c;
        Module *m = NULL;
        StructureReport r;
        int64_t value = 0;

        UNIT_CHECK(compileRunInt(src, &c, &m, &r, &value, NULL));
        UNIT_CHECK(r.ok);
        UNIT_CHECK(r.switch_count == 0); // nothing to dispatch on
        UNIT_CHECK(value == 4);

        finish(&c, m);
    }

    // 8. Unsigned selectors and unsigned constant labels.
    {
        const char *src =
            "int main():SV_Target0 {\n"
            "  uint x = 2u;\n"
            "  int r = 9;\n"
            "  switch (x) {\n"
            "    case 1u: r = 1; break;\n"
            "    case 2u: r = 2; break;\n"
            "    default: r = 3;\n"
            "  }\n"
            "  return r;\n"
            "}\n";

        Compiled c;
        Module *m = NULL;
        StructureReport r;
        int64_t value = 0;

        UNIT_CHECK(compileRunInt(src, &c, &m, &r, &value, NULL));
        UNIT_CHECK(value == 2);

        SwitchInfo si;
        UNIT_CHECK(getSwitchInfo(m, 0, &si));
        UNIT_CHECK(si.case_count == 2);
        UNIT_CHECK(si.literals[0] == 1);
        UNIT_CHECK(si.literals[1] == 2);

        finish(&c, m);
    }

    // 9. A const integer variable can name a case label; a local declaration
    //    may directly follow a label (and is in scope for later statements).
    {
        const char *src =
            "int main():SV_Target0 {\n"
            "  const int k = 4;\n"
            "  int x = 4;\n"
            "  int r = 0;\n"
            "  switch (x) {\n"
            "    case k: {\n"
            "      int y = 2;\n"
            "      r = y + 5;\n"
            "      break;\n"
            "    }\n"
            "    default: r = 1;\n"
            "  }\n"
            "  return r;\n"
            "}\n";

        Compiled c;
        Module *m = NULL;
        StructureReport r;
        int64_t value = 0;

        UNIT_CHECK(compileRunInt(src, &c, &m, &r, &value, NULL));
        UNIT_CHECK(value == 7);

        SwitchInfo si;
        UNIT_CHECK(getSwitchInfo(m, 0, &si));
        UNIT_CHECK(si.literals[0] == 4);

        finish(&c, m);
    }

    // 10. Negative case literals dispatch correctly (signed literal
    //     truncation matches the runtime selector).
    {
        const char *src =
            "int main():SV_Target0 {\n"
            "  int x = -1;\n"
            "  int r = 0;\n"
            "  switch (x) {\n"
            "    case -1: r = 1; break;\n"
            "    case 0: r = 2; break;\n"
            "    default: r = 3;\n"
            "  }\n"
            "  return r;\n"
            "}\n";

        Compiled c;
        Module *m = NULL;
        StructureReport r;
        int64_t value = 0;

        UNIT_CHECK(compileRunInt(src, &c, &m, &r, &value, NULL));
        UNIT_CHECK(value == 1);

        SwitchInfo si;
        UNIT_CHECK(getSwitchInfo(m, 0, &si));
        UNIT_CHECK(si.literals[0] == (uint32_t)-1);
        UNIT_CHECK(si.literals[1] == 0);

        finish(&c, m);
    }

    // 11. A local declaration directly after a label, with no later label
    //     in the same switch, is legal and initialized in that section.
    {
        const char *src =
            "int main():SV_Target0 {\n"
            "  int x = 1;\n"
            "  int r = 0;\n"
            "  switch (x) {\n"
            "    case 0: r = 1; break;\n"
            "    case 1:\n"
            "      int y = 7;\n"
            "      r = y + 2;\n"
            "      break;\n"
            "  }\n"
            "  return r;\n"
            "}\n";

        Compiled c;
        Module *m = NULL;
        StructureReport r;
        int64_t value = 0;

        UNIT_CHECK(compileRunInt(src, &c, &m, &r, &value, NULL));
        UNIT_CHECK(r.ok);
        UNIT_CHECK(value == 9);

        finish(&c, m);
    }

    return g_group_checks;
}

////////////////////////////////
//
// Group: control
//
////////////////////////////////

static int groupControl(void)
{
    g_group_checks = 0;

    // 1. switch inside a for loop: `break` leaves only the switch, while
    //    `continue` still reaches the loop's continue block.
    {
        const char *src =
            "int main():SV_Target0 {\n"
            "  int acc = 0;\n"
            "  for (int i = 0; i < 5; i = i + 1) {\n"
            "    switch (i) {\n"
            "      case 0: continue;\n"
            "      case 2: acc = acc + 100; break;\n"
            "      default: acc = acc + 10;\n"
            "    }\n"
            "    acc = acc + 1;\n"
            "  }\n"
            "  return acc;\n"
            "}\n";

        Compiled c;
        Module *m = NULL;
        StructureReport r;
        int64_t value = 0;

        UNIT_CHECK(compileRunInt(src, &c, &m, &r, &value, NULL));
        UNIT_CHECK(r.ok);
        UNIT_CHECK(r.switch_count == 1);
        UNIT_CHECK(value == 134);

        finish(&c, m);
    }

    // 2. A loop inside a switch: the loop's `break` exits the loop only and
    //    the code after it (still in the switch) executes.
    {
        const char *src =
            "int main():SV_Target0 {\n"
            "  int acc = 0;\n"
            "  int k = 3;\n"
            "  switch (k) {\n"
            "    case 3:\n"
            "      for (int j = 0; j < 10; j = j + 1) {\n"
            "        acc = acc + 1;\n"
            "        if (j == 2) break;\n"
            "      }\n"
            "      acc = acc + 100;\n"
            "      break;\n"
            "    default: acc = 999;\n"
            "  }\n"
            "  return acc;\n"
            "}\n";

        Compiled c;
        Module *m = NULL;
        StructureReport r;
        int64_t value = 0;

        UNIT_CHECK(compileRunInt(src, &c, &m, &r, &value, NULL));
        UNIT_CHECK(value == 103);

        finish(&c, m);
    }

    // 3. Nested switches: the inner `break` exits the inner switch only.
    {
        const char *src =
            "int main():SV_Target0 {\n"
            "  int acc = 0;\n"
            "  int a = 1;\n"
            "  int b = 2;\n"
            "  switch (a) {\n"
            "    case 1:\n"
            "      switch (b) {\n"
            "        case 2: acc = acc + 5; break;\n"
            "        default: acc = acc + 7;\n"
            "      }\n"
            "      acc = acc + 11;\n"
            "      break;\n"
            "    default: acc = 500;\n"
            "  }\n"
            "  return acc;\n"
            "}\n";

        Compiled c;
        Module *m = NULL;
        StructureReport r;
        int64_t value = 0;

        UNIT_CHECK(compileRunInt(src, &c, &m, &r, &value, NULL));
        UNIT_CHECK(r.switch_count == 2);
        UNIT_CHECK(value == 16);

        SwitchInfo outer, inner;
        UNIT_CHECK(getSwitchInfo(m, 0, &outer));
        UNIT_CHECK(getSwitchInfo(m, 1, &inner));
        UNIT_CHECK(outer.merge_label != inner.merge_label);

        finish(&c, m);
    }

    // 4. `return` inside a case: returned value is observed and the block is
    //    not given a second terminator (structural check covers that).
    {
        const char *src =
            "int main():SV_Target0 {\n"
            "  int x = 1;\n"
            "  switch (x) {\n"
            "    case 1: return 42;\n"
            "    default: return 7;\n"
            "  }\n"
            "  return 0;\n"
            "}\n";

        Compiled c;
        Module *m = NULL;
        StructureReport r;
        int64_t value = 0;

        UNIT_CHECK(compileRunInt(src, &c, &m, &r, &value, NULL));
        UNIT_CHECK(r.ok);
        UNIT_CHECK(value == 42);

        int returns_here = 0;
        SwitchInfo si;
        UNIT_CHECK(getSwitchInfo(m, 0, &si));
        for (uint32_t k = 0; k < m->inst_count; ++k)
        {
            Inst *inst = &m->insts[k];
            if (inst->op != SpvOpReturn && inst->op != SpvOpReturnValue) continue;
            if (inst->func_index == (uint32_t)-1) continue;
            if (findBlock(m, m->words[inst->offset - 1]) == NULL) continue;
            returns_here++;
        }
        UNIT_CHECK(returns_here >= 2); // both case bodies return

        finish(&c, m);
    }

    // 5. `discard` inside a case is a terminator and is not followed by any
    //    other terminator or instruction in its block.
    {
        const char *src =
            "float4 main():SV_Target {\n"
            "  int x = 1;\n"
            "  switch (x) {\n"
            "    case 1: discard;\n"
            "    default: break;\n"
            "  }\n"
            "  return float4(0.0, 0.0, 0.0, 1.0);\n"
            "}\n";

        Compiled c = compileShader(src, TS_SHADER_STAGE_FRAGMENT);
        UNIT_CHECK(c.ok);

        Module *m = moduleDecode(c.words, c.word_count);
        StructureReport r;
        validateStructure(m, &r);
        UNIT_CHECK(r.ok);

        int kills = countOp(m, SpvOpKill);
        UNIT_CHECK(kills == 1);
        UNIT_CHECK(r.switch_count == 1);

        // Locate the block that holds the kill and check that it ends there.
        int kill_is_last = 0;
        for (uint32_t k = 0; k < m->inst_count; ++k)
        {
            Inst *inst = &m->insts[k];
            if (inst->op != SpvOpKill) continue;

            for (uint32_t b = 0; b < m->block_count; ++b)
            {
                Block *block = &m->blocks[b];
                if (k == block->first + block->count - 1) kill_is_last = 1;
            }
        }
        UNIT_CHECK(kill_is_last);

        finish(&c, m);
    }

    // 6. The selecting expression is evaluated exactly once even though it
    //    has a counting side effect.
    {
        const char *src =
            "int main():SV_Target0 {\n"
            "  int i = 0;\n"
            "  int r = 0;\n"
            "  switch (i++) {\n"
            "    case 0: r = 5; break;\n"
            "    default: r = 6;\n"
            "  }\n"
            "  return r * 10 + i;\n"
            "}\n";

        Compiled c;
        Module *m = NULL;
        StructureReport r;
        int64_t value = 0;

        UNIT_CHECK(compileRunInt(src, &c, &m, &r, &value, NULL));
        UNIT_CHECK(value == 51); // selector saw 0, i advanced exactly once

        finish(&c, m);
    }

    // 7. Side effect through a helper call used as the selector, evaluated
    //    exactly once.
    {
        const char *src =
            "int main():SV_Target0 {\n"
            "  int i = 0;\n"
            "  int r = 0;\n"
            "  switch (i++) {\n"
            "    case 1: r = 1; break;\n"
            "    case 0: r = 2; break;\n"
            "  }\n"
            "  return r * 10 + i;\n"
            "}\n";

        Compiled c;
        Module *m = NULL;
        StructureReport r;
        int64_t value = 0;

        UNIT_CHECK(compileRunInt(src, &c, &m, &r, &value, NULL));
        UNIT_CHECK(value == 21); // case 0 matched, i advanced once

        finish(&c, m);
    }

    // 8. `break` without an enclosing loop is valid when inside a switch.
    {
        const char *src =
            "int main():SV_Target0 {\n"
            "  int x = 9;\n"
            "  switch (x) {\n"
            "    case 0: break;\n"
            "    default: break;\n"
            "  }\n"
            "  return 1;\n"
            "}\n";

        Compiled c;
        Module *m = NULL;
        StructureReport r;
        int64_t value = 0;

        UNIT_CHECK(compileRunInt(src, &c, &m, &r, &value, NULL));
        UNIT_CHECK(value == 1);

        finish(&c, m);
    }

    // 9. `continue` from an inner switch still targets the outer loop's
    //    continue block even when another switch sits between them.
    {
        const char *src =
            "int main():SV_Target0 {\n"
            "  int a = 0;\n"
            "  int i = 0;\n"
            "  int r = 0;\n"
            "  for (i = 0; i < 4; i = i + 1) {\n"
            "    switch (i) {\n"
            "      case 1:\n"
            "        switch (a) {\n"
            "          case 0: continue;\n"
            "        }\n"
            "        break;\n"
            "      default: r = r + 10;\n"
            "    }\n"
            "    r = r + 1;\n"
            "  }\n"
            "  return r * 10 + i;\n"
            "}\n";

        Compiled c;
        Module *m = NULL;
        StructureReport r;
        int64_t value = 0;

        UNIT_CHECK(compileRunInt(src, &c, &m, &r, &value, NULL));
        // i=0: +10+1 => 11; i=1: continue (skip the trailing increment);
        // i=2: +10+1 => 22; i=3: +10+1 => 33; i ends at 4.
        UNIT_CHECK(value == 334);

        finish(&c, m);
    }

    // 10. switch nested inside a while loop: continue goes to the while
    //     condition, not to the switch merge.
    {
        const char *src =
            "int main():SV_Target0 {\n"
            "  int i = 0;\n"
            "  int acc = 0;\n"
            "  while (i < 4) {\n"
            "    i = i + 1;\n"
            "    switch (i) {\n"
            "      case 2: continue;\n"
            "      default: acc = acc + 10;\n"
            "    }\n"
            "    acc = acc + 1;\n"
            "  }\n"
            "  return acc;\n"
            "}\n";

        Compiled c;
        Module *m = NULL;
        StructureReport r;
        int64_t value = 0;

        UNIT_CHECK(compileRunInt(src, &c, &m, &r, &value, NULL));
        // i=1: default +10, then +1 => 11
        // i=2: continue => 11
        // i=3: +10+1 => 22
        // i=4: +10+1 => 33
        UNIT_CHECK(value == 33);

        finish(&c, m);
    }

    // 11. A do-while loop inside a switch: the loop's continue/break stay
    //     local to the loop, and the switch's own break exits the switch.
    {
        const char *src =
            "int main():SV_Target0 {\n"
            "  int acc = 0;\n"
            "  int k = 1;\n"
            "  switch (k) {\n"
            "    case 1: {\n"
            "      int i = 0;\n"
            "      do {\n"
            "        i = i + 1;\n"
            "        if (i == 2) continue;\n"
            "        acc = acc + 10;\n"
            "      } while (i < 3);\n"
            "      acc = acc + 1;\n"
            "      break;\n"
            "    }\n"
            "    default: acc = 99;\n"
            "  }\n"
            "  return acc;\n"
            "}\n";

        Compiled c;
        Module *m = NULL;
        StructureReport r;
        int64_t value = 0;

        UNIT_CHECK(compileRunInt(src, &c, &m, &r, &value, NULL));
        // i=1 -> +10, i=2 -> continue, i=3 -> +10, then +1.
        UNIT_CHECK(value == 21);

        finish(&c, m);
    }

    return g_group_checks;
}

////////////////////////////////
//
// Group: diagnostics
//
////////////////////////////////

static int expectRejected(const char *src, const char *needle)
{
    Compiled c = compileShader(src, TS_SHADER_STAGE_FRAGMENT);

    int ok = 1;
    if (c.ok) ok = 0;
    if (c.spirv_present) ok = 0; // never emit a partial module
    if (c.errors[0] == '\0') ok = 0;
    if (needle && !strstr(c.errors, needle)) ok = 0;

    if (!ok)
    {
        fprintf(stderr, "diagnostic mismatch (wanted '%s') got:\n%s\n",
                needle ? needle : "<any>", c.errors);
    }

    compiledDestroy(&c);
    return ok;
}

static int groupDiagnostics(void)
{
    g_group_checks = 0;

    // Duplicate case labels (two expressions with the same value).
    {
        const char *src =
            "int main():SV_Target0 { int x = 1; switch (x) {"
            " case 2: break; case 1 + 1: break; }"
            " return 0; }";
        UNIT_CHECK(expectRejected(src, "duplicate case"));
    }

    // A second default label.
    {
        const char *src =
            "int main():SV_Target0 { int x = 1; switch (x) {"
            " default: break; default: break; }"
            " return 0; }";
        UNIT_CHECK(expectRejected(src, "default"));
    }

    // A non-constant case label.
    {
        const char *src =
            "int main():SV_Target0 { int x = 1; int y = 2; switch (x) {"
            " case y: break; }"
            " return 0; }";
        UNIT_CHECK(expectRejected(src, "constant"));
    }

    // A floating-point selector.
    {
        const char *src =
            "int main():SV_Target0 { switch (1.5) { case 1: break; }"
            " return 0; }";
        UNIT_CHECK(expectRejected(src, "integer"));
    }

    // A vector selector.
    {
        const char *src =
            "int main():SV_Target0 { int2 v = 1; switch (v) { case 1: break; }"
            " return 0; }";
        UNIT_CHECK(expectRejected(src, "integer"));
    }

    // A case label with a runtime side effect (a user function call) is not
    // a constant expression.
    {
        const char *src =
            "int f() { return 1; }\n"
            "int main():SV_Target0 { int x = 1; switch (x) {"
            " case f(): break; }"
            " return 0; }";
        UNIT_CHECK(expectRejected(src, "constant"));
    }

    // A label that jumps into the scope of an uninitialized local.
    {
        const char *src =
            "int main():SV_Target0 { int x = 1; switch (x) {"
            " case 0: int y = 3; break; case 1: y = 5; break; }"
            " return 0; }";
        UNIT_CHECK(expectRejected(src, "scope"));
    }

    // A non-integer case label.
    {
        const char *src =
            "int main():SV_Target0 { int x = 1; switch (x) {"
            " case 1.5: break; }"
            " return 0; }";
        UNIT_CHECK(expectRejected(src, "integer"));
    }

    // continue inside a switch with no enclosing loop must be diagnosed
    // rather than lowering to an invalid branch.
    {
        const char *src =
            "int main():SV_Target0 { int x = 1; switch (x) {"
            " case 1: continue; }"
            " return 0; }";
        UNIT_CHECK(expectRejected(src, "continue"));
    }

    return g_group_checks;
}

////////////////////////////////
//
// Entry point
//
////////////////////////////////

int unit_run_group(const char *name)
{
    if (strcmp(name, "lowering") == 0) return groupLowering();
    if (strcmp(name, "control") == 0) return groupControl();
    if (strcmp(name, "diagnostics") == 0) return groupDiagnostics();
    return -1;
}
