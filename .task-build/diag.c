#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void){
  MikMod_RegisterDriver(&drv_nos);
  md_mode=DMODE_16BITS|DMODE_STEREO|DMODE_SOFT_SNDFX|DMODE_INTERP;
  md_mixfreq=44100;md_device=1;md_reverb=0;md_volume=md_sndfxvolume=128;md_pansep=128;
  MikMod_Init("");MikMod_SetNumVoices(0,1);
  unsigned char raw[1024]; int i;
  for(i=0;i<512;i++){short v=(short)(((i%17)-8)*1500);raw[2*i]=(unsigned char)(v&255);raw[2*i+1]=(unsigned char)(((unsigned short)v)>>8);}
  SAMPLE*a=Sample_LoadRawMem((char*)raw,1024,44100,1,SF_16BITS|SF_SIGNED);
  a->flags|=SF_LOOP;a->loopstart=0;a->loopend=512;
  MikMod_EnableOutput();
  Voice_SetVolume(0,256);Voice_SetPanning(0,128);Voice_SetFrequency(0,22050);
  Voice_Play(0,a,0);
  short out[4096]; int bytes;
  bytes=2048; VC_WriteBytes((SBYTE*)out,bytes);
  printf("after warm pos=%ld first out %d %d\n",(long)Voice_GetPosition(0),out[0],out[1]);
  bytes=40; VC_WriteBytes((SBYTE*)out,bytes);
  printf("next pos=%ld out:",(long)Voice_GetPosition(0));
  for(i=0;i<8;i++)printf(" %d",out[i]); printf("\n");
  Voice_Stop(0);MikMod_DisableOutput();Sample_Free(a);MikMod_Exit();
  return 0;
}
