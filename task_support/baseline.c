#include <stdio.h>
#include <string.h>
#define CHIPS_IMPL
#include "chips/z80.h"
int main(void){
 static unsigned char mem[65536];
 const unsigned char code[]={0xdd,0x21,0,0x20,0xdd,0xcb,1,0x46,0x3e,0x42,0x32,0,0x30,0x76};
 memcpy(mem,code,sizeof(code));mem[0x2001]=1;
 z80_t c;uint64_t pins=z80_init(&c);unsigned ticks=0,nonm1_opcode=0;
 for(;ticks<300;ticks++){
  pins=z80_tick(&c,pins);
  if((pins&(Z80_MREQ|Z80_RD))==(Z80_MREQ|Z80_RD)){
   unsigned a=Z80_GET_ADDR(pins);if(a==7 && !(pins&Z80_M1))nonm1_opcode=1;
   Z80_SET_DATA(pins,mem[a]);
  }
  if((pins&(Z80_MREQ|Z80_WR))==(Z80_MREQ|Z80_WR))mem[Z80_GET_ADDR(pins)]=Z80_GET_DATA(pins);
  if(pins&Z80_HALT)break;
 }
 if(ticks==300||c.ix!=0x2000||c.a!=0x42||mem[0x3000]!=0x42||!nonm1_opcode)return 2;
 printf("Z80_EXEC ix=%04x a=%02x store=%02x\n",c.ix,c.a,mem[0x3000]);
 printf("DDCB_OPCODE_NON_M1=%u\n",nonm1_opcode);puts("Z80_ORIGIN_BASELINE_OK");return 0;
}
