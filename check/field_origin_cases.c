/*
 * FCML - Free Code Manipulation Library.
 * Copyright (C) 2010-2024 Slawomir Wojtasiak
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 * USA
 */

/* Regression tests for encoded-field provenance and symbol resolution.
 * The analyzed instructions are synthetic byte sequences treated purely
 * as data; they are never executed. */

#include "../task_support/check.h"

#include <fcml_disassembler.h>
#include <fcml_intel_dialect.h>
#include <fcml_gas_dialect.h>
#include <fcml_renderer.h>

#include <string.h>

/* Test instruction pointer, shared by every sample. */
#define TEST_IP 0x1000

static int make_disassembler(int gas, fcml_st_dialect **dialect,
        fcml_st_disassembler **disassembler) {
    int rc = gas ? fcml_fn_dialect_init_gas(0, dialect)
            : fcml_fn_dialect_init_intel(0, dialect);
    if (rc) {
        return rc;
    }
    return fcml_fn_disassembler_init(*dialect, disassembler);
}

static void free_disassembler(fcml_st_dialect *dialect,
        fcml_st_disassembler *disassembler) {
    fcml_fn_disassembler_free(disassembler);
    fcml_fn_dialect_free(dialect);
}

/* Decodes one instruction into a reused, freshly prepared result. */
static int decode(fcml_st_disassembler *disassembler,
        fcml_en_operating_mode mode, const fcml_uint8_t *bytes,
        fcml_usize length, fcml_st_disassembler_result *result) {
    fcml_st_disassembler_context context = { 0 };
    context.disassembler = disassembler;
    context.entry_point.op_mode = mode;
    context.entry_point.ip = TEST_IP;
    context.code = (fcml_ptr) bytes;
    context.code_length = length;
    fcml_fn_disassembler_result_prepare(result);
    return fcml_fn_disassemble(&context, result);
}

static int render_text(int gas, fcml_st_dialect *dialect,
        const fcml_st_disassembler_result *result,
        fcml_fnp_symbol_resolver resolver, fcml_ptr resolver_context,
        char *buffer, fcml_usize buffer_length) {
    fcml_st_render_config config = { 0 };
    config.render_flags = FCML_REND_FLAG_REMOVE_LEADING_ZEROS
            | FCML_REND_FLAG_HEX_IMM | FCML_REND_FLAG_HEX_DISPLACEMENT;
    config.symbol_resolver = resolver;
    config.symbol_context = resolver_context;
    (void) gas;
    return fcml_fn_render(dialect, &config, buffer, buffer_length, result);
}

/* Signed decimal rendering: no HEX flags, so sign-extended values keep
 * their natural signed notation. */
static int render_signed_text(fcml_st_dialect *dialect,
        const fcml_st_disassembler_result *result, char *buffer,
        fcml_usize buffer_length) {
    fcml_st_render_config config = { 0 };
    return fcml_fn_render(dialect, &config, buffer, buffer_length, result);
}

static const fcml_st_encoded_field *field_of(
        const fcml_st_disassembler_result *result, int operand_index,
        fcml_en_encoded_field_kind kind) {
    const fcml_st_operand_details *details =
            &(result->instruction_details.operand_details[operand_index]);
    for (int i = 0; i < details->encoded_field_count; i++) {
        if (details->encoded_fields[i].kind == kind) {
            return &(details->encoded_fields[i]);
        }
    }
    return NULL;
}

/* Symbol resolver state: per-kind call counters, a render-order log and
 * the semantic integer actually presented for rendering. */
struct symbol_context {
    unsigned calls[6];
    fcml_int index_log[8];
    fcml_en_encoded_field_kind kind_log[8];
    int log_count;
    fcml_uint64_t last_value;
    fcml_usize last_value_size;
};

