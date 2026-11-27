/*
 * FCML - Free Code Manipulation Library.
 * Regression coverage for encoded field origins and symbol resolution.
 *
 * The analyzed instructions are synthetic byte sequences that are only read
 * as data; they are never executed. This file is driven by task_support/
 * driver.c and therefore does not define main().
 */

#include "task_support/check.h"

#include <fcml_disassembler.h>
#include <fcml_intel_dialect.h>
#include <fcml_gas_dialect.h>
#include <fcml_renderer.h>

#include <string.h>

/* Render flags shared with the baseline reference output. */
#define FIELD_REND_FLAGS (FCML_REND_FLAG_REMOVE_LEADING_ZEROS | \
        FCML_REND_FLAG_HEX_IMM | FCML_REND_FLAG_HEX_DISPLACEMENT)

struct bytes_def {
    const fcml_uint8_t *code;
    fcml_usize length;
    fcml_en_operating_mode mode;
};

static const fcml_uint8_t b_imm32[] = {0x83, 0xc0, 0xff};
static const fcml_uint8_t b_imm16[] = {0x83, 0xc0, 0xff};
static const fcml_uint8_t b_imm64[] = {0x48, 0x83, 0xc0, 0xff};
static const fcml_uint8_t b_disp_imm[] = {0x83, 0x44, 0x88, 0xf0, 0x7f};
static const fcml_uint8_t b_branch32[] = {0xe8, 0x01, 0x00, 0x00, 0x00};
static const fcml_uint8_t b_branch8[] = {0xeb, 0x02};
static const fcml_uint8_t b_moffs32[] = {0xa1, 0x78, 0x56, 0x34, 0x12};
static const fcml_uint8_t b_moffs64[] = {0x48, 0xa1, 0x78, 0x56, 0x34, 0x12,
        0x00, 0x00, 0x00, 0x00};
static const fcml_uint8_t b_far32[] = {0xea, 0x78, 0x56, 0x34, 0x12, 0x9a,
        0xbc};
static const fcml_uint8_t b_far16[] = {0xea, 0x78, 0x56, 0x9a, 0xbc};
static const fcml_uint8_t b_evex[] = {0x62, 0xf2, 0xed, 0x58, 0x0d, 0x4c,
        0x24, 0x08};
static const fcml_uint8_t b_cut[] = {0x83, 0xc0}; /* missing imm8 */

static fcml_ceh_error decode_one(const struct bytes_def *def,
        fcml_st_disassembler *disasm, fcml_st_disassembler_result *result) {
    fcml_st_disassembler_context context;
    memset(&context, 0, sizeof(context));
    context.disassembler = disasm;
    context.entry_point.op_mode = def->mode;
    context.entry_point.ip = 0x1000;
    if (def->mode == FCML_OM_64_BIT) {
        context.entry_point.address_size_attribute = FCML_DS_64;
        context.entry_point.operand_size_attribute = FCML_DS_64;
    }
    context.code = (fcml_ptr) def->code;
    context.code_length = def->length;
    fcml_fn_disassembler_result_prepare(result);
    return fcml_fn_disassemble(&context, result);
}

static fcml_ceh_error make_disasm(int gas, fcml_st_dialect **dialect,
        fcml_st_disassembler **disasm) {
    fcml_ceh_error error = gas ? fcml_fn_dialect_init_gas(0, dialect)
            : fcml_fn_dialect_init_intel(0, dialect);
    if (error) {
        return error;
    }
    return fcml_fn_disassembler_init(*dialect, disasm);
}

static void render_text(const fcml_st_dialect *dialect,
        fcml_fnp_symbol_resolver resolver, fcml_ptr resolver_context,
        const fcml_st_disassembler_result *result, char *buffer,
        fcml_usize length) {
    fcml_st_render_config config;
    memset(&config, 0, sizeof(config));
    config.render_flags = FIELD_REND_FLAGS;
    config.symbol_resolver = resolver;
    config.symbol_context = resolver_context;
    fcml_ceh_error error = fcml_fn_render(dialect, &config, buffer, length,
            result);
    /* Keep the buffer deterministic if rendering ever fails. */
    if (error) {
        buffer[0] = '\0';
    }
}

static const fcml_st_encoded_field *field_of(
        const fcml_st_disassembler_result *result, fcml_int operand,
        fcml_int index) {
    return &(result->instruction_details.operand_details[operand]
            .encoded_fields[index]);
}

