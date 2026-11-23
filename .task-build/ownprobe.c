#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static SAMPLE *ld(const short*d,int n,SAMPLE**keep){
  static unsigned char raw[2048];int i;
  for(i=0;i<n;i++){unsigned short u=(unsigned short)d[i];raw[2*i]=(unsigned char)(u&255);raw[2*i+1]=(unsigned char)(u>>8);}
  SAMPLE*s=Sample_LoadRawMem((char*)raw,n*2,44100,1,SF_16BITS|SF_SIGNED);
  s->flags|=SF_LOOP;s->loopstart=0;s->loopend=n;*keep=s;return s;
}
int main(void){
  int dofree = 1;
  MikMod_RegisterDriver(&drv_nos);
  MikMod_SetRetriggerFade(24);
  md_mode=DMODE_16BITS|DMODE_STEREO|DMODE_SOFT_SNDFX|DMODE_INTERP;
  md_mixfreq=44100;md_device=1;md_reverb=0;md_volume=md_sndfxvolume=128;md_pansep=128;
  MikMod_Init("");MikMod_SetNumVoices(0,1);
  short a[512],b[320],cc[320];int i;
  for(i=0;i<512;i++)a[i]=(short)(((i%17)-8)*1500);
  for(i=0;i<320;i++){b[i]=(short)(500-i*3);cc[i]=(short)(((i%23)-11)*900);}
  SAMPLE *sa,*sb,*sc;
  ld(a,512,&sa);ld(b,320,&sb);
  MikMod_EnableOutput();
  Voice_SetVolume(0,256);Voice_SetPanning(0,128);Voice_SetFrequency(0,22050);
  Voice_Play(0,sa,0);
  { short w[4096]; VC_WriteBytes((SBYTE*)w,96*4); }
  printf("pos after warm=%ld\n",(long)Voice_GetPosition(0));
  Voice_Play(0,sb,0);
  short out[512];
  VC_WriteBytes((SBYTE*)out,12*4);
  printf("first12:"); for(i=0;i<24;i++)printf(" %d",out[i]); printf("\n");
  if(dofree){Sample_Free(sa);}
  if(dofree){ld(cc,320,&sc);}
  { short out2[512]; VC_WriteBytes((SBYTE*)out2,12*4);
    printf("next12:"); for(i=0;i<24;i++)printf(" %d",out2[i]); printf("\n"); }
  Voice_Stop(0);MikMod_DisableOutput();MikMod_Exit();
  return 0;
}
