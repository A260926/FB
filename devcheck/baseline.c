/* SPDX-License-Identifier: MIT */
#include <voAAC.h>
#include <cmnMemory.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aacenc_core.h"
static VO_AUDIO_CODECAPI api;static VO_MEM_OPERATOR mem;
static VO_HANDLE init(void){VO_HANDLE h;VO_CODEC_INIT_USERDATA u;u.memflag=VO_IMF_USERMEMOPERATOR;u.memData=&mem;
 if(api.Init(&h,VO_AUDIO_CodingAAC,&u))exit(2);return h;}
static unsigned encode(VO_HANDLE h,short *pcm,unsigned char*out){VO_CODECBUFFER in={0},o={0};VO_AUDIO_OUTPUTINFO info={0};
 in.Buffer=(unsigned char*)pcm;in.Length=4096;o.Buffer=out;o.Length=8192;
 if(api.SetInputData(h,&in)||api.GetOutputData(h,&o,&info))exit(3);
 if(o.Length<7||out[0]!=255||(out[1]&0xf6)!=0xf0)exit(4);
 unsigned len=((out[3]&3)<<11)|(out[4]<<3)|(out[5]>>5);if(len!=o.Length)exit(5);return len;}
int main(void){voGetAACEncAPI(&api);mem.Alloc=cmnMemAlloc;mem.Copy=cmnMemCopy;mem.Free=cmnMemFree;mem.Set=cmnMemSet;mem.Check=cmnMemCheck;
 VO_HANDLE a=init(),b=init();AACENC_PARAM p={0};p.sampleRate=44100;p.bitRate=64000;p.nChannels=2;p.adtsUsed=1;
 if(api.SetParam(a,VO_PID_AAC_ENCPARAM,&p)||api.SetParam(b,VO_PID_AAC_ENCPARAM,&p))return 6;
 short pcm[2048];unsigned char oa[8192],ob[8192];uint32_t rng=0x12345678;unsigned differences=0;long bytes=0;
 for(int i=0;i<48;i++){
  for(int j=0;j<2048;j++){rng=1664525*rng+1013904223;pcm[j]=(short)((int)(rng>>18)-8192);}
  if(i==16){int before=((AAC_ENCODER*)b)->qcKernel.padding.paddingRest;
   if(api.SetParam(b,VO_PID_AAC_ENCPARAM,&p))return 7;
   printf("AAC_REOPEN padding_before=%d padding_after=%d\n",before,((AAC_ENCODER*)b)->qcKernel.padding.paddingRest);
  }
  unsigned na=encode(a,pcm,oa),nb=encode(b,pcm,ob);bytes+=na+nb;
  if(na!=nb||memcmp(oa,ob,na))differences++;
 }
 api.Uninit(a);api.Uninit(b);
 if(!differences||bytes<=0)return 8;
 printf("AAC_BASELINE frames=48 adts=96 changed_after_reopen=%u bytes=%ld\n",differences,bytes);return 0;
}
