#include "x86emu.h"
#include <stdio.h>
static unsigned faults;
static int fault_handler(x86emu_t *e,u8 n,unsigned type){
 (void)type;if(n==13||n==12)faults++;x86emu_stop(e);return 1;
}
static int one(int wrap){
 const unsigned char small[]={0xc6,0x05,0x01,0x01,0,0,0x7b,0xf4};
 const unsigned char overflow[]={0xc7,0x05,0xfe,0xff,0xff,0xff,0x44,0x33,0x22,0x11,0xf4};
 const unsigned char *code=wrap?overflow:small;unsigned len=wrap?sizeof(overflow):sizeof(small);
 x86emu_t *e=x86emu_new(X86EMU_PERM_R|X86EMU_PERM_W|X86EMU_PERM_X,0);if(!e)return 1;
 // Valid preloaded protected-mode segment caches; code and data are separate.
 e->x86.R_CS=8;e->x86.R_CS_BASE=0x10000;e->x86.R_CS_LIMIT=0xffff;e->x86.R_CS_ACC=0x49b;e->x86.R_EIP=0;
 e->x86.R_DS=16;e->x86.R_DS_BASE=0x2000;e->x86.R_DS_LIMIT=0x100;e->x86.R_DS_ACC=0x493;e->x86.R_CR0|=1;
 for(unsigned i=0;i<len;i++)x86emu_write_byte(e,0x10000+i,code[i]);
 unsigned target=wrap?0x1ffe:0x2101;x86emu_write_dword(e,target,0xa5a5a5a5);
 faults=0;x86emu_set_intr_handler(e,fault_handler);e->max_instr=8;x86emu_run(e,X86EMU_RUN_MAX_INSTR);
 unsigned val=x86emu_read_dword(e,target);printf("SEGMENT_WITNESS case=%s faults=%u memory=%08x\n",wrap?"wrap":"after_fault",faults,val);
 int bad=wrap?(faults==0&&val==0x11223344):(faults==1&&(val&255)==0x7b);
 x86emu_done(e);return !bad;
}
int main(void){if(one(0)||one(1))return 2;puts("ORIGINAL_SEGMENT_FAILURE_CONFIRMED");return 0;}
