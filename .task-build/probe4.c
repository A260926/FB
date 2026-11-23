#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static short buf[4096];
static int docase(int chunk, int mode){
    static short out[4096]; int n=0;
    MikMod_RegisterDriver(&drv_nos);
    md_mode=mode|DMODE_16BITS|DMODE_STEREO|DMODE_SOFT_SNDFX|DMODE_INTERP;
    md_mixfreq=44100;md_device=1;md_reverb=0;md_volume=md_sndfxvolume=128;md_pansep=128;
    MikMod_Init("");MikMod_SetNumVoices(0,1);
    unsigned char rawa[1024],rawb[1024];
    for(int i=0;i<512;i++){short v=(short)(((i%17)-8)*1500);rawa[2*i]=(unsigned char)(v&255);rawa[2*i+1]=(unsigned char)(((unsigned short)v)>>8);
        short y=-8000;rawb[2*i]=(unsigned char)(y&255);rawb[2*i+1]=(unsigned char)(((unsigned short)y)>>8);}
    SAMPLE*a=Sample_LoadRawMem((char*)rawa,1024,44100,1,SF_16BITS|SF_SIGNED);
    SAMPLE*b=Sample_LoadRawMem((char*)rawb,1024,44100,1,SF_16BITS|SF_SIGNED);
    a->flags|=SF_LOOP;a->loopstart=0;a->loopend=512;
    b->flags|=SF_LOOP;b->loopstart=0;b->loopend=512;
    MikMod_EnableOutput();
    Voice_SetVolume(0,256);Voice_SetPanning(0,130);Voice_SetFrequency(0,30800);
    Voice_Play(0,a,0);
    { short warm[2048]; VC_WriteBytes((SBYTE*)warm,sizeof(warm)); }
    Voice_Play(0,b,0);
    int total=32;
    while(n<total){ int c=chunk; if(n+c>total)c=total-n; VC_WriteBytes((SBYTE*)(out+2*n),(ULONG)c*4); n+=c; }
    Voice_Stop(0);MikMod_DisableOutput();Sample_Free(a);Sample_Free(b);MikMod_Exit();
    memcpy(buf,out,total*2*sizeof(short)); return 0;
}
int main(void){
    int chunks[]={32,1,2,4,7,31}; int c;
    for(int k=0;k<6;k++){ docase(chunks[k],0); printf("chunk %2d: ",chunks[k]); for(c=0;c<20;c++)printf("%d ",buf[c]); printf("\n"); }
    printf("--- HQ ---\n");
    for(int k=0;k<6;k++){ docase(chunks[k],DMODE_HQMIXER); printf("chunk %2d: ",chunks[k]); for(c=0;c<12;c++)printf("%d ",buf[c]); printf("\n"); }
    return 0;
}
