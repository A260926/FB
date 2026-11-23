#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* print exact per-sample values for a simple case, controlled by chunking only at boundaries */
int main(void){
    MikMod_RegisterDriver(&drv_nos);
    md_mode=DMODE_16BITS|DMODE_STEREO|DMODE_SOFT_SNDFX; /* no interp */
    md_mixfreq=44100;md_device=1;md_reverb=0;md_volume=md_sndfxvolume=128;md_pansep=128;
    MikMod_Init("");MikMod_SetNumVoices(0,1);
    unsigned char raw[1024];
    for(int i=0;i<512;i++){short v=1000;raw[2*i]=(unsigned char)(v&255);raw[2*i+1]=(unsigned char)(((unsigned short)v)>>8);}
    SAMPLE*a=Sample_LoadRawMem((char*)raw,1024,44100,1,SF_16BITS|SF_SIGNED);
    a->flags|=SF_LOOP;a->loopstart=0;a->loopend=512;
    MikMod_EnableOutput();
    Voice_SetVolume(0,256);Voice_SetPanning(0,128);Voice_SetFrequency(0,44100);
    Voice_Play(0,a,0);
    short out[8192];
    int n=0, total=2000; int chunk=1;
    while(n<total){int c=chunk;if(n+c>total)c=total-n;VC_WriteBytes((SBYTE*)(out+2*n),(ULONG)c*4);n+=c;}
    printf("DC chunk=1 first 12:"); for(int i=0;i<12;i++)printf(" %d",out[i]); printf("\n");
    printf("DC chunk=1 @100:"); for(int i=200;i<212;i++)printf(" %d",out[i]); printf("\n");
    Voice_Stop(0);MikMod_DisableOutput();Sample_Free(a);MikMod_Exit();
    return 0;
}