static int total_field_count(const fcml_st_disassembler_result *result) {
    int total = 0;
    for (int i = 0; i < result->instruction.operands_count; i++) {
        total += result->instruction_details.operand_details[i]
                .encoded_field_count;
    }
    return total;
}

/* Flatten all fields and order them by their physical location so the same
 * bytes can be compared across dialects regardless of operand reversal. */
struct flat_field {
    fcml_usize offset;
    fcml_en_encoded_field_kind kind;
    fcml_usize size;
    fcml_uint64_t raw_value;
};

static int collect_fields(const fcml_st_disassembler_result *result,
        struct flat_field *out, int capacity) {
    int count = 0;
    for (int op = 0; op < result->instruction.operands_count; op++) {
        const fcml_st_operand_details *details =
                &(result->instruction_details.operand_details[op]);
        for (int f = 0; f < details->encoded_field_count; f++) {
            if (count >= capacity) {
                return count;
            }
            out[count].offset = details->encoded_fields[f].offset;
            out[count].kind = details->encoded_fields[f].kind;
            out[count].size = details->encoded_fields[f].size;
            out[count].raw_value = details->encoded_fields[f].raw_value;
            count++;
        }
    }
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (out[j].offset < out[i].offset
                    || (out[j].offset == out[i].offset
                            && out[j].kind < out[i].kind)) {
                struct flat_field tmp = out[i];
                out[i] = out[j];
                out[j] = tmp;
            }
        }
    }
    return count;
}

static int same_flat(const struct flat_field *a, int na,
        const struct flat_field *b, int nb) {
    if (na != nb) {
        return 0;
    }
    for (int i = 0; i < na; i++) {
        if (a[i].offset != b[i].offset || a[i].kind != b[i].kind
                || a[i].size != b[i].size
                || a[i].raw_value != b[i].raw_value) {
            return 0;
        }
    }
    return 1;
}

/*********************
 * encoding group.   *
 *********************/

static int run_short_immediates(void) {
    struct bytes_def def32 = {b_imm32, sizeof(b_imm32), FCML_OM_32_BIT};
    struct bytes_def def16 = {b_imm16, sizeof(b_imm16), FCML_OM_16_BIT};
    struct bytes_def def64 = {b_imm64, sizeof(b_imm64), FCML_OM_64_BIT};
    const struct bytes_def *defs[] = {&def32, &def16, &def64};
    const fcml_usize expected_offset[] = {2, 2, 3};

    fcml_st_dialect *dialect = NULL;
    fcml_st_disassembler *disasm = NULL;
    TASK_CHECK(make_disasm(0, &dialect, &disasm) == FCML_CEH_GEC_NO_ERROR);

    for (int i = 0; i < 3; i++) {
        fcml_st_disassembler_result result;
        TASK_CHECK(decode_one(defs[i], disasm, &result)
                == FCML_CEH_GEC_NO_ERROR);
        /* The one-byte imm8 must not be widened in the encoded field. */
        const fcml_st_encoded_field *field = field_of(&result, 1, 0);
        TASK_CHECK(result.instruction_details.operand_details[1]
                .encoded_field_count == 1);
        TASK_CHECK(field->kind == FCML_EFK_IMMEDIATE);
        TASK_CHECK(field->offset == expected_offset[i]);
        TASK_CHECK(field->size == 1);
        TASK_CHECK(field->raw_value == (fcml_uint64_t) 0xff);
        fcml_fn_disassembler_result_free(&result);
    }

    fcml_fn_disassembler_free(disasm);
    fcml_fn_dialect_free(dialect);
    return 0;
}

static int run_disp_and_immediate(void) {
    struct bytes_def def = {b_disp_imm, sizeof(b_disp_imm), FCML_OM_32_BIT};
    fcml_st_dialect *dialect = NULL;
    fcml_st_disassembler *disasm = NULL;
    TASK_CHECK(make_disasm(0, &dialect, &disasm) == FCML_CEH_GEC_NO_ERROR);

    fcml_st_disassembler_result result;
    TASK_CHECK(decode_one(&def, disasm, &result) == FCML_CEH_GEC_NO_ERROR);
    TASK_CHECK(result.instruction_details.instruction_size == 5);

    const fcml_st_encoded_field *disp = field_of(&result, 0, 0);
    const fcml_st_encoded_field *imm = field_of(&result, 1, 0);
    TASK_CHECK(disp->kind == FCML_EFK_MODRM_DISPLACEMENT);
    TASK_CHECK(disp->offset == 3 && disp->size == 1);
    TASK_CHECK(disp->raw_value == (fcml_uint64_t) 0xf0);
    TASK_CHECK(imm->kind == FCML_EFK_IMMEDIATE);
    TASK_CHECK(imm->offset == 4 && imm->size == 1);
    TASK_CHECK(imm->raw_value == (fcml_uint64_t) 0x7f);

    fcml_fn_disassembler_result_free(&result);
    fcml_fn_disassembler_free(disasm);
    fcml_fn_dialect_free(dialect);
    return 0;
}

