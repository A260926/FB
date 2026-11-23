#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void){
    int hq;
    for(hq=0;hq<2;hq++){
        MikMod_RegisterDriver(&drv_nos);
        md_mode=(hq?DMODE_HQMIXER:0)|DMODE_16BITS|DMODE_STEREO|DMODE_SOFT_SNDFX|DMODE_INTERP;
        md_mixfreq=44100;md_device=1;md_reverb=0;md_volume=md_sndfxvolume=128;md_pansep=128;
        MikMod_Init("");MikMod_SetNumVoices(0,1);
        unsigned char rawa[1024];
        for(int i=0;i<512;i++){short v=(short)(((i%17)-8)*1500);rawa[2*i]=(unsigned char)(v&255);rawa[2*i+1]=(unsigned char)(((unsigned short)v)>>8);}
        SAMPLE*a=Sample_LoadRawMem((char*)rawa,1024,44100,1,SF_16BITS|SF_SIGNED);
        a->flags|=SF_LOOP;a->loopstart=0;a->loopend=512;
        MikMod_EnableOutput();
        Voice_SetVolume(0,256);Voice_SetPanning(0,128);Voice_SetFrequency(0,22050);
        Voice_Play(0,a,0);
        short warm[512]; VC_WriteBytes((SBYTE*)warm,sizeof(warm));
        printf("hq=%d after 512B warm pos=%ld\n",hq,(long)Voice_GetPosition(0));
        VC_WriteBytes((SBYTE*)warm,sizeof(warm));
        printf("hq=%d after 1024B warm pos=%ld\n",hq,(long)Voice_GetPosition(0));
        Voice_Stop(0);MikMod_DisableOutput();Sample_Free(a);MikMod_Exit();
    }
    return 0;
}
