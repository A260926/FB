#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "udis86.h"
int main(void){
 const unsigned char a[]={0x48,0x89,0xd8,0xc5,0xf4,0x58,0xc2};ud_t u;
 ud_init(&u);ud_set_mode(&u,64);ud_set_syntax(&u,UD_SYN_INTEL);ud_set_input_buffer(&u,a,sizeof a);
 assert(ud_disassemble(&u)==3);assert(u.mnemonic==UD_Imov);assert(ud_insn_opr(&u,0)->base==UD_R_RAX);
 assert(ud_disassemble(&u)==4);assert(u.mnemonic==UD_Ivaddps);assert(ud_insn_opr(&u,0)->base==UD_R_YMM0);assert(ud_insn_opr(&u,1)->base==UD_R_YMM1);assert(ud_insn_opr(&u,2)->base==UD_R_YMM2);
 printf("UDIS_BASELINE %s\n",ud_insn_asm(&u));assert(!ud_disassemble(&u));{const unsigned char ev[]={0x62,0x81,0x5c,0xc3,0x58,0xcf};ud_set_input_buffer(&u,ev,sizeof ev);assert(ud_disassemble(&u)>0);assert(u.mnemonic==UD_Iinvalid);puts("UDIS_BASELINE_EVEX unsupported");}return 0;
}