static int run_branch(void) {
    struct bytes_def def32 = {b_branch32, sizeof(b_branch32),
            FCML_OM_64_BIT};
    struct bytes_def def8 = {b_branch8, sizeof(b_branch8), FCML_OM_32_BIT};

    fcml_st_dialect *dialect = NULL;
    fcml_st_disassembler *disasm = NULL;
    TASK_CHECK(make_disasm(0, &dialect, &disasm) == FCML_CEH_GEC_NO_ERROR);

    fcml_st_disassembler_result result;
    TASK_CHECK(decode_one(&def32, disasm, &result)
            == FCML_CEH_GEC_NO_ERROR);
    const fcml_st_encoded_field *rel = field_of(&result, 0, 0);
    TASK_CHECK(rel->kind == FCML_EFK_RELATIVE_BRANCH);
    TASK_CHECK(rel->offset == 1 && rel->size == 4);
    /* The raw relative displacement is kept, not the absolute target. */
    TASK_CHECK(rel->raw_value == (fcml_uint64_t) 0x01);
    fcml_fn_disassembler_result_free(&result);

    TASK_CHECK(decode_one(&def8, disasm, &result)
            == FCML_CEH_GEC_NO_ERROR);
    rel = field_of(&result, 0, 0);
    TASK_CHECK(rel->kind == FCML_EFK_RELATIVE_BRANCH);
    TASK_CHECK(rel->offset == 1 && rel->size == 1);
    TASK_CHECK(rel->raw_value == (fcml_uint64_t) 0x02);
    fcml_fn_disassembler_result_free(&result);

    fcml_fn_disassembler_free(disasm);
    fcml_fn_dialect_free(dialect);
    return 0;
}

static int run_moffs(void) {
    struct bytes_def def32 = {b_moffs32, sizeof(b_moffs32), FCML_OM_32_BIT};
    struct bytes_def def64 = {b_moffs64, sizeof(b_moffs64), FCML_OM_64_BIT};

    fcml_st_dialect *dialect = NULL;
    fcml_st_disassembler *disasm = NULL;
    TASK_CHECK(make_disasm(0, &dialect, &disasm) == FCML_CEH_GEC_NO_ERROR);

    fcml_st_disassembler_result result;
    TASK_CHECK(decode_one(&def32, disasm, &result)
            == FCML_CEH_GEC_NO_ERROR);
    const fcml_st_encoded_field *moffs = field_of(&result, 1, 0);
    TASK_CHECK(moffs->kind == FCML_EFK_MOFFS);
    TASK_CHECK(moffs->offset == 1 && moffs->size == 4);
    TASK_CHECK(moffs->raw_value == (fcml_uint64_t) 0x12345678);
    fcml_fn_disassembler_result_free(&result);

    TASK_CHECK(decode_one(&def64, disasm, &result)
            == FCML_CEH_GEC_NO_ERROR);
    moffs = field_of(&result, 1, 0);
    TASK_CHECK(moffs->kind == FCML_EFK_MOFFS);
    TASK_CHECK(moffs->offset == 2 && moffs->size == 8);
    TASK_CHECK(moffs->raw_value == (fcml_uint64_t) 0x12345678);
    fcml_fn_disassembler_result_free(&result);

    fcml_fn_disassembler_free(disasm);
    fcml_fn_dialect_free(dialect);
    return 0;
}

