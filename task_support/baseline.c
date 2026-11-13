#include "opus.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
 int e,p,k,j,n,pitch,active=0; double energy=0; unsigned char packet[1500];
 float in[1920],out[1920];
 OpusEncoder *enc=opus_encoder_create(48000,2,OPUS_APPLICATION_RESTRICTED_LOWDELAY,&e);
 if(!enc||e) return 1;
 OpusDecoder *dec=opus_decoder_create(48000,2,&e); if(!dec||e)return 2;
 if(opus_encoder_ctl(enc,OPUS_SET_BITRATE(96000))||opus_encoder_ctl(enc,OPUS_SET_COMPLEXITY(10)))return 3;
 for(p=0;p<18;p++) {
  for(j=0;j<960;j++)for(k=0;k<2;k++) {
   double t=(p*960+j)/48000.;
   in[j*2+k]=(float)(.12*sin(6.28318530718*(p<8?200:320)*t+.3*k)+.03*sin(6.28318530718*600*t));
  }
  n=opus_encode_float(enc,in,960,packet,sizeof(packet));if(n<=0)return 4;
  n=opus_decode_float(dec,packet,n,out,960,0);if(n!=960)return 5;
  if(opus_decoder_ctl(dec,OPUS_GET_PITCH(&pitch)))return 6;
  active+=pitch>0;
  for(j=0;j<1920;j++){if(!isfinite(out[j]))return 7;energy+=out[j]*out[j];}
 }
 if(energy<1||active==0)return 8;
 printf("OPUS_BASELINE_PCM packets=18 frames=17280 pitch_frames=%d energy=%.6f\n",active,energy);
 opus_encoder_destroy(enc);opus_decoder_destroy(dec);return 0;
}
