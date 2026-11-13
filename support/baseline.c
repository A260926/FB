#include <assert.h>
#include <stdio.h>
#include <capstone/capstone.h>
int main(void) {
  const unsigned char bytes[]={0xb8,1,0,0,0,0x48,0x8d,0x15,0x10,0,0,0,0xff,0x24,0xc2};
  csh h; cs_insn *a; assert(cs_open(CS_ARCH_X86,CS_MODE_64,&h)==CS_ERR_OK);
  assert(cs_option(h,CS_OPT_DETAIL,CS_OPT_ON)==CS_ERR_OK);
  size_t n=cs_disasm(h,bytes,sizeof bytes,0x1000,0,&a); assert(n==3);
  assert(a[0].id==X86_INS_MOV && a[0].detail->x86.operands[0].reg==X86_REG_EAX);
  assert(a[1].id==X86_INS_LEA && a[1].detail->x86.operands[1].mem.base==X86_REG_RIP);
  assert(a[2].id==X86_INS_JMP && a[2].detail->x86.operands[0].mem.scale==8);
  cs_free(a,n); cs_close(&h); puts("BASELINE_CAPSTONE_DETAIL_OK instructions=3");
}