static int run_far_pointer(void) {
    struct bytes_def def32 = {b_far32, sizeof(b_far32), FCML_OM_32_BIT};
    struct bytes_def def16 = {b_far16, sizeof(b_far16), FCML_OM_16_BIT};

    fcml_st_dialect *dialect = NULL;
    fcml_st_disassembler *disasm = NULL;
    TASK_CHECK(make_disasm(0, &dialect, &disasm) == FCML_CEH_GEC_NO_ERROR);

    fcml_st_disassembler_result result;
    TASK_CHECK(decode_one(&def32, disasm, &result)
            == FCML_CEH_GEC_NO_ERROR);
    TASK_CHECK(result.instruction_details.operand_details[0]
            .encoded_field_count == 2);
    /* Physical order: offset first, segment second. */
    const fcml_st_encoded_field *offset = field_of(&result, 0, 0);
    const fcml_st_encoded_field *segment = field_of(&result, 0, 1);
    TASK_CHECK(offset->kind == FCML_EFK_FAR_OFFSET);
    TASK_CHECK(offset->offset == 1 && offset->size == 4);
    TASK_CHECK(offset->raw_value == (fcml_uint64_t) 0x12345678);
    TASK_CHECK(segment->kind == FCML_EFK_FAR_SEGMENT);
    TASK_CHECK(segment->offset == 5 && segment->size == 2);
    TASK_CHECK(segment->raw_value == (fcml_uint64_t) 0xbc9a);
    fcml_fn_disassembler_result_free(&result);

    TASK_CHECK(decode_one(&def16, disasm, &result)
            == FCML_CEH_GEC_NO_ERROR);
    offset = field_of(&result, 0, 0);
    segment = field_of(&result, 0, 1);
    TASK_CHECK(offset->kind == FCML_EFK_FAR_OFFSET);
    TASK_CHECK(offset->offset == 1 && offset->size == 2);
    TASK_CHECK(offset->raw_value == (fcml_uint64_t) 0x5678);
    TASK_CHECK(segment->kind == FCML_EFK_FAR_SEGMENT);
    TASK_CHECK(segment->offset == 3 && segment->size == 2);
    TASK_CHECK(segment->raw_value == (fcml_uint64_t) 0xbc9a);
    fcml_fn_disassembler_result_free(&result);

    fcml_fn_disassembler_free(disasm);
    fcml_fn_dialect_free(dialect);
    return 0;
}

static int run_compressed_disp8(void) {
    struct bytes_def def = {b_evex, sizeof(b_evex), FCML_OM_64_BIT};
    fcml_st_dialect *dialect = NULL;
    fcml_st_disassembler *disasm = NULL;
    TASK_CHECK(make_disasm(0, &dialect, &disasm) == FCML_CEH_GEC_NO_ERROR);

    fcml_st_disassembler_result result;
    TASK_CHECK(decode_one(&def, disasm, &result) == FCML_CEH_GEC_NO_ERROR);
    TASK_CHECK(result.instruction_details.instruction_size == 8);

    /* The memory operand is operand 2 in Intel order. */
    const fcml_st_encoded_field *disp = field_of(&result, 2, 0);
    TASK_CHECK(disp->kind == FCML_EFK_MODRM_DISPLACEMENT);
    TASK_CHECK(disp->offset == 7 && disp->size == 1);
    /* Raw byte is preserved before disp8*N scaling. */
    TASK_CHECK(disp->raw_value == (fcml_uint64_t) 0x08);

    /* Semantic value and N keep their existing meaning. */
    const fcml_st_raw_displacement *raw =
            &(result.instruction_details.modrm_details.displacement);
    TASK_CHECK(raw->N.is_not_null == FCML_TRUE);
    TASK_CHECK(raw->N.value == 8);
    TASK_CHECK(result.instruction.operands[2].address.effective_address
            .displacement.int32 == 64);

    fcml_fn_disassembler_result_free(&result);
    fcml_fn_disassembler_free(disasm);
    fcml_fn_dialect_free(dialect);
    return 0;
}

/*********************
 * identity group.   *
 *********************/

struct text_case {
    const char *name;
    struct bytes_def def;
    const char *intel;
    const char *gas;
};

