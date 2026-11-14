#include "x86emu.h"
#include <stdio.h>

/*
 * This used to document the broken segmented MOV behavior:
 *   - a byte write one past the DS limit faulted but still modified memory,
 *   - a dword write starting at 0xfffffffe wrapped its end calculation and
 *     ran without any fault at all.
 * It now verifies the fixed behavior: the whole access is validated before
 * data memory is touched, so a rejected MOV raises the exception and leaves
 * the target bytes (and any wrapped-around location) unchanged.
 */

static unsigned faults;
static unsigned fault_nr;

static int fault_handler(x86emu_t *e, u8 n, unsigned type)
{
 (void)type;
 if(n == 13 || n == 12) {
   faults++;
   fault_nr = n;
 }
 x86emu_stop(e);
 return 1;
}

static int one(int wrap)
{
 const unsigned char small[]={0xc6,0x05,0x01,0x01,0,0,0x7b,0xf4};
 const unsigned char overflow[]={0xc7,0x05,0xfe,0xff,0xff,0xff,0x44,0x33,0x22,0x11,0xf4};
 const unsigned char *code=wrap?overflow:small;
 unsigned len=wrap?sizeof(overflow):sizeof(small);
 x86emu_t *e=x86emu_new(X86EMU_PERM_R|X86EMU_PERM_W|X86EMU_PERM_X,0);
 if(!e) return 1;
 // Valid preloaded protected-mode segment caches; code and data are separate.
 e->x86.R_CS=8;e->x86.R_CS_BASE=0x10000;e->x86.R_CS_LIMIT=0xffff;e->x86.R_CS_ACC=0x49b;e->x86.R_EIP=0;
 e->x86.R_DS=16;e->x86.R_DS_BASE=0x2000;e->x86.R_DS_LIMIT=0x100;e->x86.R_DS_ACC=0x493;e->x86.R_CR0|=1;
 for(unsigned i=0;i<len;i++)x86emu_write_byte(e,0x10000+i,code[i]);
 unsigned target=wrap?0x1ffe:0x2101;
 x86emu_write_dword(e,target,0xa5a5a5a5);
 faults=0;fault_nr=0;
 x86emu_set_intr_handler(e,fault_handler);
 e->max_instr=8;
 x86emu_run(e,X86EMU_RUN_MAX_INSTR);
 unsigned val=x86emu_read_dword(e,target);
 printf("SEGMENT_WITNESS case=%s faults=%u vector=%u memory=%08x\n",
        wrap?"wrap":"after_fault",faults,fault_nr,val);
 // Both accesses are outside the segment: exactly one #GP, memory untouched.
 int bad=(faults!=1||fault_nr!=13||val!=0xa5a5a5a5);
 x86emu_done(e);
 return bad;
}

int main(void)
{
 if(one(0)||one(1)) return 2;
 puts("SEGMENT_PROTECTION_OK");
 return 0;
}
