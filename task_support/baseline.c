#include "x86emu.h"
#include <stdio.h>
static int run(int bits){
 const unsigned char code16[]={0xb8,0x34,0x12,0x8a,0x26,0,2,0x3d,0x34,0x56,0x0f,0x94,0xc3,0x89,0x1e,2,2,0xf4};
 const unsigned char code32[]={0xb8,0x34,0x12,0,0,0x8a,0x25,0,2,0,0,0x3d,0x34,0x56,0,0,0x0f,0x94,0xc3,0x89,0x1d,2,2,0,0,0xf4};
 const unsigned char *p=bits==16?code16:code32;unsigned n=bits==16?sizeof(code16):sizeof(code32);
 x86emu_t*e=x86emu_new(X86EMU_PERM_R|X86EMU_PERM_W|X86EMU_PERM_X,0);if(!e)return 1;
 x86emu_set_seg_register(e,e->x86.R_CS_SEL,0);e->x86.R_EIP=0;
 if(bits==32){e->x86.R_CS_ACC|=0x400;e->x86.R_CS_LIMIT=0xffffffff;e->x86.R_DS_LIMIT=0xffffffff;}
 for(unsigned i=0;i<n;i++)x86emu_write_byte(e,i,p[i]);x86emu_write_byte(e,0x200,0x56);
 e->max_instr=30;unsigned status=x86emu_run(e,X86EMU_RUN_MAX_INSTR);
 unsigned out=x86emu_read_word(e,0x202);
 if(e->x86.R_EAX!=0x5634||e->x86.R_BL!=1||out!=1){fprintf(stderr,"bits=%d eax=%x bl=%x mem=%x status=%x\n",bits,e->x86.R_EAX,e->x86.R_BL,out,status);x86emu_done(e);return 2;}
 printf("EXECUTED bits=%d eax=%x bl=%u stored=%u\n",bits,e->x86.R_EAX,e->x86.R_BL,out);x86emu_done(e);return 0;
}
int main(void){if(run(16)||run(32))return 2;puts("X86EMU_DATAFLOW_BASELINE_OK");return 0;}