static const struct text_case text_cases[] = {
    {"imm32", {b_imm32, sizeof(b_imm32), FCML_OM_32_BIT},
            "add eax,0ffffffffh", "add $0xffffffff,%eax"},
    {"imm16", {b_imm16, sizeof(b_imm16), FCML_OM_16_BIT},
            "add ax,0ffffh", "add $0xffff,%ax"},
    {"imm64", {b_imm64, sizeof(b_imm64), FCML_OM_64_BIT},
            "add rax,0ffffffffffffffffh",
            "add $0xffffffffffffffff,%rax"},
    {"disp_imm", {b_disp_imm, sizeof(b_disp_imm), FCML_OM_32_BIT},
            "add dword ptr [eax+ecx*4+0f0h],7fh",
            "addl $0x7f,0xf0(%eax,%ecx,4)"},
    {"branch32", {b_branch32, sizeof(b_branch32), FCML_OM_64_BIT},
            "call 1006h", "callq 0x1006"},
    {"branch8", {b_branch8, sizeof(b_branch8), FCML_OM_32_BIT},
            "jmp 1004h", "jmp 0x1004"},
    {"moffs32", {b_moffs32, sizeof(b_moffs32), FCML_OM_32_BIT},
            "mov eax,dword ptr [12345678h]",
            "mov 0x12345678,%eax"},
    {"moffs64", {b_moffs64, sizeof(b_moffs64), FCML_OM_64_BIT},
            "mov rax,qword ptr [12345678h]",
            "movabs 0x12345678,%rax"},
    {"far32", {b_far32, sizeof(b_far32), FCML_OM_32_BIT},
            "jmp far 0bc9ah:12345678h",
            "ljmpl $0xbc9a,$0x12345678"},
    {"far16", {b_far16, sizeof(b_far16), FCML_OM_16_BIT},
            "jmp far 0bc9ah:5678h", "ljmpw $0xbc9a,$0x5678"},
    {"compressed_disp8", {b_evex, sizeof(b_evex), FCML_OM_64_BIT},
            "vpermilpd zmm1,zmm2,mmword ptr [rsp+40h]{1to8}",
            "vpermilpd 0x40(%rsp){1to8},%zmm2,%zmm1"},
};

static int run_dialect_text_and_origins(void) {
    for (unsigned i = 0; i < sizeof(text_cases) / sizeof(text_cases[0]);
            i++) {
        const struct text_case *tc = &text_cases[i];

        fcml_st_dialect *intel_dialect = NULL;
        fcml_st_dialect *gas_dialect = NULL;
        fcml_st_disassembler *intel_disasm = NULL;
        fcml_st_disassembler *gas_disasm = NULL;
        TASK_CHECK(make_disasm(0, &intel_dialect, &intel_disasm)
                == FCML_CEH_GEC_NO_ERROR);
        TASK_CHECK(make_disasm(1, &gas_dialect, &gas_disasm)
                == FCML_CEH_GEC_NO_ERROR);

        fcml_st_disassembler_result intel_result;
        fcml_st_disassembler_result gas_result;
        TASK_CHECK(decode_one(&tc->def, intel_disasm, &intel_result)
                == FCML_CEH_GEC_NO_ERROR);
        TASK_CHECK(decode_one(&tc->def, gas_disasm, &gas_result)
                == FCML_CEH_GEC_NO_ERROR);

        /* Without a resolver the text must be identical in both dialects. */
        char intel_text[FCML_REND_MAX_BUFF_LEN];
        char gas_text[FCML_REND_MAX_BUFF_LEN];
        render_text(intel_dialect, NULL, NULL, &intel_result, intel_text,
                sizeof(intel_text));
        render_text(gas_dialect, NULL, NULL, &gas_result, gas_text,
                sizeof(gas_text));
        TASK_CHECK(strcmp(intel_text, tc->intel) == 0);
        TASK_CHECK(strcmp(gas_text, tc->gas) == 0);

        /* Operand reversal keeps pointing at the same encoded bytes. */
        struct flat_field intel_fields[FCML_DASM_ENCODED_FIELDS_PER_OPERAND
                * FCML_OPERANDS_COUNT];
        struct flat_field gas_fields[FCML_DASM_ENCODED_FIELDS_PER_OPERAND
                * FCML_OPERANDS_COUNT];
        int intel_count = collect_fields(&intel_result, intel_fields,
                sizeof(intel_fields) / sizeof(intel_fields[0]));
        int gas_count = collect_fields(&gas_result, gas_fields,
                sizeof(gas_fields) / sizeof(gas_fields[0]));
        TASK_CHECK(same_flat(intel_fields, intel_count, gas_fields,
                gas_count) == 1);
        TASK_CHECK(intel_count >= 1);

        fcml_fn_disassembler_result_free(&intel_result);
        fcml_fn_disassembler_result_free(&gas_result);
        fcml_fn_disassembler_free(intel_disasm);
        fcml_fn_disassembler_free(gas_disasm);
        fcml_fn_dialect_free(intel_dialect);
        fcml_fn_dialect_free(gas_dialect);
    }
    return 0;
}

