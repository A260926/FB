#include "layer3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char stream[3][65536];static size_t lengths[3];
static unsigned get(const unsigned char *p,unsigned *bit,unsigned n){unsigned v=0;while(n--){v=(v<<1)|((p[*bit>>3]>>(7-(*bit&7)))&1);(*bit)++;}return v;}
int main(void){
 int16_t inter[2304],left[1152],right[1152];int16_t *planes[]={left,right};
 for(int mode=0;mode<3;mode++){
  shine_config_t cfg;memset(&cfg,0,sizeof(cfg));shine_set_config_mpeg_defaults(&cfg.mpeg);
  cfg.wave.channels=PCM_STEREO;cfg.wave.samplerate=48000;cfg.mpeg.mode=mode==2?JOINT_STEREO:STEREO;cfg.mpeg.bitr=128;
  shine_t enc=shine_initialise(&cfg);if(!enc||shine_samples_per_pass(enc)!=1152)return 1;
  for(int p=0;p<6;p++){
   for(int i=0;i<1152;i++){int t=p*1152+i;left[i]=(int16_t)(((t*37)%3000)-1500);right[i]=(int16_t)(left[i]/2+((t*13)%500)-250);inter[2*i]=left[i];inter[2*i+1]=right[i];}
   int n;unsigned char *v=mode==1?shine_encode_buffer(enc,planes,&n):shine_encode_buffer_interleaved(enc,inter,&n);
   if(n<0||lengths[mode]+n>65536)return 2;
   memcpy(stream[mode]+lengths[mode],v,n);lengths[mode]+=n;
  }
  int n;unsigned char *v=shine_flush(enc,&n);memcpy(stream[mode]+lengths[mode],v,n);lengths[mode]+=n;shine_close(enc);
 }
 if(lengths[0]!=2304||lengths[1]!=2304||lengths[2]!=2304||memcmp(stream[0],stream[1],2304))return 3;
 for(int f=0;f<6;f++){
  unsigned p=f*384;if(stream[0][p]!=255||(stream[0][p+1]&0xfe)!=0xfa)return 4;
  if(((stream[2][p+3]>>6)&3)!=1||((stream[2][p+3]>>4)&3)!=0)return 5;
  for(int j=0;j<384;j++)if(j!=3&&stream[0][p+j]!=stream[2][p+j])return 6;
  if((stream[0][p+3]^stream[2][p+3])!=0x40)return 7;
 }
 for(int f=0;f<6;f++){const unsigned char *si=stream[0]+f*384+4;unsigned bit=20;for(int g=0;g<4;g++){get(si,&bit,12);get(si,&bit,9);get(si,&bit,8);if(get(si,&bit,4)!=0)return 8;if(get(si,&bit,1)!=0)return 9;get(si,&bit,25);}}
 printf("SHINE_BASELINE frames=6 bytes=2304 planar_equal=1 scalefac_compress_zero=24\n");return 0;
}
