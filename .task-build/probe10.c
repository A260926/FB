#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void run(int bits,int nfade,UWORD extra,int pan,int frq,void *out,int nout){
  MikMod_RegisterDriver(&drv_nos); MikMod_SetRetriggerFade((ULONG)nfade);
  UWORD m=DMODE_SOFT_SNDFX|DMODE_INTERP|DMODE_STEREO|extra;
  if(bits==16)m|=DMODE_16BITS; if(bits==8)m|=0; if(bits==32)m|=DMODE_FLOAT;
  md_mode=m;md_mixfreq=44100;md_device=1;md_reverb=0;md_volume=md_sndfxvolume=128;md_pansep=128;
  MikMod_Init("");MikMod_SetNumVoices(0,1);
  static unsigned char rawa[2048],rawb[2048]; int i;
  for(i=0;i<512;i++){short v=(short)(((i%17)-8)*1500);rawa[2*i]=(unsigned char)(v&255);rawa[2*i+1]=(unsigned char)(((unsigned short)v)>>8);
    short y=(short)(500-i*3);rawb[2*i]=(unsigned char)(y&255);rawb[2*i+1]=(unsigned char)(((unsigned short)y)>>8);}
  SAMPLE*a=Sample_LoadRawMem((char*)rawa,1024,44100,1,SF_16BITS|SF_SIGNED);
  SAMPLE*b=Sample_LoadRawMem((char*)rawb,1024,44100,1,SF_16BITS|SF_SIGNED);
  a->flags|=SF_LOOP;a->loopstart=0;a->loopend=512; b->flags|=SF_LOOP;b->loopstart=0;b->loopend=512;
  MikMod_EnableOutput();
  Voice_SetVolume(0,256);Voice_SetPanning(0,pan);Voice_SetFrequency(0,frq);
  Voice_Play(0,a,0);
  { static short warm[8192]; VC_WriteBytes((SBYTE*)warm,64*4); }
  Voice_Play(0,b,0);
  VC_WriteBytes((SBYTE*)out,(ULONG)(nout*(bits==16?4:bits==8?2:8)));
  Voice_Stop(0);MikMod_DisableOutput();Sample_Free(a);Sample_Free(b);MikMod_Exit();
}
int main(void){
  static short o16[512]; static signed char o8[512]; static float o32[512]; int i;
  run(16,8,0,128,22050,o16,16);
  run(8,8,0,128,22050,o8,16);
  run(32,8,0,128,22050,o32,16);
  printf("16:"); for(i=0;i<8;i++)printf(" %d",o16[i]); printf("\n");
  printf("8 :"); for(i=0;i<8;i++)printf(" %d",(unsigned char)o8[i]); printf("\n");
  printf("32:"); for(i=0;i<8;i++)printf(" %.6f",o32[i]); printf("\n");
  return 0;
}
