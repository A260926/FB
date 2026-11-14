#include <speex/speex.h>
#include "nb_celp.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint64_t hash_pcm(uint64_t h, const float *a) {
  for(int i=0;i<160;i++) {
    uint32_t u; memcpy(&u,a+i,4);
    h=(h^u)*UINT64_C(1099511628211);
  }
  return h;
}
int main(void) {
  void *enc=speex_encoder_init(&speex_nb_mode),*dec=speex_decoder_init(&speex_nb_mode);
  if(!enc||!dec) return 1;
  int quality=5,zero=0;
  speex_encoder_ctl(enc,SPEEX_SET_QUALITY,&quality);
  speex_encoder_ctl(enc,SPEEX_SET_VBR,&zero);
  speex_encoder_ctl(enc,SPEEX_SET_DTX,&zero);
  SpeexBits write,read; speex_bits_init(&write);speex_bits_init(&read);
  float pcm[160]; spx_int16_t input[160]; char packet[256];
  int bytes=0; double normal_energy=0,lost_energy=0;
  uint64_t normal=UINT64_C(1469598103934665603),lost=normal;
  for(int f=0;f<10;f++) {
    for(int i=0;i<160;i++) {
      int t=f*160+i;
      input[i]=(spx_int16_t)(1600*sin(.071*t)+900*cos(.023*t)+350*sin(.137*t));
    }
    speex_bits_reset(&write);
    if(speex_encode_int(enc,input,&write)<0)return 2;
    int n=speex_bits_write(&write,packet,sizeof(packet));if(n<=0||n>256)return 3;
    bytes+=n;speex_bits_read_from(&read,packet,n);
    if(speex_decode(dec,&read,pcm))return 4;
    normal=hash_pcm(normal,pcm);
    for(int i=0;i<160;i++){if(!isfinite(pcm[i]))return 5;normal_energy+=pcm[i]*pcm[i];}
  }
  if(normal_energy<1)return 6;
  printf("normal_decode frames=10 samples=1600 bytes=%d nonzero=1\n",bytes);
  printf("normal_hash=%016llx\n",(unsigned long long)normal);
  for(int f=0;f<12;f++) {
    if(speex_decode(dec,NULL,pcm))return 7;
    lost=hash_pcm(lost,pcm);
    for(int i=0;i<160;i++){if(!isfinite(pcm[i]))return 8;lost_energy+=pcm[i]*pcm[i];}
  }
  DecState *state=(DecState*)dec;
  if(state->count_lost!=12||lost_energy<1)return 9;
  printf("plc_decode frames=12 samples=1920 count_lost=%d finite=1 nonzero=1\n",state->count_lost);
  printf("plc_hash=%016llx seed=%u gain_ring=%.9g,%.9g,%.9g\n",(unsigned long long)lost,state->seed,
   state->pitch_gain_buf[0],state->pitch_gain_buf[1],state->pitch_gain_buf[2]);
  speex_bits_destroy(&read);speex_bits_destroy(&write);
  speex_decoder_destroy(dec);speex_encoder_destroy(enc);
  puts("BASELINE_OK real_NB_encode_decode_and_PLC");
  return 0;
}
