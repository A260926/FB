#include "layer3.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
static unsigned char outputs[2][4096];static int sizes[2];
static unsigned get(const unsigned char*p,int off,int n){unsigned v=0;while(n--){v=(v<<1)|((p[off/8]>>(7-off%8))&1);off++;}return v;}
static int encode(int planar){
 shine_config_t c={0};c.wave.channels=PCM_STEREO;c.wave.samplerate=48000;shine_set_config_mpeg_defaults(&c.mpeg);c.mpeg.mode=STEREO;
 shine_t s=shine_initialise(&c);if(!s||shine_samples_per_pass(s)!=1152)return 2;
 int16_t pcm[2304],l[1152],r[1152],*split[2]={l,r};int n;unsigned char*p;
 for(int f=0;f<6;f++){
  for(int i=0;i<1152;i++){l[i]=(int16_t)(((i+f*1152)%73-36)*200);r[i]=(int16_t)(((i*3+f*1152)%89-44)*140);pcm[i*2]=l[i];pcm[i*2+1]=r[i];}
  p=planar?shine_encode_buffer(s,split,&n):shine_encode_buffer_interleaved(s,pcm,&n);
  if(n<0||sizes[planar]+n>4096)return 3;memcpy(outputs[planar]+sizes[planar],p,n);sizes[planar]+=n;
 }
 p=shine_flush(s,&n);if(n<0||sizes[planar]+n>4096)return 4;memcpy(outputs[planar]+sizes[planar],p,n);sizes[planar]+=n;shine_close(s);return 0;
}
int main(void){
 if(encode(0)||encode(1))return 2;if(sizes[0]!=2304||sizes[1]!=2304||memcmp(outputs[0],outputs[1],2304))return 3;
 unsigned used=0;uint32_t hash=2166136261u;
 for(int f=0;f<6;f++){const unsigned char*p=outputs[0]+384*f;
  if(p[0]!=255||(p[1]&0xfe)!=0xfa||((p[2]>>2)&3)!=1)return 4;
  for(int g=0;g<4;g++){int pos=32+20+59*g;used+=get(p,pos,12);if(get(p,pos+33,1))return 5;}
 }
 for(int i=0;i<2304;i++)hash=(hash^outputs[0][i])*16777619u;
 if(!used)return 6;printf("SHINE_BASELINE frames=6 bytes=2304 dual_entry=exact switched=0 used_bits=%u hash=%08x\n",used,hash);return 0;
}
