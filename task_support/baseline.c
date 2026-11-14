#include <stdio.h>
#include <string.h>
#include <capstone/capstone.h>
int main(void) {
  csh h; cs_insn *p = NULL;
  const unsigned char image[] = {0x42,0xb8,0x13,0x05,0x10,0x00};
  if (cs_open(CS_ARCH_RISCV, CS_MODE_RISCV64|CS_MODE_RISCVC, &h)) return 1;
  if (cs_option(h, CS_OPT_DETAIL, CS_OPT_ON)) return 2;
  size_t n=cs_disasm(h,image,sizeof(image),0x8000,0,&p);
  if (n!=2 || p[0].id!=RISCV_INS_C_FSDSP || p[1].id!=RISCV_INS_ADDI || p[0].size!=2 || p[1].address!=0x8002) return 3;
  printf("LEGACY_OVERLAP %s %s size=%u\n",p[0].mnemonic,p[0].op_str,p[0].size);
  printf("NORMAL_PATH %s %s\n",p[1].mnemonic,p[1].op_str);
  cs_free(p,n); cs_close(&h); puts("BASELINE_OK"); return 0;
}
