#include <speex/speex.h>
#include <speex/speex_bits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void run(int mode,int cap) {
 const SpeexMode *m=speex_lib_get_mode(mode);void *e=speex_encoder_init(m),*d=speex_decoder_init(m);
 SpeexBits b,read;short in[640],out[640];int fs,on=1,q=9,min=10000,max=0,first=0,f,i;uint32_t seed=7;
 float quality=9; if(!e||!d)exit(2);speex_bits_init(&b);speex_bits_init(&read);
 speex_encoder_ctl(e,SPEEX_GET_FRAME_SIZE,&fs);speex_encoder_ctl(e,SPEEX_SET_QUALITY,&q);
 speex_encoder_ctl(e,SPEEX_SET_VBR,&on);speex_encoder_ctl(e,SPEEX_SET_VBR_QUALITY,&quality);
 if(cap)speex_encoder_ctl(e,SPEEX_SET_VBR_MAX_BITRATE,&cap);
 for(f=0;f<16;f++){
  for(i=0;i<fs;i++){seed=1664525u*seed+1013904223u;in[i]=(short)((int)(seed>>17)-16384);if(f<4)in[i]/=64;}
  speex_bits_reset(&b);speex_bits_pack(&b,5,3);int before=b.nbBits;
  if(speex_encode_int(e,in,&b)<0)exit(3);int total=b.nbBits-before;
  if(total<min)min=total;if(total>max)max=total;
  if(!f)first=total;
  char data[512];int len=speex_bits_write(&b,data,sizeof(data));if(len<=0)exit(4);
  speex_bits_read_from(&read,data,len);speex_bits_advance(&read,3);
  if(speex_decode_int(d,&read,out)!=0)exit(5);
 }
 printf("BASELINE_OK mode=%s cap_bps=%d frames=16 first_bits=%d min_bits=%d max_bits=%d decode=1 prefix_bits=3\n",mode==1?"WB":"UWB",cap,first,min,max);
 speex_bits_destroy(&b);speex_bits_destroy(&read);speex_encoder_destroy(e);speex_decoder_destroy(d);
}
int main(void){run(1,0);run(2,0);run(1,20600);run(2,32000);return 0;}
