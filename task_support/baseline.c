#include <fcml_disassembler.h>
#include <fcml_intel_dialect.h>
#include <fcml_gas_dialect.h>
#include <fcml_renderer.h>
#include <stdio.h>
#include <string.h>
#include "samples.h"
int main(void) {
 for (int gas=0;gas<2;gas++) {
  fcml_st_dialect *dialect=NULL; fcml_st_disassembler *dis=NULL;
  int rc=gas?fcml_fn_dialect_init_gas(0,&dialect):fcml_fn_dialect_init_intel(0,&dialect);
  if(rc || fcml_fn_disassembler_init(dialect,&dis)) return 1;
  for(unsigned i=0;i<sizeof(samples)/sizeof(samples[0]);i++) {
   fcml_st_disassembler_context ctx={0}; fcml_st_disassembler_result result;
   fcml_st_render_config config={0}; char output[512]={0};
   ctx.disassembler=dis;ctx.entry_point.op_mode=samples[i].mode;ctx.entry_point.ip=0x1000;
   ctx.code=(fcml_ptr)samples[i].bytes;ctx.code_length=samples[i].count;
   fcml_fn_disassembler_result_prepare(&result);
   rc=fcml_fn_disassemble(&ctx,&result);
   if(rc || result.instruction_details.instruction_size!=samples[i].count) return 2;
   config.render_flags=FCML_REND_FLAG_REMOVE_LEADING_ZEROS|FCML_REND_FLAG_HEX_IMM|FCML_REND_FLAG_HEX_DISPLACEMENT;
   if(fcml_fn_render(dialect,&config,output,sizeof(output),&result) || !output[0]) return 3;
   if(i==0 && result.instruction.operands[gas?0:1].immediate.size!=32) return 4;
   printf("%s %s len=%u text=%s\n",gas?"GAS":"INTEL",samples[i].name,(unsigned)result.instruction_details.instruction_size,output);
   fcml_fn_disassembler_result_free(&result);
  }
  fcml_fn_disassembler_free(dis);fcml_fn_dialect_free(dialect);
 }
 puts("BASELINE_OK decoded=12 signed_imm_semantic_bits=32");return 0;
}
