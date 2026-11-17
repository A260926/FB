/* Existing API example: read synthetic bytes; never execute the instruction.
 * It shows how to trace a rendered number back to the encoding field it
 * comes from and how to replace such numbers with local symbols. */
#include <fcml_disassembler.h>
#include <fcml_intel_dialect.h>
#include <fcml_gas_dialect.h>
#include <fcml_renderer.h>
#include <stdio.h>

/* Borrowed symbol names; the table itself is not owned by FCML. */
static const fcml_char *resolve_symbol(fcml_ptr context,
        fcml_int operand_index, const fcml_st_encoded_field *field,
        const fcml_st_integer *value) {
    (void) context;
    (void) operand_index;
    (void) value;
    /* Branches are labelled, plain immediates keep their numeric form. */
    if (field->kind == FCML_EFK_RELATIVE_BRANCH) {
        return "branch_target";
    }
    if (field->kind == FCML_EFK_MODRM_DISPLACEMENT) {
        return "table";
    }
    return NULL;
}

static void dump_fields(const fcml_st_disassembler_result *result) {
    for (int i = 0; i < result->instruction.operands_count; i++) {
        const fcml_st_operand_details *details =
                &(result->instruction_details.operand_details[i]);
        for (int f = 0; f < details->encoded_field_count; f++) {
            const fcml_st_encoded_field *field =
                    &(details->encoded_fields[f]);
            printf("  operand %d field: kind=%d offset=%u size=%u "
                    "raw=0x%llx\n", i, (int) field->kind,
                    (unsigned) field->offset, (unsigned) field->size,
                    (unsigned long long) field->raw_value);
        }
    }
}

int main(void) {
    /* call rel32 (branch) and add [rip+disp],imm (displacement). */
    unsigned char branch[] = { 0xe8, 0x01, 0x00, 0x00, 0x00 };
    unsigned char memory[] = { 0x48, 0x83, 0x05, 0x11, 0x00, 0x00, 0x00,
            0xff };

    for (int gas = 0; gas < 2; gas++) {
        fcml_st_dialect *dialect = 0;
        fcml_st_disassembler *disassembler = 0;
        if (gas ? fcml_fn_dialect_init_gas(0, &dialect)
                : fcml_fn_dialect_init_intel(0, &dialect)) {
            return 1;
        }
        if (fcml_fn_disassembler_init(dialect, &disassembler)) {
            return 1;
        }

        for (int sample = 0; sample < 2; sample++) {
            unsigned char *bytes = sample ? memory : branch;
            unsigned length = sample ? (unsigned) sizeof(memory)
                    : (unsigned) sizeof(branch);

            fcml_st_disassembler_context context = { 0 };
            fcml_st_disassembler_result result;
            context.disassembler = disassembler;
            context.entry_point.op_mode = FCML_OM_64_BIT;
            context.entry_point.ip = 0x1000;
            context.code = bytes;
            context.code_length = length;

            fcml_fn_disassembler_result_prepare(&result);
            if (fcml_fn_disassemble(&context, &result)) {
                return 2;
            }

            char text[512];
            fcml_st_render_config config = { 0 };
            config.render_flags = FCML_REND_FLAG_REMOVE_LEADING_ZEROS
                    | FCML_REND_FLAG_HEX_IMM
                    | FCML_REND_FLAG_HEX_DISPLACEMENT;
            config.symbol_resolver = resolve_symbol;
            if (fcml_fn_render(dialect, &config, text, sizeof(text),
                    &result)) {
                return 3;
            }

            printf("%s %s\n", gas ? "GAS  " : "INTEL", text);
            dump_fields(&result);
            fcml_fn_disassembler_result_free(&result);
        }

        fcml_fn_disassembler_free(disassembler);
        fcml_fn_dialect_free(dialect);
    }
    return 0;
}
