#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static short buf[8192];
static int docase(int chunk){
    static short out[8192]; int n=0;
    MikMod_RegisterDriver(&drv_nos);
    md_mode=DMODE_HQMIXER|DMODE_16BITS|DMODE_STEREO|DMODE_SOFT_SNDFX;
    md_mixfreq=44100;md_device=1;md_reverb=0;md_volume=md_sndfxvolume=128;md_pansep=128;
    MikMod_Init("");MikMod_SetNumVoices(0,1);
    unsigned char raw[1024];
    for(int i=0;i<512;i++){short v=(short)(((i%17)-8)*1500);raw[2*i]=(unsigned char)(v&255);raw[2*i+1]=(unsigned char)(((unsigned short)v)>>8);}
    SAMPLE*a=Sample_LoadRawMem((char*)raw,1024,44100,1,SF_16BITS|SF_SIGNED);
    a->flags|=SF_LOOP;a->loopstart=0;a->loopend=512;
    MikMod_EnableOutput();
    Voice_SetVolume(0,256);Voice_SetPanning(0,130);Voice_SetFrequency(0,30800);
    Voice_Play(0,a,0);
    int total=2000;
    while(n<total){int c=chunk;if(n+c>total)c=total-n;VC_WriteBytes((SBYTE*)(out+2*n),(ULONG)c*4);n+=c;}
    Voice_Stop(0);MikMod_DisableOutput();Sample_Free(a);MikMod_Exit();
    memcpy(buf,out,sizeof(short)*total*2); return 0;
}
int main(void){
    static short big[8192],f1[8192],f7[8192]; int i,d1=0,d7=0;
    docase(2000); memcpy(big,buf,sizeof(big));
    docase(1); memcpy(f1,buf,sizeof(big));
    docase(7); memcpy(f7,buf,sizeof(big));
    for(i=0;i<4000;i++){if(big[i]!=f1[i])d1++; if(big[i]!=f7[i])d7++;}
    printf("HQ only: diff1=%d diff7=%d\n",d1,d7);
    return 0;
}
