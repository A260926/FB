#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SAMPLE *mkraw(const char *raw,int len){return Sample_LoadRawMem(raw,len,44100,1,SF_16BITS|SF_SIGNED);}

int main(void){
    /* simple constant sample: verify DC gain and conversion */
    int n=64,i; unsigned char *raw=malloc(n*2); short d[512];
    static short out[512];
    for(i=0;i<n;i++){short v=1000; raw[2*i]=(unsigned char)(v&255); raw[2*i+1]=(unsigned char)(((unsigned short)v)>>8);}
    MikMod_RegisterDriver(&drv_nos);
    md_mode=DMODE_16BITS|DMODE_STEREO|DMODE_SOFT_SNDFX|DMODE_INTERP;
    md_mixfreq=44100;md_device=1;md_reverb=0;md_volume=md_sndfxvolume=128;md_pansep=128;
    MikMod_Init("");MikMod_SetNumVoices(0,1);
    SAMPLE *a=mkraw((char*)raw,n*2);
    a->flags|=SF_LOOP;a->loopstart=0;a->loopend=a->length;
    MikMod_EnableOutput();
    Voice_SetVolume(0,256);Voice_SetPanning(0,128);Voice_SetFrequency(0,44100);
    Voice_Play(0,a,0);
    VC_WriteBytes((SBYTE*)d,sizeof(d));
    printf("dc first frames: "); for(i=0;i<8;i++)printf("%d ",d[i]); printf("\n");
    printf("dc mid: "); for(i=200;i<208;i++)printf("%d ",d[i]); printf("\n");
    Voice_Stop(0);MikMod_DisableOutput();Sample_Free(a);MikMod_Exit();free(raw);
    return 0;
}
