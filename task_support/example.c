/* Existing API example: read synthetic bytes; never execute the instruction.
 * Also shows how to locate the encoded bytes behind a rendered number and how
 * to replace that number with a symbol through the render callback. */
#include <fcml_disassembler.h>
#include <fcml_intel_dialect.h>
#include <fcml_renderer.h>
#include <stdio.h>

/* A real tool would map field offsets to symbols in its own symbol table.
 * Here every backed number is named after the raw byte value it encodes. */
static fcml_string resolve_symbol(fcml_ptr context, fcml_int operand_index,
        const fcml_st_encoded_field *field, const fcml_st_integer *value) {
    (void) context;
    (void) operand_index;
    (void) value;
    static char name[40];
    /* Reject plain immediates by kind: they keep their numeric rendering. */
    if (field->kind == FCML_EFK_IMMEDIATE) {
        return NULL;
    }
    snprintf(name, sizeof(name), "sym_%llx",
            (unsigned long long) field->raw_value);
    return name;
}

int main(void) {
    /* call rel32: E8 01 00 00 00, read as data only. */
    unsigned char bytes[] = {0xe8, 0x01, 0x00, 0x00, 0x00};
    fcml_st_dialect *d = 0;
    fcml_st_disassembler *a = 0;
    fcml_st_disassembler_context c = {0};
    fcml_st_disassembler_result r;
    fcml_st_render_config cfg = {0};
    char text[512];

    if (fcml_fn_dialect_init_intel(0, &d)
            || fcml_fn_disassembler_init(d, &a)) {
        return 1;
    }
    c.disassembler = a;
    c.entry_point.op_mode = FCML_OM_64_BIT;
    c.entry_point.ip = 0x1000;
    c.code = bytes;
    c.code_length = sizeof(bytes);

    fcml_fn_disassembler_result_prepare(&r);
    if (fcml_fn_disassemble(&c, &r)) {
        return 2;
    }

    /* Inspect the physical origin of the branch displacement. */
    const fcml_st_encoded_field *field =
            r.instruction_details.operand_details[0].encoded_fields;
    printf("field kind=%d offset=%u size=%u raw=0x%llx\n",
            (int) field->kind, (unsigned) field->offset,
            (unsigned) field->size, (unsigned long long) field->raw_value);

    if (fcml_fn_render(d, &cfg, text, sizeof(text), &r)) {
        return 3;
    }
    printf("plain : %s\n", text);

    /* Attach the resolver: the branch target number becomes a symbol. */
    cfg.symbol_resolver = resolve_symbol;
    if (fcml_fn_render(d, &cfg, text, sizeof(text), &r)) {
        return 4;
    }
    printf("symbol: %s\n", text);

    fcml_fn_disassembler_result_free(&r);
    fcml_fn_disassembler_free(a);
    fcml_fn_dialect_free(d);
    return 0;
}
