#include "opus.h"
#include "opus_multistream.h"
#include "src/opus_resampler_44100.h"
#include <stdio.h>
#include <math.h>
#define NEED(x,msg) do{if(!(x)){fprintf(stderr,"API_CONSUMER_FAIL %s\n",msg);return 29;}}while(0)
int main(void){
 int err=0,i,p,nb,n;unsigned char packet[2000];float input[240],out[300];unsigned long total=0;double energy=0;
 opus_int32 rate=0,duration=0;OpusEncoder *en;OpusDecoder *de;OpusMSDecoder *ms;unsigned char map[2]={0,1};float silence[882*2];
 en=opus_encoder_create(48000,2,OPUS_APPLICATION_RESTRICTED_LOWDELAY,&err);NEED(en&&err==OPUS_OK,"ENCODER48_CREATE");
 de=opus_decoder_create(44100,2,&err);NEED(de&&err==OPUS_OK,"CODEC44100_CREATE");
 NEED(opus_decoder_ctl(de,OPUS_GET_SAMPLE_RATE(&rate))==OPUS_OK&&rate==44100,"SAMPLE_RATE");
 for(p=0;p<8;p++){
 for(i=0;i<120;i++){input[2*i]=(float)(.2*sin((120*p+i)*.08));input[2*i+1]=(float)(.13*cos((120*p+i)*.11));}
 nb=opus_encode_float(en,input,120,packet,sizeof(packet));NEED(nb>0,"ENCODE_SHORT");
 n=opus_decoder_get_nb_samples(de,packet,nb);NEED(n==(int)(((p+1)*120*147)/160-(p*120*147)/160),"PREDICT_REMAINDER");
 NEED(opus_decode_float(de,packet,nb,out,150,0)==n,"DECODE_COUNT");total+=(unsigned)n;
 for(i=0;i<2*n;i++){NEED(isfinite(out[i]),"FINITE_PCM");energy+=out[i]*out[i];}
 NEED(opus_decoder_ctl(de,OPUS_GET_LAST_PACKET_DURATION(&duration))==OPUS_OK&&duration==n,"LAST_DURATION");
 }
 NEED(total==882&&energy>.1,"SMALL_PACKET_PCM");
 ms=opus_multistream_decoder_create(44100,2,1,1,map,&err);NEED(ms&&err==OPUS_OK,"MS44100_CREATE");
 NEED(opus_multistream_decode_float(ms,NULL,0,silence,882,0)==882,"MS_PLC_COUNT");
 for(i=0;i<1764;i++)NEED(isfinite(silence[i])&&fabs(silence[i])<1e-9,"MS_INITIAL_PLC_PCM");
 opus_multistream_decoder_destroy(ms);opus_decoder_destroy(de);opus_encoder_destroy(en);
 puts("OPUS_44100_API_CONSUMER_OK");return 0;
}
