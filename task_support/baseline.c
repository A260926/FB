#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aacenc_lib.h"
#include "aacdecoder_lib.h"
#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"AAC baseline failure line=%d\n",__LINE__);return 1;} }while(0)
static int run(int transport,int subs,int version){
 HANDLE_AACENCODER enc=0;HANDLE_AACDECODER dec;AACENC_InfoStruct info={0};
 UCHAR out[16384];INT_PCM pcm[2048],decoded[8192];uint64_t hash=14695981039346656037ULL,energy=0;
 int packets=0,frames=0,step,consumed=0,eof=0;
 REQUIRE(aacEncOpen(&enc,0,2)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(enc,AACENC_AOT,AOT_AAC_LC)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(enc,AACENC_SAMPLERATE,48000)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(enc,AACENC_CHANNELMODE,MODE_2)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(enc,AACENC_CHANNELORDER,1)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(enc,AACENC_BITRATE,128000)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(enc,AACENC_AFTERBURNER,0)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(enc,AACENC_TRANSMUX,transport)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(enc,AACENC_TPSUBFRAMES,subs)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(enc,AACENC_AUDIOMUXVER,version)==AACENC_OK);
 REQUIRE(aacEncEncode(enc,0,0,0,0)==AACENC_OK);
 REQUIRE(aacEncInfo(enc,&info)==AACENC_OK&&info.frameLength==1024);
 dec=aacDecoder_Open((TRANSPORT_TYPE)transport,1);REQUIRE(dec);
 if(transport==TT_MP4_RAW){UCHAR *p=info.confBuf;UINT n=info.confSize;REQUIRE(aacDecoder_ConfigRaw(dec,&p,&n)==AAC_DEC_OK);}
 REQUIRE(aacDecoder_SetParam(dec,AAC_PCM_LIMITER_ENABLE,0)==AAC_DEC_OK);
 for(step=0;step<64;step++){
  AACENC_BufDesc ib={0},ob={0};AACENC_InArgs ia={0};AACENC_OutArgs oa={0};
  int iid=IN_AUDIO_DATA,oid=OUT_BITSTREAM_DATA,is=sizeof pcm,os=sizeof out,ie=sizeof(INT_PCM),oe=1,j;
  void *ip=pcm,*op=out;AACENC_ERROR rc;
  for(j=0;j<1024;j++){unsigned t=(unsigned)(step*1024+j);pcm[2*j]=(INT_PCM)(((int)(t%97)-48)*180);pcm[2*j+1]=(INT_PCM)(((int)(t%131)-65)*120);}
  ib.numBufs=ob.numBufs=1;ib.bufs=&ip;ob.bufs=&op;ib.bufferIdentifiers=&iid;ob.bufferIdentifiers=&oid;
  ib.bufSizes=&is;ob.bufSizes=&os;ib.bufElSizes=&ie;ob.bufElSizes=&oe;ia.numInSamples=step<20?2048:-1;
  rc=aacEncEncode(enc,&ib,&ob,&ia,&oa);if(rc==AACENC_ENCODE_EOF){eof=1;break;}REQUIRE(rc==AACENC_OK);
  if(step<20){REQUIRE(oa.numInSamples==2048);consumed+=oa.numInSamples;}
  if(oa.numOutBytes){UCHAR*p=out;UINT n=(UINT)oa.numOutBytes,v=n;int drain;
   for(j=0;j<oa.numOutBytes;j++)hash=(hash^out[j])*1099511628211ULL;
   packets++;REQUIRE(aacDecoder_Fill(dec,&p,&n,&v)==AAC_DEC_OK&&v==0);
   for(drain=0;drain<8;drain++){CStreamInfo*si;AAC_DECODER_ERROR dr=aacDecoder_DecodeFrame(dec,decoded,8192,0);
    if(dr==AAC_DEC_NOT_ENOUGH_BITS)break;REQUIRE(dr==AAC_DEC_OK);
    si=aacDecoder_GetStreamInfo(dec);REQUIRE(si->frameSize==1024&&si->numChannels==2&&si->sampleRate==48000);frames++;
    for(j=0;j<2048;j++)energy+=(uint64_t)(decoded[j]<0?-(int)decoded[j]:decoded[j]);
   }REQUIRE(drain<8);
  }
 }
 REQUIRE(eof&&consumed==40960&&packets>0&&frames>=20&&energy>0);
 printf("AAC_NORMAL transport=%d subframes=%d version=%d packets=%d decoded=%d consumed=%d energy=%llu fnv=%016llx\n",transport,subs,version,packets,frames,consumed,(unsigned long long)energy,(unsigned long long)hash);
 aacDecoder_Close(dec);aacEncClose(&enc);return 0;
}
int main(void){if(run(TT_MP4_RAW,1,0)||run(TT_MP4_LOAS,1,0)||run(TT_MP4_LOAS,2,1))return 1;puts("AAC_BASELINE_COMPLETE configurations=3");return 0;}
