#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static short buf[4096];
static int docase(int chunk, int pan, int vol, int frq){
    static short out[4096]; int n=0;
    MikMod_RegisterDriver(&drv_nos);
    md_mode=DMODE_16BITS|DMODE_STEREO|DMODE_SOFT_SNDFX|DMODE_INTERP;
    md_mixfreq=44100;md_device=1;md_reverb=0;md_volume=md_sndfxvolume=128;md_pansep=128;
    MikMod_Init("");MikMod_SetNumVoices(0,1);
    unsigned char raw[1024];
    for(int i=0;i<512;i++){short v=(short)(((i%17)-8)*1500);raw[2*i]=(unsigned char)(v&255);raw[2*i+1]=(unsigned char)(((unsigned short)v)>>8);}
    SAMPLE*a=Sample_LoadRawMem((char*)raw,1024,44100,1,SF_16BITS|SF_SIGNED);
    a->flags|=SF_LOOP;a->loopstart=0;a->loopend=512;
    MikMod_EnableOutput();
    Voice_SetVolume(0,vol);Voice_SetPanning(0,pan);Voice_SetFrequency(0,frq);
    Voice_Play(0,a,0);
    int total=32;
    while(n<total){ int c=chunk; if(n+c>total)c=total-n; VC_WriteBytes((SBYTE*)(out+2*n),(ULONG)c*4); n+=c; }
    Voice_Stop(0);MikMod_DisableOutput();Sample_Free(a);MikMod_Exit();
    memcpy(buf,out,total*2*sizeof(short)); return 0;
}
int main(void){
    int chunks[]={32,1,2,4,7,31}; int c;
    for(int k=0;k<6;k++){ docase(chunks[k],130,256,30800); printf("chunk %2d: ",chunks[k]); for(c=0;c<10;c++)printf("%d ",buf[c]); printf("\n"); }
    printf("--- no pan ramp, vol=256 pan=128 frq=44100 ---\n");
    for(int k=0;k<6;k++){ docase(chunks[k],128,256,44100); printf("chunk %2d: ",chunks[k]); for(c=0;c<10;c++)printf("%d ",buf[c]); printf("\n"); }
    return 0;
}
