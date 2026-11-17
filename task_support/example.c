/* Existing API example: read synthetic bytes; never execute the instruction. */
#include <fcml_disassembler.h>
#include <fcml_intel_dialect.h>
#include <fcml_renderer.h>
#include <stdio.h>
int main(void) {
 unsigned char bytes[]={0x83,0xc0,0xff};
 fcml_st_dialect *d=0;fcml_st_disassembler *a=0;
 fcml_st_disassembler_context c={0};fcml_st_disassembler_result r;
 fcml_st_render_config cfg={0};char text[512];
 if(fcml_fn_dialect_init_intel(0,&d)||fcml_fn_disassembler_init(d,&a)) return 1;
 c.disassembler=a;c.entry_point.op_mode=FCML_OM_32_BIT;c.code=bytes;c.code_length=sizeof(bytes);
 fcml_fn_disassembler_result_prepare(&r);
 if(fcml_fn_disassemble(&c,&r)||fcml_fn_render(d,&cfg,text,sizeof(text),&r)) return 2;
 puts(text);fcml_fn_disassembler_result_free(&r);fcml_fn_disassembler_free(a);fcml_fn_dialect_free(d);return 0;
}