static int run_semantic_values(void) {
    fcml_st_dialect *dialect = NULL;
    fcml_st_disassembler *disasm = NULL;
    TASK_CHECK(make_disasm(0, &dialect, &disasm) == FCML_CEH_GEC_NO_ERROR);

    fcml_st_disassembler_result result;

    /* Sign extended immediate keeps its 32-bit semantic value. */
    struct bytes_def imm_def = {b_imm32, sizeof(b_imm32), FCML_OM_32_BIT};
    TASK_CHECK(decode_one(&imm_def, disasm, &result)
            == FCML_CEH_GEC_NO_ERROR);
    TASK_CHECK(result.instruction.operands[1].immediate.size == FCML_DS_32);
    TASK_CHECK(result.instruction.operands[1].immediate.int32 == -1);
    fcml_fn_disassembler_result_free(&result);

    /* Displacement and immediate semantic values. */
    struct bytes_def disp_def = {b_disp_imm, sizeof(b_disp_imm),
            FCML_OM_32_BIT};
    TASK_CHECK(decode_one(&disp_def, disasm, &result)
            == FCML_CEH_GEC_NO_ERROR);
    TASK_CHECK(result.instruction.operands[0].address.effective_address
            .displacement.int8 == -16);
    TASK_CHECK(result.instruction.operands[1].immediate.int8 == 0x7f);
    fcml_fn_disassembler_result_free(&result);

    /* Branch target is the absolute address. */
    struct bytes_def branch_def = {b_branch32, sizeof(b_branch32),
            FCML_OM_64_BIT};
    TASK_CHECK(decode_one(&branch_def, disasm, &result)
            == FCML_CEH_GEC_NO_ERROR);
    TASK_CHECK(result.instruction.operands[0].address.offset.off64
            == (fcml_int64_t) 0x1006);
    fcml_fn_disassembler_result_free(&result);

    /* moffs semantic value. */
    struct bytes_def moffs_def = {b_moffs32, sizeof(b_moffs32),
            FCML_OM_32_BIT};
    TASK_CHECK(decode_one(&moffs_def, disasm, &result)
            == FCML_CEH_GEC_NO_ERROR);
    TASK_CHECK(result.instruction.operands[1].address.offset.off32
            == (fcml_int32_t) 0x12345678);
    fcml_fn_disassembler_result_free(&result);

    /* Far pointer semantic values. */
    struct bytes_def far_def = {b_far32, sizeof(b_far32), FCML_OM_32_BIT};
    TASK_CHECK(decode_one(&far_def, disasm, &result)
            == FCML_CEH_GEC_NO_ERROR);
    TASK_CHECK(result.instruction.operands[0].far_pointer.offset32
            == (fcml_int32_t) 0x12345678);
    TASK_CHECK(result.instruction.operands[0].far_pointer.segment
            == (fcml_uint16_t) 0xbc9a);
    fcml_fn_disassembler_result_free(&result);

    fcml_fn_disassembler_free(disasm);
    fcml_fn_dialect_free(dialect);
    return 0;
}

