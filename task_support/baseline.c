#include <speex/speex.h>
#include <speex/speex_bits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
int main(void){
  for(int m=0;m<3;m++){
    const SpeexMode *mode=speex_lib_get_mode(m);
    void *e=speex_encoder_init(mode),*d=speex_decoder_init(mode);
    SpeexBits b;spx_int16_t in[640],out[640];int n=0,q=5,v=0;uint32_t h=2166136261u;unsigned nonzero=0;
    if(!e||!d)return 2;
    speex_encoder_ctl(e,SPEEX_GET_FRAME_SIZE,&n);
    speex_encoder_ctl(e,SPEEX_SET_QUALITY,&q);speex_encoder_ctl(e,SPEEX_SET_VBR,&v);
    speex_bits_init(&b);
    for(int f=0;f<6;f++){
      for(int i=0;i<n;i++)in[i]=(spx_int16_t)(((i+f*n)*79%7001)-3500);
      if(speex_encode_int(e,in,&b)<0)return 3;
    }
    int bits=b.nbBits;speex_bits_rewind(&b);
    for(int f=0;f<6;f++){
      if(speex_decode_int(d,&b,out)!=0)return 4;
      for(int i=0;i<n;i++){nonzero+=out[i]!=0;h=(h^(uint16_t)out[i])*16777619u;}
    }
    if(!nonzero||bits<30)return 5;
    printf("SPEEX_BASELINE mode=%d frames=6 samples=%d bits=%d nonzero=%u hash=%08x\n",m,6*n,bits,nonzero,h);
    speex_bits_destroy(&b);speex_encoder_destroy(e);speex_decoder_destroy(d);
  }
  puts("SPEEX_BASELINE_OK");return 0;
}
