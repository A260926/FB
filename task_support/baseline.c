#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "armdisasm.h"
struct capture {int count;char first[128];};
static bool collect(uint32_t a,const char *text,void *user){
  struct capture*c=user;(void)a;if(c->count++==0)snprintf(c->first,sizeof(c->first),"%s",text);return true;
}
int main(void){
  /* Little-endian Thumb NOP,NOP followed by ARM MOV r0,r0; data only. */
  static const uint8_t code[]={0x00,0xbf,0x00,0xbf,0x00,0x00,0xa0,0xe1};
  ARMSTATE s;struct capture normal={0},future={0};
  disasm_init(&s,0);disasm_address(&s,0x1000);
  if(!disasm_buffer(&s,code,sizeof(code),ARMMODE_THUMB,collect,&normal))return 1;
  if(strncmp(normal.first,"nop",3)!=0)return 2;
  printf("NORMAL first=%s count=%d\n",normal.first,normal.count);disasm_cleanup(&s);
  disasm_init(&s,0);disasm_address(&s,0x1000);disasm_symbol(&s,"arm_later",0x1004,ARMMODE_ARM);
  if(!disasm_buffer(&s,code,sizeof(code),ARMMODE_THUMB,collect,&future))return 3;
  printf("CURRENT_FUTURE_SYMBOL first=%s count=%d\n",future.first,future.count);
  /* Snapshot current behavior, not the requested future result. */
  printf("BASELINE_OK raw_decoder=1 future_mode_mismatch=%d\n",strncmp(future.first,"nop",3)!=0);disasm_cleanup(&s);return 0;
}