static int run_reuse_isolation(void) {
    fcml_st_dialect *dialect = NULL;
    fcml_st_disassembler *disasm = NULL;
    TASK_CHECK(make_disasm(0, &dialect, &disasm) == FCML_CEH_GEC_NO_ERROR);

    fcml_st_disassembler_result result;

    /* A complete far instruction populates two fields. */
    struct bytes_def far_def = {b_far32, sizeof(b_far32), FCML_OM_32_BIT};
    TASK_CHECK(decode_one(&far_def, disasm, &result)
            == FCML_CEH_GEC_NO_ERROR);
    TASK_CHECK(total_field_count(&result) == 2);

    /* Truncated input in the same holder fails and leaves no origins. */
    struct bytes_def cut_def = {b_cut, sizeof(b_cut), FCML_OM_32_BIT};
    TASK_CHECK(decode_one(&cut_def, disasm, &result)
            == FCML_CEH_GEC_EOF);
    TASK_CHECK(total_field_count(&result) == 0);

    /* The holder can be used again; the new decode owns its own fields. */
    struct bytes_def imm_def = {b_imm32, sizeof(b_imm32), FCML_OM_32_BIT};
    TASK_CHECK(decode_one(&imm_def, disasm, &result)
            == FCML_CEH_GEC_NO_ERROR);
    TASK_CHECK(total_field_count(&result) == 1);
    const fcml_st_encoded_field *field = field_of(&result, 1, 0);
    TASK_CHECK(field->kind == FCML_EFK_IMMEDIATE);
    TASK_CHECK(field->offset == 2 && field->size == 1);
    TASK_CHECK(field->raw_value == (fcml_uint64_t) 0xff);
    fcml_fn_disassembler_result_free(&result);

    /* Register-only operands never produce fields. */
    fcml_uint8_t reg_reg[] = {0x89, 0xd8}; /* mov eax, ebx */
    struct bytes_def reg_def = {reg_reg, sizeof(reg_reg), FCML_OM_32_BIT};
    TASK_CHECK(decode_one(&reg_def, disasm, &result)
            == FCML_CEH_GEC_NO_ERROR);
    TASK_CHECK(result.instruction.operands_count == 2);
    TASK_CHECK(total_field_count(&result) == 0);
    fcml_fn_disassembler_result_free(&result);

    fcml_fn_disassembler_free(disasm);
    fcml_fn_dialect_free(dialect);
    return 0;
}

/*******************
 * symbols group.  *
 *******************/

struct resolver_state {
    int calls;
};

/* Plain immediates are rejected by kind; every other backed number gets a
 * symbol. The returned strings are static, so they stay valid for the whole
 * fcml_fn_render() call. */
static fcml_string test_resolver(fcml_ptr context, fcml_int operand_index,
        const fcml_st_encoded_field *field,
        const fcml_st_integer *value) {
    (void) operand_index;
    (void) value;
    struct resolver_state *state = (struct resolver_state *) context;
    state->calls++;
    switch (field->kind) {
    case FCML_EFK_IMMEDIATE:
        return NULL;
    case FCML_EFK_RELATIVE_BRANCH:
        return "target";
    case FCML_EFK_MODRM_DISPLACEMENT:
        return "disp_sym";
    case FCML_EFK_MOFFS:
        return "moffs_sym";
    case FCML_EFK_FAR_OFFSET:
        return "off_sym";
    case FCML_EFK_FAR_SEGMENT:
        return "seg_sym";
    }
    return NULL;
}

struct symbol_case {
    const char *name;
    struct bytes_def def;
    int expected_calls;
    const char *intel;
    const char *gas;
};

static const struct symbol_case symbol_cases[] = {
    {"imm_rejected", {b_imm32, sizeof(b_imm32), FCML_OM_32_BIT}, 1,
            "add eax,0ffffffffh", "add $0xffffffff,%eax"},
    {"disp_imm", {b_disp_imm, sizeof(b_disp_imm), FCML_OM_32_BIT}, 2,
            "add dword ptr [eax+ecx*4+disp_sym],7fh",
            "addl $0x7f,disp_sym(%eax,%ecx,4)"},
    {"branch", {b_branch32, sizeof(b_branch32), FCML_OM_64_BIT}, 1,
            "call target", "callq target"},
    {"moffs", {b_moffs32, sizeof(b_moffs32), FCML_OM_32_BIT}, 1,
            "mov eax,dword ptr [moffs_sym]", "mov moffs_sym,%eax"},
    {"far", {b_far32, sizeof(b_far32), FCML_OM_32_BIT}, 2,
            "jmp far seg_sym:off_sym", "ljmpl $seg_sym,$off_sym"},
    {"compressed_disp8", {b_evex, sizeof(b_evex), FCML_OM_64_BIT}, 1,
            "vpermilpd zmm1,zmm2,mmword ptr [rsp+disp_sym]{1to8}",
            "vpermilpd disp_sym(%rsp){1to8},%zmm2,%zmm1"},
};

