#include "opus.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
int main(int argc,char**argv){
 OpusEncoder* en;OpusDecoder* de;OpusDecoder* unsupported;int err=0,i,p,bytes,ret;unsigned char packet[4000];
 float input[960*2],output[960*2];double energy=0;opus_int32 duration=0;
 en=opus_encoder_create(48000,2,OPUS_APPLICATION_RESTRICTED_LOWDELAY,&err);if(!en||err)return 1;
 de=opus_decoder_create(48000,2,&err);if(!de||err)return 2;
 for(p=0;p<8;p++){
 for(i=0;i<960;i++){input[i*2]=(float)(.2*sin((p*960+i)*.043));input[i*2+1]=(float)(.16*cos((p*960+i)*.027));}
 bytes=opus_encode_float(en,input,960,packet,sizeof(packet));if(bytes<=0)return 3;
 if(opus_decoder_get_nb_samples(de,packet,bytes)!=960)return 4;
 ret=opus_decode_float(de,packet,bytes,output,960,0);if(ret!=960)return 5;
 for(i=0;i<1920;i++){if(!isfinite(output[i]))return 6;energy+=output[i]*output[i];}
 }
 if(energy<1)return 7;
 ret=opus_decode_float(de,NULL,0,output,960,0);if(ret!=960)return 8;
 if(opus_decoder_ctl(de,OPUS_GET_LAST_PACKET_DURATION(&duration))!=OPUS_OK||duration!=960)return 9;
 if(opus_decoder_ctl(de,OPUS_RESET_STATE)!=OPUS_OK)return 10;
 printf("OPUS48_NORMAL_OK packets=8 frames=7680 plc=960 energy=%.6f\n",energy);
 if(argc>1&&strcmp(argv[1],"--gap")==0){
 unsupported=opus_decoder_create(44100,2,&err);
 if(unsupported||err!=OPUS_BAD_ARG)return 11;
 puts("OPUS44100_STARTING_GAP create_rejected=OPUS_BAD_ARG");
 }
 opus_decoder_destroy(de);opus_encoder_destroy(en);return 0;
}
