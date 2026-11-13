#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "distorm.h"
static void one(const char *name,const unsigned char *b,int n,_DecodeType dt,unsigned mask,unsigned size) {
 _CodeInfo ci; _DInst out[16]; unsigned used=0; memset(&ci,0,sizeof(ci));ci.code=b;ci.codeLen=n;ci.dt=dt;
 _DecodeResult r=distorm_decompose(&ci,out,16,&used); printf("probe %s result=%d used=%u size=%u unused=%u\n",name,r,used,out[0].size,out[0].unusedPrefixesMask); if (r!=DECRES_SUCCESS || !used || out[0].flags==FLAG_NOT_DECODABLE || out[0].unusedPrefixesMask!=mask || out[0].size!=size) exit(1);
 printf("%s size=%u unused=%u opcode=%u\n",name,out[0].size,out[0].unusedPrefixesMask,out[0].opcode);
}
int main(void){
 const unsigned char dup[]={0x66,0x66,0xff,0xc0}, rex[]={0x48,0x66,0x0f,0x10,0xc0},mandatory[]={0xf3,0x0f,0x10,0xc0},vex[]={0xc5,0xf8,0x77};
 one("duplicate",dup,4,Decode64Bits,1,4);one("rex_position",rex,5,Decode64Bits,1,5);one("mandatory",mandatory,4,Decode32Bits,0,4);one("vex",vex,3,Decode64Bits,0,3);
 puts("BASELINE_OK cases=4");return 0;
}
