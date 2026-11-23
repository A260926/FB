#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void){
  MikMod_RegisterDriver(&drv_nos); MikMod_SetRetriggerFade(0);
  md_mode=DMODE_16BITS|DMODE_STEREO|DMODE_SOFT_SNDFX|DMODE_INTERP;
  md_mixfreq=44100;md_device=1;md_reverb=0;md_volume=md_sndfxvolume=128;md_pansep=128;
  MikMod_Init("");MikMod_SetNumVoices(0,1);
  static unsigned char rb[640];int i;
  for(i=0;i<320;i++){short v=(short)(500-i*3);rb[2*i]=(unsigned char)(v&255);rb[2*i+1]=(unsigned char)(((unsigned short)v)>>8);}
  SAMPLE*b=Sample_LoadRawMem((char*)rb,640,44100,1,SF_16BITS|SF_SIGNED);
  b->flags|=SF_LOOP;b->loopstart=0;b->loopend=320;
  MikMod_EnableOutput();Voice_SetVolume(0,256);Voice_SetPanning(0,128);Voice_SetFrequency(0,22050);
  Voice_Play(0,b,0);
  short o[64]; VC_WriteBytes((SBYTE*)o,32*4);
  printf("fresh:"); for(i=0;i<12;i++)printf(" %d",o[i]); printf("\n");
  Voice_Stop(0);MikMod_DisableOutput();Sample_Free(b);MikMod_Exit();
  return 0;
}
