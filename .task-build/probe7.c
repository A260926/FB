#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static short out[8192];
static void run(int hq,int nfade,int nread,int chunk){
    MikMod_RegisterDriver(&drv_nos);
    MikMod_SetRetriggerFade((ULONG)nfade);
    md_mode=(hq?DMODE_HQMIXER:0)|DMODE_16BITS|DMODE_STEREO|DMODE_SOFT_SNDFX|DMODE_INTERP;
    md_mixfreq=44100;md_device=1;md_reverb=0;md_volume=md_sndfxvolume=128;md_pansep=128;
    MikMod_Init("");MikMod_SetNumVoices(0,1);
    unsigned char rawa[1024],rawb[1024];
    for(int i=0;i<512;i++){short v=(short)(((i%17)-8)*1500);rawa[2*i]=(unsigned char)(v&255);rawa[2*i+1]=(unsigned char)(((unsigned short)v)>>8);
        short y=(short)(500-i*3);rawb[2*i]=(unsigned char)(y&255);rawb[2*i+1]=(unsigned char)(((unsigned short)y)>>8);}
    SAMPLE*a=Sample_LoadRawMem((char*)rawa,1024,44100,1,SF_16BITS|SF_SIGNED);
    SAMPLE*b=Sample_LoadRawMem((char*)rawb,1024,44100,1,SF_16BITS|SF_SIGNED);
    a->flags|=SF_LOOP;a->loopstart=0;a->loopend=512;
    b->flags|=SF_LOOP;b->loopstart=0;b->loopend=512;
    MikMod_EnableOutput();
    Voice_SetVolume(0,256);Voice_SetPanning(0,128);Voice_SetFrequency(0,22050);
    Voice_Play(0,a,0);
    { short warm[512]; VC_WriteBytes((SBYTE*)warm,sizeof(warm)); }
    Voice_Play(0,b,0);
    int n=0;
    while(n<nread){int c=chunk;if(n+c>nread)c=nread-n;VC_WriteBytes((SBYTE*)(out+2*n),(ULONG)c*4);n+=c;}
    Voice_Stop(0);MikMod_DisableOutput();Sample_Free(a);Sample_Free(b);MikMod_Exit();
}
int main(void){
    int i;
    for(int hq=0;hq<2;hq++)for(int nf=4;nf<=8;nf+=4){
        run(hq,nf,16,16);
        printf("hq=%d N=%d:",hq,nf); for(i=0;i<16;i++)printf(" %d",out[i]); printf("\n");
    }
    return 0;
}
