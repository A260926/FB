#include "udis86.h"
#include <stdio.h>
#include <stdint.h>
int main(void){
 /* AL/AH, EAX/R8D and a flag-preserving INC before a conditional transfer. */
 const uint8_t code[]={0xb0,0x12,0xb4,0x34,0x41,0x89,0xc0,0xff,0xc1,0x72,0x01,0x90,0xc3};
 const enum ud_mnemonic_code mn[]={UD_Imov,UD_Imov,UD_Imov,UD_Iinc,UD_Ijb,UD_Inop,UD_Iret};
 const unsigned lengths[]={2,2,3,2,2,1,1};
 ud_t u;ud_init(&u);ud_set_mode(&u,64);ud_set_pc(&u,0x1000);ud_set_input_buffer(&u,code,sizeof code);
 for(unsigned i=0;i<7;++i){
  if(ud_disassemble(&u)!=lengths[i]||ud_insn_mnemonic(&u)!=mn[i])return 1;
  if(i==0&&ud_insn_opr(&u,0)->base!=UD_R_AL)return 2;
  if(i==1&&ud_insn_opr(&u,0)->base!=UD_R_AH)return 3;
  if(i==2&&(ud_insn_opr(&u,0)->base!=UD_R_R8D||ud_insn_opr(&u,1)->base!=UD_R_EAX))return 4;
  if(i==4&&(ud_insn_opr(&u,0)->type!=UD_OP_JIMM||ud_insn_opr(&u,0)->lval.sbyte!=1))return 5;
 }
 if(ud_disassemble(&u))return 6;
 puts("MATCH_INPUT_BASELINE instructions=7 alias=AL/AH/R8D conditional=JB relative=1");return 0;
}
