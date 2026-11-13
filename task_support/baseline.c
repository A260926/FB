#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SIMD
#include "dr_flac.h"
#include <stdio.h>
#include <string.h>
static unsigned char data[4096];static unsigned pos;static unsigned offsets[4];
static void put(unsigned long long v,unsigned n){while(n--){data[pos>>3]|=((v>>n)&1)<<(7-(pos&7));pos++;}}
static unsigned crc(const unsigned char *p,unsigned n,unsigned width,unsigned poly){unsigned c=0,top=1u<<(width-1),mask=(1u<<width)-1;for(unsigned i=0;i<n;i++){c^=(unsigned)p[i]<<(width-8);for(int j=0;j<8;j++)c=((c<<1)^((c&top)?poly:0))&mask;}return c;}
static int sample(int f,int c){return (c? -900:600)+(c?-7:11)*f;}
static unsigned make(void){
 memset(data,0,sizeof(data));pos=0;put(0x664c6143,32);put(0x80,8);put(34,24);
 put(16,16);put(16,16);put(0,24);put(0,24);put(44100,20);put(1,3);put(15,5);put(48,36);put(0,64);put(0,64);
 for(int k=0;k<3;k++){
  unsigned begin=pos/8;offsets[k]=begin;
  put(0xfff9,16);put(0x69,8);put(0x18,8);put(k*16,8);put(15,8);
  unsigned v=crc(data+begin,pos/8-begin,8,7);put(v,8);
  for(int c=0;c<2;c++){put(2,8);for(int j=0;j<16;j++)put((unsigned short)sample(k*16+j,c),16);}
  v=crc(data+begin,pos/8-begin,16,0x8005);put(v,16);
 }
 offsets[3]=pos/8;return pos/8;
}
int main(void){
 unsigned size=make();drflac_int32 out[96];drflac_int16 narrow[2];float floating[2];
 drflac *d=drflac_open_memory(data,size,NULL);if(!d)return 1;
 drflac_uint64 n=drflac_read_pcm_frames_s32(d,48,out);if(n!=48)return 2;
 for(int f=0;f<48;f++)for(int c=0;c<2;c++)if(out[2*f+c]!=sample(f,c)*65536)return 3;
 if(!drflac_seek_to_pcm_frame(d,17)||drflac_read_pcm_frames_s16(d,1,narrow)!=1)return 4;
 if(narrow[0]!=sample(17,0)||narrow[1]!=sample(17,1))return 5;
 if(!drflac_seek_to_pcm_frame(d,9)||drflac_read_pcm_frames_f32(d,1,floating)!=1)return 6;
 drflac_close(d);data[offsets[2]-1]^=1; /* only middle frame CRC */
 d=drflac_open_memory(data,size,NULL);if(!d)return 7;
 n=drflac_read_pcm_frames_s32(d,48,out);if(n!=32)return 8;
 for(int f=0;f<32;f++)for(int c=0;c<2;c++){int src=f<16?f:f+16;if(out[2*f+c]!=sample(src,c)*65536)return 9;}
 printf("DRFLAC_BASELINE clean=48 crc_skipped=%llu middle_gap=16 seek_s16=17 seek_f32=9\n",(unsigned long long)n);
 drflac_close(d);return 0;
}
