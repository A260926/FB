#include "mikmod.h"
#include "mikmod_internals.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static short out[4096];
static void run(int chunk,int hq,int stage,int nfade,int nread){
  MikMod_RegisterDriver(&drv_nos); MikMod_SetRetriggerFade(nfade);
  UWORD m=DMODE_SOFT_SNDFX|DMODE_INTERP|(hq?DMODE_HQMIXER:0)|DMODE_STEREO;
  if(stage==0)m|=DMODE_16BITS;
  md_mode=m;md_mixfreq=44100;md_device=1;md_reverb=0;md_volume=md_sndfxvolume=128;md_pansep=128;
  MikMod_Init("");MikMod_SetNumVoices(0,1);
  static unsigned char ra[1024],rb[640];int i;
  for(i=0;i<512;i++){short v=(short)(((i%17)-8)*1500);ra[2*i]=(unsigned char)(v&255);ra[2*i+1]=(unsigned char)(((unsigned short)v)>>8);}
  for(i=0;i<320;i++){short v=(short)(500-i*3);rb[2*i]=(unsigned char)(v&255);rb[2*i+1]=(unsigned char)(((unsigned short)v)>>8);}
  SAMPLE*a=Sample_LoadRawMem((char*)ra,1024,44100,1,SF_16BITS|SF_SIGNED);SAMPLE*b=Sample_LoadRawMem((char*)rb,640,44100,1,SF_16BITS|SF_SIGNED);
  a->flags|=SF_LOOP;a->loopstart=0;a->loopend=512;b->flags|=SF_LOOP;b->loopstart=0;b->loopend=320;
  MikMod_EnableOutput();Voice_SetVolume(0,256);Voice_SetPanning(0,130);Voice_SetFrequency(0,30800);
  Voice_Play(0,a,0);
  {static short w[4096];VC_WriteBytes((SBYTE*)w,96*(stage==0?4:2));}
  Voice_Play(0,b,0);
  int n=0,bps=(stage==0?2:1)*2;
  while(n<nread){int c=chunk;if(n+c>nread)c=nread-n;VC_WriteBytes((SBYTE*)(out+n*(bps/2)),(ULONG)c*bps);n+=c;}
  Voice_Stop(0);MikMod_DisableOutput();Sample_Free(a);Sample_Free(b);MikMod_Exit();
}
static int cmpc(int hq,int stage,int nfade,int nread){
  static short A[4096],B[4096],C[4096],D[4096];int i,d1=0,d7=0,d31=0;
  run(4096,hq,stage,nfade,nread);memcpy(A,out,sizeof A);
  run(1,hq,stage,nfade,nread);memcpy(B,out,sizeof B);
  run(7,hq,stage,nfade,nread);memcpy(C,out,sizeof C);
  run(31,hq,stage,nfade,nread);memcpy(D,out,sizeof D);
  int bytes=nread*(stage==0?2:1)*2;
  for(i=0;i<bytes/2;i++){if(A[i]!=B[i])d1++;if(A[i]!=C[i])d7++;if(A[i]!=D[i])d31++;}
  printf("hq=%d stage=%d N=%d read=%d : d1=%d d7=%d d31=%d\n",hq,stage,nfade,nread,d1,d7,d31);
  return d1+d7+d31;
}
int main(void){int t=0;
 t+=cmpc(0,0,8,40); t+=cmpc(0,0,16,40);
 t+=cmpc(1,0,8,40); t+=cmpc(1,0,16,40); t+=cmpc(1,0,16,64);
 t+=cmpc(1,1,16,40);
 printf("TOTAL=%d\n",t);return 0;}
