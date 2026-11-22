#ifndef VOICE_FIXTURE_H
#define VOICE_FIXTURE_H
#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
static SAMPLE *vf_a,*vf_b;
static int vf_start(void) {
    char a[1024],b[1024]; int i; short warm[512];
    md_mode=DMODE_16BITS|DMODE_STEREO|DMODE_SOFT_SNDFX|DMODE_INTERP;
    md_mixfreq=44100;md_device=1;md_reverb=0;
    md_volume=md_sndfxvolume=128;md_pansep=128;
    if(MikMod_Init(""))return 10;
    if(MikMod_SetNumVoices(0,1))return 11;
    for(i=0;i<512;i++) {
        short x=(short)(((i%32)-16)*400), y=-8000;
        a[2*i]=(char)(x&255);a[2*i+1]=(char)(((unsigned short)x)>>8);
        b[2*i]=(char)(y&255);b[2*i+1]=(char)(((unsigned short)y)>>8);
    }
    vf_a=Sample_LoadRawMem(a,sizeof(a),44100,1,SF_16BITS|SF_SIGNED);
    vf_b=Sample_LoadRawMem(b,sizeof(b),44100,1,SF_16BITS|SF_SIGNED);
    if(!vf_a||!vf_b)return 12;
    vf_a->flags|=SF_LOOP;vf_a->loopstart=0;vf_a->loopend=vf_a->length;
    vf_b->flags|=SF_LOOP;vf_b->loopstart=0;vf_b->loopend=vf_b->length;
    if(MikMod_EnableOutput())return 13;
    Voice_SetVolume(0,256);Voice_SetPanning(0,128);Voice_SetFrequency(0,44100);
    Voice_Play(0,vf_a,0);
    if(VC_WriteBytes((SBYTE*)warm,sizeof(warm))!=sizeof(warm))return 14;
    return 0;
}
static void vf_stop(void) {
    Voice_Stop(0);MikMod_DisableOutput();
    if(vf_a)Sample_Free(vf_a);
    if(vf_b)Sample_Free(vf_b);
    vf_a=vf_b=NULL;MikMod_Exit();
}
static int vf_read(short *p,int frames) {
    return VC_WriteBytes((SBYTE*)p,(ULONG)frames*4)==(ULONG)frames*4?0:15;
}
#endif
