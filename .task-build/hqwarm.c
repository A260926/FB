#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void run(int warmchunk){
  MikMod_RegisterDriver(&drv_nos); MikMod_SetRetriggerFade(16);
  md_mode=DMODE_HQMIXER|DMODE_SOFT_SNDFX|DMODE_INTERP|DMODE_STEREO|DMODE_16BITS;
  md_mixfreq=44100;md_device=1;md_reverb=0;md_volume=md_sndfxvolume=128;md_pansep=128;
  MikMod_Init("");MikMod_SetNumVoices(0,1);
  static unsigned char ra[1024],rb[640];int i;
  for(i=0;i<512;i++){short v=(short)(((i%17)-8)*1500);ra[2*i]=(unsigned char)(v&255);ra[2*i+1]=(unsigned char)(((unsigned short)v)>>8);}
  for(i=0;i<320;i++){short v=(short)(500-i*3);rb[2*i]=(unsigned char)(v&255);rb[2*i+1]=(unsigned char)(((unsigned short)v)>>8);}
  SAMPLE*a=Sample_LoadRawMem((char*)ra,1024,44100,1,SF_16BITS|SF_SIGNED);SAMPLE*b=Sample_LoadRawMem((char*)rb,640,44100,1,SF_16BITS|SF_SIGNED);
  a->flags|=SF_LOOP;a->loopstart=0;a->loopend=512;b->flags|=SF_LOOP;b->loopstart=0;b->loopend=320;
  MikMod_EnableOutput();Voice_SetVolume(0,256);Voice_SetPanning(0,130);Voice_SetFrequency(0,30800);
  Voice_Play(0,a,0);
  { short w[4096]; int n=0; while(n<96){int c=warmchunk;if(n+c>96)c=96-n;VC_WriteBytes((SBYTE*)(w+2*n),c*4);n+=c;} }
  printf("warmchunk=%d pos_after=%ld\n",warmchunk,(long)Voice_GetPosition(0));
  Voice_Stop(0);MikMod_DisableOutput();Sample_Free(a);Sample_Free(b);MikMod_Exit();
}
int main(void){run(96);run(64);run(1);run(7);return 0;}
