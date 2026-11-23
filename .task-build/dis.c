#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* disabled: compare PCM of retrigger vs fresh single-play of B; must be identical */
static void run(int retrig, short*out,int n){
  MikMod_RegisterDriver(&drv_nos); MikMod_SetRetriggerFade(0);
  md_mode=DMODE_16BITS|DMODE_STEREO|DMODE_SOFT_SNDFX|DMODE_INTERP;
  md_mixfreq=44100;md_device=1;md_reverb=0;md_volume=md_sndfxvolume=128;md_pansep=128;
  MikMod_Init("");MikMod_SetNumVoices(0,1);
  static unsigned char ra[1024],rb[640];int i;
  for(i=0;i<512;i++){short v=(short)(((i%17)-8)*1500);ra[2*i]=(unsigned char)(v&255);ra[2*i+1]=(unsigned char)(((unsigned short)v)>>8);}
  for(i=0;i<320;i++){short v=(short)(500-i*3);rb[2*i]=(unsigned char)(v&255);rb[2*i+1]=(unsigned char)(((unsigned short)v)>>8);}
  SAMPLE*a=Sample_LoadRawMem((char*)ra,1024,44100,1,SF_16BITS|SF_SIGNED);
  SAMPLE*b=Sample_LoadRawMem((char*)rb,640,44100,1,SF_16BITS|SF_SIGNED);
  a->flags|=SF_LOOP;a->loopstart=0;a->loopend=512;b->flags|=SF_LOOP;b->loopstart=0;b->loopend=320;
  MikMod_EnableOutput();Voice_SetVolume(0,256);Voice_SetPanning(0,128);Voice_SetFrequency(0,22050);
  if(retrig){ Voice_Play(0,a,0); {short w[4096];VC_WriteBytes((SBYTE*)w,96*4);} }
  Voice_Play(0,b,0);
  VC_WriteBytes((SBYTE*)out,(ULONG)n*4);
  Voice_Stop(0);MikMod_DisableOutput();Sample_Free(a);Sample_Free(b);MikMod_Exit();
}
int main(void){
  short r[64],f[64];int i,d=0;
  run(1,r,32); run(0,f,32);
  for(i=0;i<64;i++) if(r[i]!=f[i]){d++; if(d<6)printf("[%d] r=%d f=%d\n",i,r[i],f[i]);}
  printf("disabled mismatch=%d\n",d);
  return 0;
}
