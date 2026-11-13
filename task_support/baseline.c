#include <beaengine/BeaEngine.h>
#include <string.h>
#include <stdio.h>
static int check(const unsigned char*b,size_t n,const char*word){
 DISASM d;memset(&d,0,sizeof(d));d.EIP=(UIntPtr)b;d.SecurityBlock=(UInt32)n;d.Archi=64;d.VirtualAddr=0x1000;d.Options=ShowEVEXMasking;
 int len=Disasm(&d);if(len!=(int)n||!strstr(d.CompleteInstr,word))return 1;
 printf("DECODE len=%d %s\n",len,d.CompleteInstr);return 0;
}
int main(void){
 const unsigned char vex[]={0xc4,0xe2,0x6d,0x90,0x0c,0x98};
 const unsigned char gather[]={0x62,0xf2,0x7d,0x49,0x90,0x4c,0x98,0x02};
 const unsigned char scatter[]={0x62,0xf2,0x7d,0x49,0xa0,0x4c,0x98,0x02};
 const unsigned char rip[]={0x48,0x8b,0x05,0x04,0,0,0};
 if(check(vex,sizeof(vex),"vpgatherdd")||check(gather,sizeof(gather),"vpgatherdd")||check(scatter,sizeof(scatter),"vpscatterdd")||check(rip,sizeof(rip),"mov"))return 2;
 puts("BEA_VSIB_BASELINE_OK");return 0;
}