static const fcml_char *test_symbol_resolver(fcml_ptr user_context,
        fcml_int operand_index, const fcml_st_encoded_field *field,
        const fcml_st_integer *value) {
    struct symbol_context *context =
            (struct symbol_context *) user_context;
    context->calls[field->kind]++;
    if (context->log_count < 8) {
        context->index_log[context->log_count] = operand_index;
        context->kind_log[context->log_count] = field->kind;
        context->log_count++;
    }
    context->last_value_size = value->size;
    switch (value->size) {
    case FCML_DS_8:
        context->last_value = (fcml_uint8_t) value->int8;
        break;
    case FCML_DS_16:
        context->last_value = (fcml_uint16_t) value->int16;
        break;
    case FCML_DS_32:
        context->last_value = (fcml_uint32_t) value->int32;
        break;
    case FCML_DS_64:
        context->last_value = (fcml_uint64_t) value->int64;
        break;
    default:
        context->last_value = 0;
        break;
    }
    /* Plain immediates are rejected on purpose. */
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

/*****************************
 * Group: encoding origins.
 *****************************/

static int case_short_imm(void) {
    fcml_st_dialect *dialect = NULL;
    fcml_st_disassembler *disassembler = NULL;
    TASK_CHECK(make_disassembler(0, &dialect, &disassembler) == 0);
    fcml_st_disassembler_result result;
    fcml_uint8_t code[] = { 0x83, 0xc0, 0xff }; /* add eax,-1 */
    TASK_CHECK(decode(disassembler, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    /* The register operand has no encoding field. */
    TASK_CHECK(result.instruction_details.operand_details[0]
            .encoded_field_count == 0);
    const fcml_st_encoded_field *field = field_of(&result, 1,
            FCML_EFK_IMMEDIATE);
    TASK_CHECK(field != NULL);
    TASK_CHECK(field->offset == 2);
    TASK_CHECK(field->size == 1);
    TASK_CHECK(field->raw_value == 0xff);
    TASK_CHECK(result.instruction_details.operand_details[1]
            .encoded_field_count == 1);
    free_disassembler(dialect, disassembler);
    return 0;
}

static int case_disp_and_imm(void) {
    fcml_st_dialect *dialect = NULL;
    fcml_st_disassembler *disassembler = NULL;
    TASK_CHECK(make_disassembler(0, &dialect, &disassembler) == 0);
    fcml_st_disassembler_result result;
    /* add dword ptr [eax+ecx*4-0x10],0x7f */
    fcml_uint8_t code[] = { 0x83, 0x44, 0x88, 0xf0, 0x7f };
    TASK_CHECK(decode(disassembler, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    const fcml_st_encoded_field *disp = field_of(&result, 0,
            FCML_EFK_MODRM_DISPLACEMENT);
    TASK_CHECK(disp != NULL);
    TASK_CHECK(disp->offset == 3);
    TASK_CHECK(disp->size == 1);
    TASK_CHECK(disp->raw_value == 0xf0);
    const fcml_st_encoded_field *imm = field_of(&result, 1,
            FCML_EFK_IMMEDIATE);
    TASK_CHECK(imm != NULL);
    TASK_CHECK(imm->offset == 4);
    TASK_CHECK(imm->size == 1);
    TASK_CHECK(imm->raw_value == 0x7f);
    free_disassembler(dialect, disassembler);
    return 0;
}

static int case_rip_relative(void) {
    fcml_st_dialect *dialect = NULL;
    fcml_st_disassembler *disassembler = NULL;
    TASK_CHECK(make_disassembler(0, &dialect, &disassembler) == 0);
    fcml_st_disassembler_result result;
    /* add qword ptr [rip+0x11],-1 */
    fcml_uint8_t code[] = { 0x48, 0x83, 0x05, 0x11, 0x00, 0x00, 0x00, 0xff };
    TASK_CHECK(decode(disassembler, FCML_OM_64_BIT, code, sizeof(code),
            &result) == 0);
    /* The field keeps the raw relative displacement, not the target. */
    const fcml_st_encoded_field *disp = field_of(&result, 0,
            FCML_EFK_MODRM_DISPLACEMENT);
    TASK_CHECK(disp != NULL);
    TASK_CHECK(disp->offset == 3);
    TASK_CHECK(disp->size == 4);
    TASK_CHECK(disp->raw_value == 0x11);
    const fcml_st_encoded_field *imm = field_of(&result, 1,
            FCML_EFK_IMMEDIATE);
    TASK_CHECK(imm != NULL);
    TASK_CHECK(imm->offset == 7);
    TASK_CHECK(imm->size == 1);
    TASK_CHECK(imm->raw_value == 0xff);
    free_disassembler(dialect, disassembler);
    return 0;
}

static int case_branch_rel32(void) {
    fcml_st_dialect *dialect = NULL;
    fcml_st_disassembler *disassembler = NULL;
    TASK_CHECK(make_disassembler(0, &dialect, &disassembler) == 0);
    fcml_st_disassembler_result result;
    fcml_uint8_t code[] = { 0xe8, 0x01, 0x00, 0x00, 0x00 }; /* call rel32 */
    TASK_CHECK(decode(disassembler, FCML_OM_64_BIT, code, sizeof(code),
            &result) == 0);
    const fcml_st_encoded_field *field = field_of(&result, 0,
            FCML_EFK_RELATIVE_BRANCH);
    TASK_CHECK(field != NULL);
    TASK_CHECK(field->offset == 1);
    TASK_CHECK(field->size == 4);
    TASK_CHECK(field->raw_value == 1);
    free_disassembler(dialect, disassembler);
    return 0;
}

static int case_branch_rel16(void) {
    fcml_st_dialect *dialect = NULL;
    fcml_st_disassembler *disassembler = NULL;
    TASK_CHECK(make_disassembler(0, &dialect, &disassembler) == 0);
    fcml_st_disassembler_result result;
    fcml_uint8_t code[] = { 0xe9, 0x01, 0x00 }; /* jmp rel16 */
    TASK_CHECK(decode(disassembler, FCML_OM_16_BIT, code, sizeof(code),
            &result) == 0);
    const fcml_st_encoded_field *field = field_of(&result, 0,
            FCML_EFK_RELATIVE_BRANCH);
    TASK_CHECK(field != NULL);
    TASK_CHECK(field->offset == 1);
    TASK_CHECK(field->size == 2);
    TASK_CHECK(field->raw_value == 1);
    free_disassembler(dialect, disassembler);
    return 0;
}

static int case_moffs32(void) {
    fcml_st_dialect *dialect = NULL;
    fcml_st_disassembler *disassembler = NULL;
    TASK_CHECK(make_disassembler(0, &dialect, &disassembler) == 0);
    fcml_st_disassembler_result result;
    fcml_uint8_t code[] = { 0xa1, 0x78, 0x56, 0x34, 0x12 };
    TASK_CHECK(decode(disassembler, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    const fcml_st_encoded_field *field = field_of(&result, 1,
            FCML_EFK_MOFFS);
    TASK_CHECK(field != NULL);
    TASK_CHECK(field->offset == 1);
    TASK_CHECK(field->size == 4);
    TASK_CHECK(field->raw_value == 0x12345678);
    free_disassembler(dialect, disassembler);
    return 0;
}

static int case_moffs16(void) {
    fcml_st_dialect *dialect = NULL;
    fcml_st_disassembler *disassembler = NULL;
    TASK_CHECK(make_disassembler(0, &dialect, &disassembler) == 0);
    fcml_st_disassembler_result result;
    fcml_uint8_t code[] = { 0xa1, 0x78, 0x56 };
    TASK_CHECK(decode(disassembler, FCML_OM_16_BIT, code, sizeof(code),
            &result) == 0);
    const fcml_st_encoded_field *field = field_of(&result, 1,
            FCML_EFK_MOFFS);
    TASK_CHECK(field != NULL);
    TASK_CHECK(field->offset == 1);
    TASK_CHECK(field->size == 2);
    TASK_CHECK(field->raw_value == 0x5678);
    free_disassembler(dialect, disassembler);
    return 0;
}

static int case_far32(void) {
    fcml_st_dialect *dialect = NULL;
    fcml_st_disassembler *disassembler = NULL;
    TASK_CHECK(make_disassembler(0, &dialect, &disassembler) == 0);
    fcml_st_disassembler_result result;
    /* jmp far 0xbc9a:0x12345678 */
    fcml_uint8_t code[] = { 0xea, 0x78, 0x56, 0x34, 0x12, 0x9a, 0xbc };
    TASK_CHECK(decode(disassembler, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    const fcml_st_operand_details *details =
            &(result.instruction_details.operand_details[0]);
    TASK_CHECK(details->encoded_field_count == 2);
    /* Physical order: offset first, segment second. */
    TASK_CHECK(details->encoded_fields[0].kind == FCML_EFK_FAR_OFFSET);
    TASK_CHECK(details->encoded_fields[0].offset == 1);
    TASK_CHECK(details->encoded_fields[0].size == 4);
    TASK_CHECK(details->encoded_fields[0].raw_value == 0x12345678);
    TASK_CHECK(details->encoded_fields[1].kind == FCML_EFK_FAR_SEGMENT);
    TASK_CHECK(details->encoded_fields[1].offset == 5);
    TASK_CHECK(details->encoded_fields[1].size == 2);
    TASK_CHECK(details->encoded_fields[1].raw_value == 0xbc9a);
    free_disassembler(dialect, disassembler);
    return 0;
}

static int case_far16(void) {
    fcml_st_dialect *dialect = NULL;
    fcml_st_disassembler *disassembler = NULL;
    TASK_CHECK(make_disassembler(0, &dialect, &disassembler) == 0);
    fcml_st_disassembler_result result;
    fcml_uint8_t code[] = { 0xea, 0x78, 0x56, 0x9a, 0xbc };
    TASK_CHECK(decode(disassembler, FCML_OM_16_BIT, code, sizeof(code),
            &result) == 0);
    const fcml_st_operand_details *details =
            &(result.instruction_details.operand_details[0]);
    TASK_CHECK(details->encoded_field_count == 2);
    TASK_CHECK(details->encoded_fields[0].kind == FCML_EFK_FAR_OFFSET);
    TASK_CHECK(details->encoded_fields[0].size == 2);
    TASK_CHECK(details->encoded_fields[0].raw_value == 0x5678);
    TASK_CHECK(details->encoded_fields[1].kind == FCML_EFK_FAR_SEGMENT);
    TASK_CHECK(details->encoded_fields[1].size == 2);
    TASK_CHECK(details->encoded_fields[1].raw_value == 0xbc9a);
    free_disassembler(dialect, disassembler);
    return 0;
}

static int case_compressed_disp8(void) {
    fcml_st_dialect *dialect = NULL;
    fcml_st_disassembler *disassembler = NULL;
    TASK_CHECK(make_disassembler(0, &dialect, &disassembler) == 0);
    fcml_st_disassembler_result result;
    /* vorps xmm1{k4}{z},xmm0,dword ptr [rsp+0x20]{1to4} */
    fcml_uint8_t code[] = { 0x62, 0xf1, 0x7c, 0x9c, 0x56, 0x4c, 0x24,
            0x08 };
    TASK_CHECK(decode(disassembler, FCML_OM_64_BIT, code, sizeof(code),
            &result) == 0);
    const fcml_st_encoded_field *field = field_of(&result, 2,
            FCML_EFK_MODRM_DISPLACEMENT);
    TASK_CHECK(field != NULL);
    TASK_CHECK(field->offset == 7);
    TASK_CHECK(field->size == 1);
    /* The pre-scaling byte, not the 0x20 semantic displacement. */
    TASK_CHECK(field->raw_value == 0x08);
    /* Register operands carry no fields. */
    TASK_CHECK(result.instruction_details.operand_details[0]
            .encoded_field_count == 0);
    TASK_CHECK(result.instruction_details.operand_details[1]
            .encoded_field_count == 0);
    free_disassembler(dialect, disassembler);
    return 0;
}

/*****************************
 * Group: identity.
 *****************************/

static int case_semantic_short_imm(void) {
    fcml_st_dialect *intel_dialect = NULL, *gas_dialect = NULL;
    fcml_st_disassembler *intel_dis = NULL, *gas_dis = NULL;
    TASK_CHECK(make_disassembler(0, &intel_dialect, &intel_dis) == 0);
    TASK_CHECK(make_disassembler(1, &gas_dialect, &gas_dis) == 0);
    fcml_st_disassembler_result result;
    fcml_uint8_t code[] = { 0x83, 0xc0, 0xff };
    TASK_CHECK(decode(intel_dis, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    /* Semantic immediate is sign extended to 32 bits, field stays 8. */
    const fcml_st_integer *immediate = &(result.instruction.operands[1]
            .immediate);
    TASK_CHECK(immediate->size == 32);
    TASK_CHECK(immediate->int32 == -1);
    TASK_CHECK(result.instruction_details.instruction_size == sizeof(code));
    TASK_CHECK(memcmp(result.instruction_details.instruction_code, code,
            sizeof(code)) == 0);
    char text[256];
    TASK_CHECK(render_text(0, intel_dialect, &result, NULL, NULL, text,
            sizeof(text)) == 0);
    TASK_CHECK(strcmp(text, "add eax,0ffffffffh") == 0);
    TASK_CHECK(decode(gas_dis, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    TASK_CHECK(render_text(1, gas_dialect, &result, NULL, NULL, text,
            sizeof(text)) == 0);
    TASK_CHECK(strcmp(text, "add $0xffffffff,%eax") == 0);
    free_disassembler(intel_dialect, intel_dis);
    free_disassembler(gas_dialect, gas_dis);
    return 0;
}

static int case_semantic_rip(void) {
    fcml_st_dialect *intel_dialect = NULL, *gas_dialect = NULL;
    fcml_st_disassembler *intel_dis = NULL, *gas_dis = NULL;
    TASK_CHECK(make_disassembler(0, &intel_dialect, &intel_dis) == 0);
    TASK_CHECK(make_disassembler(1, &gas_dialect, &gas_dis) == 0);
    fcml_st_disassembler_result result;
    fcml_uint8_t code[] = { 0x48, 0x83, 0x05, 0x11, 0x00, 0x00, 0x00,
            0xff };
    TASK_CHECK(decode(intel_dis, FCML_OM_64_BIT, code, sizeof(code),
            &result) == 0);
    /* RIP target is the converted value, the field keeps 0x11. */
    TASK_CHECK(result.instruction.operands[0].address.offset.off64
            == (fcml_int64_t) 0x1019);
    TASK_CHECK(result.instruction.operands[1].immediate.size == 64);
    TASK_CHECK(result.instruction.operands[1].immediate.int64 == -1);
    char text[256];
    TASK_CHECK(render_text(0, intel_dialect, &result, NULL, NULL, text,
            sizeof(text)) == 0);
    TASK_CHECK(strcmp(text,
            "add qword ptr [1019h],0ffffffffffffffffh") == 0);
    TASK_CHECK(decode(gas_dis, FCML_OM_64_BIT, code, sizeof(code),
            &result) == 0);
    TASK_CHECK(render_text(1, gas_dialect, &result, NULL, NULL, text,
            sizeof(text)) == 0);
    TASK_CHECK(strcmp(text,
            "addq $0xffffffffffffffff,0x11(%rip)") == 0);
    free_disassembler(intel_dialect, intel_dis);
    free_disassembler(gas_dialect, gas_dis);
    return 0;
}

static int case_semantic_branch(void) {
    fcml_st_dialect *intel_dialect = NULL, *gas_dialect = NULL;
    fcml_st_disassembler *intel_dis = NULL, *gas_dis = NULL;
    TASK_CHECK(make_disassembler(0, &intel_dialect, &intel_dis) == 0);
    TASK_CHECK(make_disassembler(1, &gas_dialect, &gas_dis) == 0);
    fcml_st_disassembler_result result;
    fcml_uint8_t code[] = { 0xe8, 0x01, 0x00, 0x00, 0x00 };
    TASK_CHECK(decode(intel_dis, FCML_OM_64_BIT, code, sizeof(code),
            &result) == 0);
    TASK_CHECK(result.instruction.operands[0].address.offset.off64
            == (fcml_int64_t) 0x1006);
    char text[256];
    TASK_CHECK(render_text(0, intel_dialect, &result, NULL, NULL, text,
            sizeof(text)) == 0);
    TASK_CHECK(strcmp(text, "call 1006h") == 0);
    TASK_CHECK(decode(gas_dis, FCML_OM_64_BIT, code, sizeof(code),
            &result) == 0);
    TASK_CHECK(render_text(1, gas_dialect, &result, NULL, NULL, text,
            sizeof(text)) == 0);
    TASK_CHECK(strcmp(text, "callq 0x1006") == 0);
    free_disassembler(intel_dialect, intel_dis);
    free_disassembler(gas_dialect, gas_dis);
    return 0;
}

static int case_semantic_moffs(void) {
    fcml_st_dialect *intel_dialect = NULL, *gas_dialect = NULL;
    fcml_st_disassembler *intel_dis = NULL, *gas_dis = NULL;
    TASK_CHECK(make_disassembler(0, &intel_dialect, &intel_dis) == 0);
    TASK_CHECK(make_disassembler(1, &gas_dialect, &gas_dis) == 0);
    fcml_st_disassembler_result result;
    fcml_uint8_t code[] = { 0xa1, 0x78, 0x56, 0x34, 0x12 };
    TASK_CHECK(decode(intel_dis, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    TASK_CHECK(result.instruction.operands[1].address.offset.off32
            == (fcml_int32_t) 0x12345678);
    char text[256];
    TASK_CHECK(render_text(0, intel_dialect, &result, NULL, NULL, text,
            sizeof(text)) == 0);
    TASK_CHECK(strcmp(text, "mov eax,dword ptr [12345678h]") == 0);
    TASK_CHECK(decode(gas_dis, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    TASK_CHECK(render_text(1, gas_dialect, &result, NULL, NULL, text,
            sizeof(text)) == 0);
    TASK_CHECK(strcmp(text, "mov 0x12345678,%eax") == 0);
    free_disassembler(intel_dialect, intel_dis);
    free_disassembler(gas_dialect, gas_dis);
    return 0;
}

static int case_semantic_far(void) {
    fcml_st_dialect *intel_dialect = NULL, *gas_dialect = NULL;
    fcml_st_disassembler *intel_dis = NULL, *gas_dis = NULL;
    TASK_CHECK(make_disassembler(0, &intel_dialect, &intel_dis) == 0);
    TASK_CHECK(make_disassembler(1, &gas_dialect, &gas_dis) == 0);
    fcml_st_disassembler_result result;
    fcml_uint8_t code[] = { 0xea, 0x78, 0x56, 0x34, 0x12, 0x9a, 0xbc };
    TASK_CHECK(decode(intel_dis, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    const fcml_st_far_pointer *far =
            &(result.instruction.operands[0].far_pointer);
    TASK_CHECK(far->segment == 0xbc9a);
    TASK_CHECK(far->offset_size == 32);
    TASK_CHECK(far->offset32 == (fcml_int32_t) 0x12345678);
    char text[256];
    TASK_CHECK(render_text(0, intel_dialect, &result, NULL, NULL, text,
            sizeof(text)) == 0);
    TASK_CHECK(strcmp(text, "jmp far 0bc9ah:12345678h") == 0);
    TASK_CHECK(decode(gas_dis, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    TASK_CHECK(render_text(1, gas_dialect, &result, NULL, NULL, text,
            sizeof(text)) == 0);
    TASK_CHECK(strcmp(text, "ljmpl $0xbc9a,$0x12345678") == 0);
    free_disassembler(intel_dialect, intel_dis);
    free_disassembler(gas_dialect, gas_dis);
    return 0;
}

static int case_semantic_disp_imm(void) {
    fcml_st_dialect *intel_dialect = NULL, *gas_dialect = NULL;
    fcml_st_disassembler *intel_dis = NULL, *gas_dis = NULL;
    TASK_CHECK(make_disassembler(0, &intel_dialect, &intel_dis) == 0);
    TASK_CHECK(make_disassembler(1, &gas_dialect, &gas_dis) == 0);
    fcml_st_disassembler_result result;
    fcml_uint8_t code[] = { 0x83, 0x44, 0x88, 0xf0, 0x7f };
    TASK_CHECK(decode(intel_dis, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    const fcml_st_integer *disp = &(result.instruction.operands[0].address
            .effective_address.displacement);
    TASK_CHECK(disp->size == 8);
    TASK_CHECK(disp->int8 == (fcml_int8_t) 0xf0);
    TASK_CHECK(result.instruction.operands[1].immediate.int8 == 0x7f);
    /* No compressed scaling here. */
    TASK_CHECK(result.instruction_details.modrm_details.displacement.N
            .is_not_null == FCML_FALSE);
    char text[256];
    TASK_CHECK(render_text(0, intel_dialect, &result, NULL, NULL, text,
            sizeof(text)) == 0);
    TASK_CHECK(strcmp(text,
            "add dword ptr [eax+ecx*4+0f0h],7fh") == 0);
    TASK_CHECK(decode(gas_dis, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    TASK_CHECK(render_text(1, gas_dialect, &result, NULL, NULL, text,
            sizeof(text)) == 0);
    TASK_CHECK(strcmp(text, "addl $0x7f,0xf0(%eax,%ecx,4)") == 0);
    free_disassembler(intel_dialect, intel_dis);
    free_disassembler(gas_dialect, gas_dis);
    return 0;
}

static int case_semantic_compressed(void) {
    fcml_st_dialect *intel_dialect = NULL, *gas_dialect = NULL;
    fcml_st_disassembler *intel_dis = NULL, *gas_dis = NULL;
    TASK_CHECK(make_disassembler(0, &intel_dialect, &intel_dis) == 0);
    TASK_CHECK(make_disassembler(1, &gas_dialect, &gas_dis) == 0);
    fcml_st_disassembler_result result;
    fcml_uint8_t code[] = { 0x62, 0xf1, 0x7c, 0x9c, 0x56, 0x4c, 0x24,
            0x08 };
    TASK_CHECK(decode(intel_dis, FCML_OM_64_BIT, code, sizeof(code),
            &result) == 0);
    const fcml_st_raw_displacement *raw =
            &(result.instruction_details.modrm_details.displacement);
    TASK_CHECK(raw->N.is_not_null == FCML_TRUE);
    TASK_CHECK(raw->N.value == 4);
    /* The raw displacement keeps the pre-scaling one-byte value; the
     * scaled 0x20 is only available as the operand's semantic value. */
    TASK_CHECK(raw->displacement.size == 8);
    TASK_CHECK((fcml_uint8_t) raw->displacement.int8 == 0x08);
    TASK_CHECK(result.instruction.operands[2].address.effective_address
            .displacement.int32 == 0x20);
    char text[256];
    TASK_CHECK(render_text(0, intel_dialect, &result, NULL, NULL, text,
            sizeof(text)) == 0);
    TASK_CHECK(strcmp(text,
            "vorps xmm1{k4}{z},xmm0,dword ptr [rsp+20h]{1to4}") == 0);
    TASK_CHECK(decode(gas_dis, FCML_OM_64_BIT, code, sizeof(code),
            &result) == 0);
    TASK_CHECK(render_text(1, gas_dialect, &result, NULL, NULL, text,
            sizeof(text)) == 0);
    TASK_CHECK(strcmp(text,
            "vorps 0x20(%rsp){1to4},%xmm0,%xmm1{%k4}{z}") == 0);
    free_disassembler(intel_dialect, intel_dis);
    free_disassembler(gas_dialect, gas_dis);
    return 0;
}

static int case_signed_decimal_text(void) {
    fcml_st_dialect *intel_dialect = NULL, *gas_dialect = NULL;
    fcml_st_disassembler *intel_dis = NULL, *gas_dis = NULL;
    TASK_CHECK(make_disassembler(0, &intel_dialect, &intel_dis) == 0);
    TASK_CHECK(make_disassembler(1, &gas_dialect, &gas_dis) == 0);
    fcml_st_disassembler_result result;
    /* add dword ptr [eax+ecx*4-0x10],0x7f: one negative disp8 and a
     * positive imm8, both encoded in a single byte. */
    fcml_uint8_t code[] = { 0x83, 0x44, 0x88, 0xf0, 0x7f };
    char text[256];
    TASK_CHECK(decode(intel_dis, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    TASK_CHECK(render_signed_text(intel_dialect, &result, text,
            sizeof(text)) == 0);
    TASK_CHECK(strcmp(text,
            "add dword ptr [eax+ecx*4-16],127") == 0);
    TASK_CHECK(decode(gas_dis, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    TASK_CHECK(render_signed_text(gas_dialect, &result, text,
            sizeof(text)) == 0);
    TASK_CHECK(strcmp(text,
            "addl $127,-16(%eax,%ecx,4)") == 0);
    free_disassembler(intel_dialect, intel_dis);
    free_disassembler(gas_dialect, gas_dis);
    return 0;
}

static int case_truncation_reuse(void) {
    fcml_st_dialect *dialect = NULL;
    fcml_st_disassembler *disassembler = NULL;
    TASK_CHECK(make_disassembler(0, &dialect, &disassembler) == 0);
    fcml_st_disassembler_result result;
    fcml_uint8_t truncated[] = { 0x83, 0xc0 }; /* missing immediate */
    TASK_CHECK(decode(disassembler, FCML_OM_32_BIT, truncated,
            sizeof(truncated), &result) != 0);
    /* A failed decode must not leave any provenance behind. */
    for (int i = 0; i < FCML_OPERANDS_COUNT; i++) {
        TASK_CHECK(result.instruction_details.operand_details[i]
                .encoded_field_count == 0);
    }
    /* Reusing the same result holder for the next instruction works and
     * brings fresh fields with it. */
    fcml_uint8_t code[] = { 0x83, 0x44, 0x88, 0xf0, 0x7f };
    TASK_CHECK(decode(disassembler, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    TASK_CHECK(field_of(&result, 0, FCML_EFK_MODRM_DISPLACEMENT) != NULL);
    TASK_CHECK(field_of(&result, 1, FCML_EFK_IMMEDIATE) != NULL);
    char text[256];
    TASK_CHECK(render_text(0, dialect, &result, NULL, NULL, text,
            sizeof(text)) == 0);
    TASK_CHECK(strcmp(text,
            "add dword ptr [eax+ecx*4+0f0h],7fh") == 0);
    free_disassembler(dialect, disassembler);
    return 0;
}

/*****************************
 * Group: symbols.
 *****************************/

static int case_symbol_branch(void) {
    fcml_st_dialect *intel_dialect = NULL, *gas_dialect = NULL;
    fcml_st_disassembler *intel_dis = NULL, *gas_dis = NULL;
    TASK_CHECK(make_disassembler(0, &intel_dialect, &intel_dis) == 0);
    TASK_CHECK(make_disassembler(1, &gas_dialect, &gas_dis) == 0);
    fcml_st_disassembler_result result;
    fcml_uint8_t code[] = { 0xe8, 0x01, 0x00, 0x00, 0x00 };
    struct symbol_context context;
    char text[256];
    TASK_CHECK(decode(intel_dis, FCML_OM_64_BIT, code, sizeof(code),
            &result) == 0);
    memset(&context, 0, sizeof(context));
    TASK_CHECK(render_text(0, intel_dialect, &result, test_symbol_resolver,
            &context, text, sizeof(text)) == 0);
    TASK_CHECK(strcmp(text, "call target") == 0);
    TASK_CHECK(context.calls[FCML_EFK_RELATIVE_BRANCH] == 1);
    /* The callback sees the displayed target IP, not the raw displacement. */
    TASK_CHECK(context.last_value == 0x1006);
    TASK_CHECK(context.last_value_size == 64);
    TASK_CHECK(context.index_log[0] == 0);
    TASK_CHECK(decode(gas_dis, FCML_OM_64_BIT, code, sizeof(code),
            &result) == 0);
    memset(&context, 0, sizeof(context));
    TASK_CHECK(render_text(1, gas_dialect, &result, test_symbol_resolver,
            &context, text, sizeof(text)) == 0);
    TASK_CHECK(strcmp(text, "callq target") == 0);
    TASK_CHECK(context.calls[FCML_EFK_RELATIVE_BRANCH] == 1);
    free_disassembler(intel_dialect, intel_dis);
    free_disassembler(gas_dialect, gas_dis);
    return 0;
}

static int case_symbol_disp_and_imm(void) {
    fcml_st_dialect *intel_dialect = NULL, *gas_dialect = NULL;
    fcml_st_disassembler *intel_dis = NULL, *gas_dis = NULL;
    TASK_CHECK(make_disassembler(0, &intel_dialect, &intel_dis) == 0);
    TASK_CHECK(make_disassembler(1, &gas_dialect, &gas_dis) == 0);
    fcml_st_disassembler_result result;
    fcml_uint8_t code[] = { 0x83, 0x44, 0x88, 0xf0, 0x7f };
    struct symbol_context context;
    char text[256];
    TASK_CHECK(decode(intel_dis, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    memset(&context, 0, sizeof(context));
    TASK_CHECK(render_text(0, intel_dialect, &result, test_symbol_resolver,
            &context, text, sizeof(text)) == 0);
    /* The immediate is rejected by kind and stays numeric. */
    TASK_CHECK(strcmp(text,
            "add dword ptr [eax+ecx*4+disp_sym],7fh") == 0);
    TASK_CHECK(context.log_count == 2);
    TASK_CHECK(context.kind_log[0] == FCML_EFK_MODRM_DISPLACEMENT);
    TASK_CHECK(context.index_log[0] == 0);
    TASK_CHECK(context.kind_log[1] == FCML_EFK_IMMEDIATE);
    TASK_CHECK(context.index_log[1] == 1);
    TASK_CHECK(context.calls[FCML_EFK_MODRM_DISPLACEMENT] == 1);
    TASK_CHECK(context.calls[FCML_EFK_IMMEDIATE] == 1);
    /* GAS reverses the operands; indices follow the rendered order. */
    TASK_CHECK(decode(gas_dis, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    memset(&context, 0, sizeof(context));
    TASK_CHECK(render_text(1, gas_dialect, &result, test_symbol_resolver,
            &context, text, sizeof(text)) == 0);
    TASK_CHECK(strcmp(text,
            "addl $0x7f,disp_sym(%eax,%ecx,4)") == 0);
    TASK_CHECK(context.log_count == 2);
    TASK_CHECK(context.kind_log[0] == FCML_EFK_IMMEDIATE);
    TASK_CHECK(context.index_log[0] == 0);
    TASK_CHECK(context.kind_log[1] == FCML_EFK_MODRM_DISPLACEMENT);
    TASK_CHECK(context.index_log[1] == 1);
    free_disassembler(intel_dialect, intel_dis);
    free_disassembler(gas_dialect, gas_dis);
    return 0;
}

static int case_symbol_moffs(void) {
    fcml_st_dialect *intel_dialect = NULL, *gas_dialect = NULL;
    fcml_st_disassembler *intel_dis = NULL, *gas_dis = NULL;
    TASK_CHECK(make_disassembler(0, &intel_dialect, &intel_dis) == 0);
    TASK_CHECK(make_disassembler(1, &gas_dialect, &gas_dis) == 0);
    fcml_st_disassembler_result result;
    fcml_uint8_t code[] = { 0xa1, 0x78, 0x56, 0x34, 0x12 };
    struct symbol_context context;
    char text[256];
    TASK_CHECK(decode(intel_dis, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    memset(&context, 0, sizeof(context));
    TASK_CHECK(render_text(0, intel_dialect, &result, test_symbol_resolver,
            &context, text, sizeof(text)) == 0);
    TASK_CHECK(strcmp(text, "mov eax,dword ptr [moffs_sym]") == 0);
    TASK_CHECK(context.calls[FCML_EFK_MOFFS] == 1);
    TASK_CHECK(context.index_log[0] == 1);
    TASK_CHECK(decode(gas_dis, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    memset(&context, 0, sizeof(context));
    TASK_CHECK(render_text(1, gas_dialect, &result, test_symbol_resolver,
            &context, text, sizeof(text)) == 0);
    TASK_CHECK(strcmp(text, "mov moffs_sym,%eax") == 0);
    TASK_CHECK(context.calls[FCML_EFK_MOFFS] == 1);
    TASK_CHECK(context.index_log[0] == 0);
    free_disassembler(intel_dialect, intel_dis);
    free_disassembler(gas_dialect, gas_dis);
    return 0;
}

static int case_symbol_far(void) {
    fcml_st_dialect *intel_dialect = NULL, *gas_dialect = NULL;
    fcml_st_disassembler *intel_dis = NULL, *gas_dis = NULL;
    TASK_CHECK(make_disassembler(0, &intel_dialect, &intel_dis) == 0);
    TASK_CHECK(make_disassembler(1, &gas_dialect, &gas_dis) == 0);
    fcml_st_disassembler_result result;
    fcml_uint8_t code[] = { 0xea, 0x78, 0x56, 0x34, 0x12, 0x9a, 0xbc };
    struct symbol_context context;
    char text[256];
    TASK_CHECK(decode(intel_dis, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    memset(&context, 0, sizeof(context));
    TASK_CHECK(render_text(0, intel_dialect, &result, test_symbol_resolver,
            &context, text, sizeof(text)) == 0);
    TASK_CHECK(strcmp(text, "jmp far seg_sym:off_sym") == 0);
    TASK_CHECK(context.calls[FCML_EFK_FAR_SEGMENT] == 1);
    TASK_CHECK(context.calls[FCML_EFK_FAR_OFFSET] == 1);
    TASK_CHECK(context.log_count == 2);
    TASK_CHECK(decode(gas_dis, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    memset(&context, 0, sizeof(context));
    TASK_CHECK(render_text(1, gas_dialect, &result, test_symbol_resolver,
            &context, text, sizeof(text)) == 0);
    /* '$' and ',' decorations survive, only the number slots change. */
    TASK_CHECK(strcmp(text, "ljmpl $seg_sym,$off_sym") == 0);
    TASK_CHECK(context.calls[FCML_EFK_FAR_SEGMENT] == 1);
    TASK_CHECK(context.calls[FCML_EFK_FAR_OFFSET] == 1);
    free_disassembler(intel_dialect, intel_dis);
    free_disassembler(gas_dialect, gas_dis);
    return 0;
}

static int case_symbol_compressed_value(void) {
    fcml_st_dialect *dialect = NULL;
    fcml_st_disassembler *disassembler = NULL;
    TASK_CHECK(make_disassembler(0, &dialect, &disassembler) == 0);
    fcml_st_disassembler_result result;
    fcml_uint8_t code[] = { 0x62, 0xf1, 0x7c, 0x9c, 0x56, 0x4c, 0x24,
            0x08 };
    TASK_CHECK(decode(disassembler, FCML_OM_64_BIT, code, sizeof(code),
            &result) == 0);
    struct symbol_context context;
    char text[256];
    memset(&context, 0, sizeof(context));
    TASK_CHECK(render_text(0, dialect, &result, test_symbol_resolver,
            &context, text, sizeof(text)) == 0);
    TASK_CHECK(strcmp(text,
            "vorps xmm1{k4}{z},xmm0,dword ptr [rsp+disp_sym]{1to4}") == 0);
    TASK_CHECK(context.calls[FCML_EFK_MODRM_DISPLACEMENT] == 1);
    /* Displayed integer is the scaled semantic value 0x20. */
    TASK_CHECK(context.last_value == 0x20);
    TASK_CHECK(context.last_value_size == 32);
    /* A second rendering consults the field once again. */
    memset(&context, 0, sizeof(context));
    TASK_CHECK(render_text(0, dialect, &result, test_symbol_resolver,
            &context, text, sizeof(text)) == 0);
    TASK_CHECK(context.calls[FCML_EFK_MODRM_DISPLACEMENT] == 1);
    free_disassembler(dialect, disassembler);
    return 0;
}

static int case_symbol_hidden_pseudo(void) {
    fcml_st_dialect *intel_dialect = NULL, *gas_dialect = NULL;
    fcml_st_disassembler *intel_dis = NULL, *gas_dis = NULL;
    TASK_CHECK(make_disassembler(0, &intel_dialect, &intel_dis) == 0);
    TASK_CHECK(make_disassembler(1, &gas_dialect, &gas_dis) == 0);
    fcml_st_disassembler_result result;
    /* vcmppd xmm1,xmm0,xmm3,0 - the predicate is a pseudo operand. */
    fcml_uint8_t code[] = { 0xc5, 0xf9, 0xc2, 0xcb, 0x00 };
    struct symbol_context context;
    char with_symbol[256], plain[256];
    TASK_CHECK(decode(intel_dis, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    memset(&context, 0, sizeof(context));
    TASK_CHECK(render_text(0, intel_dialect, &result, test_symbol_resolver,
            &context, with_symbol, sizeof(with_symbol)) == 0);
    TASK_CHECK(render_text(0, intel_dialect, &result, NULL, NULL, plain,
            sizeof(plain)) == 0);
    /* No field-backed number means no resolver calls and identical text. */
    TASK_CHECK(context.log_count == 0);
    TASK_CHECK(strcmp(with_symbol, plain) == 0);
    TASK_CHECK(decode(gas_dis, FCML_OM_32_BIT, code, sizeof(code),
            &result) == 0);
    memset(&context, 0, sizeof(context));
    TASK_CHECK(render_text(1, gas_dialect, &result, test_symbol_resolver,
            &context, with_symbol, sizeof(with_symbol)) == 0);
    TASK_CHECK(render_text(1, gas_dialect, &result, NULL, NULL, plain,
            sizeof(plain)) == 0);
    TASK_CHECK(context.log_count == 0);
    TASK_CHECK(strcmp(with_symbol, plain) == 0);
    free_disassembler(intel_dialect, intel_dis);
    free_disassembler(gas_dialect, gas_dis);
    return 0;
}

const struct task_case task_cases[] = {
    /* Encoded-field provenance. */
    { "encoding", "short_imm", case_short_imm },
    { "encoding", "disp_and_imm", case_disp_and_imm },
    { "encoding", "rip_relative", case_rip_relative },
    { "encoding", "branch_rel32", case_branch_rel32 },
    { "encoding", "branch_rel16", case_branch_rel16 },
    { "encoding", "moffs32", case_moffs32 },
    { "encoding", "moffs16", case_moffs16 },
    { "encoding", "far32", case_far32 },
    { "encoding", "far16", case_far16 },
    { "encoding", "compressed_disp8", case_compressed_disp8 },
    /* Semantic values and dialect identity without a resolver. */
    { "identity", "short_imm", case_semantic_short_imm },
    { "identity", "rip_relative", case_semantic_rip },
    { "identity", "branch", case_semantic_branch },
    { "identity", "moffs", case_semantic_moffs },
    { "identity", "far", case_semantic_far },
    { "identity", "disp_and_imm", case_semantic_disp_imm },
    { "identity", "compressed_disp8", case_semantic_compressed },
    { "identity", "signed_decimal_text", case_signed_decimal_text },
    { "identity", "truncation_reuse", case_truncation_reuse },
    /* Symbol resolution. */
    { "symbols", "branch_target", case_symbol_branch },
    { "symbols", "disp_and_imm", case_symbol_disp_and_imm },
    { "symbols", "moffs", case_symbol_moffs },
    { "symbols", "far", case_symbol_far },
    { "symbols", "compressed_value", case_symbol_compressed_value },
    { "symbols", "hidden_pseudo", case_symbol_hidden_pseudo }
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
