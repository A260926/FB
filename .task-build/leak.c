#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int VC_DbgLiveGen(void),VC_DbgAllocGen(void),VC_DbgFreeGen(void);
int main(void){
  MikMod_RegisterDriver(&drv_nos); MikMod_SetRetriggerFade(64);
  md_mode=DMODE_16BITS|DMODE_STEREO|DMODE_SOFT_SNDFX|DMODE_INTERP;
  md_mixfreq=44100;md_device=1;md_reverb=0;md_volume=md_sndfxvolume=128;md_pansep=128;
  MikMod_Init("");MikMod_SetNumVoices(0,1);
  static unsigned char ra[1024],rb[640];int i,it;
  for(i=0;i<512;i++){short v=(short)(((i%17)-8)*1500);ra[2*i]=(unsigned char)(v&255);ra[2*i+1]=(unsigned char)(((unsigned short)v)>>8);}
  for(i=0;i<320;i++){short v=(short)(500-i*3);rb[2*i]=(unsigned char)(v&255);rb[2*i+1]=(unsigned char)(((unsigned short)v)>>8);}
  MikMod_EnableOutput();Voice_SetVolume(0,256);Voice_SetPanning(0,128);Voice_SetFrequency(0,22050);
  {short w[512];VC_WriteBytes((SBYTE*)w,512);}
  for(it=0;it<20000;it++){
    SAMPLE*a=Sample_LoadRawMem((char*)ra,1024,44100,1,SF_16BITS|SF_SIGNED);
    SAMPLE*b=Sample_LoadRawMem((char*)rb,640,44100,1,SF_16BITS|SF_SIGNED);
    a->flags|=SF_LOOP;a->loopstart=0;a->loopend=512;b->flags|=SF_LOOP;b->loopstart=0;b->loopend=320;
    Voice_Play(0,a,0);{short w[512];VC_WriteBytes((SBYTE*)w,512);}
    Voice_Play(0,b,0); Sample_Free(a);
    {short w[512];VC_WriteBytes((SBYTE*)w,512);}
    Sample_Free(b); Voice_Stop(0);
  }
  printf("mid: live=%d alloc=%d free=%d\n",VC_DbgLiveGen(),VC_DbgAllocGen(),VC_DbgFreeGen());
  MikMod_DisableOutput();MikMod_Exit();
  printf("exit: live=%d alloc=%d free=%d\n",VC_DbgLiveGen(),VC_DbgAllocGen(),VC_DbgFreeGen());
  return 0;
}