static int run_symbol_resolution(void) {
    for (unsigned i = 0; i < sizeof(symbol_cases) / sizeof(symbol_cases[0]);
            i++) {
        const struct symbol_case *sc = &symbol_cases[i];

        fcml_st_dialect *intel_dialect = NULL;
        fcml_st_dialect *gas_dialect = NULL;
        fcml_st_disassembler *intel_disasm = NULL;
        fcml_st_disassembler *gas_disasm = NULL;
        TASK_CHECK(make_disasm(0, &intel_dialect, &intel_disasm)
                == FCML_CEH_GEC_NO_ERROR);
        TASK_CHECK(make_disasm(1, &gas_dialect, &gas_disasm)
                == FCML_CEH_GEC_NO_ERROR);

        fcml_st_disassembler_result intel_result;
        fcml_st_disassembler_result gas_result;
        TASK_CHECK(decode_one(&sc->def, intel_disasm, &intel_result)
                == FCML_CEH_GEC_NO_ERROR);
        TASK_CHECK(decode_one(&sc->def, gas_disasm, &gas_result)
                == FCML_CEH_GEC_NO_ERROR);

        struct resolver_state intel_state = {0};
        struct resolver_state gas_state = {0};
        char intel_text[FCML_REND_MAX_BUFF_LEN];
        char gas_text[FCML_REND_MAX_BUFF_LEN];
        render_text(intel_dialect, test_resolver, &intel_state, &intel_result,
                intel_text, sizeof(intel_text));
        render_text(gas_dialect, test_resolver, &gas_state, &gas_result,
                gas_text, sizeof(gas_text));
        TASK_CHECK(strcmp(intel_text, sc->intel) == 0);
        TASK_CHECK(strcmp(gas_text, sc->gas) == 0);

        /* Each backed number triggers exactly one callback per render. */
        TASK_CHECK(intel_state.calls == sc->expected_calls);
        TASK_CHECK(gas_state.calls == sc->expected_calls);

        /* With the resolver detached the default numbers come back. This is
         * asserted exhaustively for every dialect in the identity group. */
        render_text(intel_dialect, NULL, NULL, &intel_result, intel_text,
                sizeof(intel_text));
        render_text(gas_dialect, NULL, NULL, &gas_result, gas_text,
                sizeof(gas_text));
        TASK_CHECK(strchr(intel_text, '_') == NULL);
        TASK_CHECK(strchr(gas_text, '_') == NULL);

        fcml_fn_disassembler_result_free(&intel_result);
        fcml_fn_disassembler_result_free(&gas_result);
        fcml_fn_disassembler_free(intel_disasm);
        fcml_fn_disassembler_free(gas_disasm);
        fcml_fn_dialect_free(intel_dialect);
        fcml_fn_dialect_free(gas_dialect);
    }
    return 0;
}

static int run_far_two_callbacks(void) {
    struct bytes_def def = {b_far32, sizeof(b_far32), FCML_OM_32_BIT};
    fcml_st_dialect *dialect = NULL;
    fcml_st_disassembler *disasm = NULL;
    TASK_CHECK(make_disasm(0, &dialect, &disasm) == FCML_CEH_GEC_NO_ERROR);

    fcml_st_disassembler_result result;
    TASK_CHECK(decode_one(&def, disasm, &result) == FCML_CEH_GEC_NO_ERROR);

    struct resolver_state state = {0};
    fcml_st_render_config config;
    memset(&config, 0, sizeof(config));
    config.render_flags = FIELD_REND_FLAGS;
    config.symbol_resolver = test_resolver;
    config.symbol_context = &state;
    char text[FCML_REND_MAX_BUFF_LEN];
    TASK_CHECK(fcml_fn_render(dialect, &config, text, sizeof(text), &result)
            == FCML_CEH_GEC_NO_ERROR);
    /* Segment and offset are resolved independently. */
    TASK_CHECK(state.calls == 2);
    TASK_CHECK(strcmp(text, "jmp far seg_sym:off_sym") == 0);

    fcml_fn_disassembler_result_free(&result);
    fcml_fn_disassembler_free(disasm);
    fcml_fn_dialect_free(dialect);
    return 0;
}

const struct task_case task_cases[] = {
    {"encoding", "short_immediates", run_short_immediates},
    {"encoding", "disp_and_immediate", run_disp_and_immediate},
    {"encoding", "branch", run_branch},
    {"encoding", "moffs", run_moffs},
    {"encoding", "far_pointer", run_far_pointer},
    {"encoding", "compressed_disp8", run_compressed_disp8},

    {"identity", "dialect_text_and_origins", run_dialect_text_and_origins},
    {"identity", "semantic_values", run_semantic_values},
    {"identity", "reuse_isolation", run_reuse_isolation},

    {"symbols", "symbol_resolution", run_symbol_resolution},
    {"symbols", "far_two_callbacks", run_far_two_callbacks},
};

const size_t task_case_count =
        sizeof(task_cases) / sizeof(task_cases[0]);
