#include "aacenc_lib.h"
#include "aacdecoder_lib.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define REQUIRE(x) do {if(!(x)){std::fprintf(stderr,"baseline failed line %d: %s\n",__LINE__,#x);return 1;}}while(0)
static int run(int protection) {
 HANDLE_AACENCODER e=nullptr; REQUIRE(aacEncOpen(&e,0,2)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(e,AACENC_AOT,2)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(e,AACENC_SAMPLERATE,48000)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(e,AACENC_CHANNELMODE,MODE_2)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(e,AACENC_CHANNELORDER,1)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(e,AACENC_BITRATE,128000)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(e,AACENC_TRANSMUX,TT_MP4_ADTS)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(e,AACENC_TPSUBFRAMES,4)==AACENC_OK);
 REQUIRE(aacEncoder_SetParam(e,AACENC_PROTECTION,protection)==AACENC_OK);
 REQUIRE(aacEncEncode(e,nullptr,nullptr,nullptr,nullptr)==AACENC_OK);
 AACENC_InfoStruct info={};REQUIRE(aacEncInfo(e,&info)==AACENC_OK && info.frameLength==1024);
 HANDLE_AACDECODER d=aacDecoder_Open(TT_MP4_ADTS,1);REQUIRE(d);
 INT_PCM pcm[2048],decoded[2048];UCHAR packet[8192];uint32_t rng=12345;
 int packets=0,frames=0;uint64_t total=0,energy=0,hash=0;int quiet=0;
 for(int n=0;n<18;n++) {
  for(int i=0;i<1024;i++) {rng=rng*1664525u+1013904223u;int v=((n*1024+i)%193-96)*220;pcm[2*i]=static_cast<INT_PCM>(v+(int)(rng>>27)-16);pcm[2*i+1]=static_cast<INT_PCM>(-v/2);}
  void *ip=pcm,*op=packet;INT iid=IN_AUDIO_DATA,oid=OUT_BITSTREAM_DATA,is=sizeof(pcm),os=sizeof(packet),ie=sizeof(INT_PCM),oe=1;
  AACENC_BufDesc in={1,&ip,&iid,&is,&ie},out={1,&op,&oid,&os,&oe};AACENC_InArgs ia={};AACENC_OutArgs oa={};ia.numInSamples=2048;
  REQUIRE(aacEncEncode(e,&in,&out,&ia,&oa)==AACENC_OK && oa.numInSamples==2048);
  if(!oa.numOutBytes) {quiet++;continue;}
  int len=oa.numOutBytes;REQUIRE(len>=7 && packet[0]==0xff && (packet[1]&0xf6)==0xf0);
  int header_len=((packet[3]&3)<<11)|(packet[4]<<3)|(packet[5]>>5);REQUIRE(header_len==len && (packet[6]&3)==3 && (packet[1]&1)==!protection);
  packets++;total+=len;for(int i=0;i<len;i++)hash=hash*33+packet[i];
  UCHAR *p=packet;UINT size=len,valid=len;REQUIRE(aacDecoder_Fill(d,&p,&size,&valid)==AAC_DEC_OK && valid==0);
  for(;frames<12;) {
   AAC_DECODER_ERROR err=aacDecoder_DecodeFrame(d,decoded,2048,0);if(err==AAC_DEC_NOT_ENOUGH_BITS)break;
   REQUIRE(err==AAC_DEC_OK);CStreamInfo *si=aacDecoder_GetStreamInfo(d);REQUIRE(si && si->frameSize==1024 && si->numChannels==2);
   for(int i=0;i<2048;i++)energy+=decoded[i]<0?-(int64_t)decoded[i]:decoded[i];frames++;
  }
 }
 std::fprintf(stderr,"OBS protection=%d packets=%d frames=%d quiet=%d energy=%llu bytes=%llu\n",protection,packets,frames,quiet,(unsigned long long)energy,(unsigned long long)total); REQUIRE(packets==4 && frames==12 && quiet==14 && energy>100000);
 std::printf("BASELINE protection=%d encoded=18 packets=%d decoded_prefix=%d pending_AUs=2 bytes=%llu energy=%llu hash=%016llx\n",protection,packets,frames,(unsigned long long)total,(unsigned long long)energy,(unsigned long long)hash);
 aacDecoder_Close(d);REQUIRE(aacEncClose(&e)==AACENC_OK);return 0;
}
int main(){if(run(0)||run(1))return 1;return 0;}
