#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <capstone/capstone.h>
static int has(cs_regs regs,unsigned n,unsigned r){for(unsigned i=0;i<n;i++)if(regs[i]==r)return 1;return 0;}
int main(void){
  const uint32_t words[]={0xf85f8c20,0xf8408420,0xf862d820};
  uint8_t code[12];for(unsigned i=0;i<3;i++)for(unsigned j=0;j<4;j++)code[4*i+j]=(uint8_t)(words[i]>>(8*j));
  csh h;cs_insn *ins=NULL;assert(cs_open(CS_ARCH_ARM64,CS_MODE_ARM,&h)==CS_ERR_OK);
  assert(cs_option(h,CS_OPT_DETAIL,CS_OPT_ON)==CS_ERR_OK);
  size_t n=cs_disasm(h,code,sizeof(code),0x1000,0,&ins);assert(n==3);
  for(unsigned i=0;i<n;i++)printf("%s %s\n",ins[i].mnemonic,ins[i].op_str);
  if(ins[0].detail->arm64.operands[1].mem.disp!=-8){fputs("FAIL mem.disp==-8\n",stderr);return 2;}
  puts("CAPSTONE_NEGATIVE_DISP_OK");
  if(!ins[1].detail->arm64.post_index || !ins[1].detail->arm64.writeback){fputs("FAIL post_index\n",stderr);return 2;}
  puts("CAPSTONE_POST_INDEX_OK");
  cs_regs rd,wr;uint8_t nr,nw;
  assert(cs_regs_access(h,&ins[0],rd,&nr,wr,&nw)==CS_ERR_OK);
  if(!has(rd,nr,ARM64_REG_X1) || !has(wr,nw,ARM64_REG_X1)){fputs("FAIL has(wr,nw,ARM64_REG_X1)\n",stderr);return 2;}
  puts("CAPSTONE_BASE_WRITEBACK_OK");
  assert(ins[2].detail->arm64.operands[1].mem.index==ARM64_REG_W2);
  assert(ins[2].detail->arm64.operands[1].ext==ARM64_EXT_SXTW);
  assert(ins[2].detail->arm64.operands[1].shift.value==3);
  cs_free(ins,n);assert(cs_close(&h)==CS_ERR_OK);return 0;
}
