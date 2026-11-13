#include "twolame.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define N (1152*5+173)
static short l[N],r[N],inter[N*2];
static twolame_options *init(int in,int out) {
 twolame_options *o=twolame_init(); if(!o) exit(2);
 twolame_set_num_channels(o,2);twolame_set_in_samplerate(o,in);
 twolame_set_out_samplerate(o,out);twolame_set_bitrate(o,192);
 twolame_set_mode(o,TWOLAME_STEREO);twolame_set_verbosity(o,0);
 return o;
}
static int encode(unsigned char *out,int chunks) {
 twolame_options *o=init(48000,48000);int pos=0,n=0,k,ret;
 if(twolame_init_params(o)) exit(3);
 while(pos<N){ k=chunks?(pos%29+1):N; if(k>N-pos)k=N-pos;
 ret=chunks?twolame_encode_buffer(o,l+pos,r+pos,k,out+n,32768-n):twolame_encode_buffer_interleaved(o,inter+2*pos,k,out+n,32768-n);
 if(ret<0)exit(4);n+=ret;pos+=k; }
 ret=twolame_encode_flush(o,out+n,32768-n);if(ret<0)exit(5);n+=ret;
 if(twolame_encode_flush(o,out+n,32768-n)!=0)exit(6);
 twolame_close(&o);return n;
}
int main(void){unsigned char a[32768],b[32768];int i,n,m,off=0,frames=0;uint32_t h=2166136261u;
 for(i=0;i<N;i++){l[i]=(short)((i*97u)%24001-12000);r[i]=(short)(l[i]/-2);inter[2*i]=l[i];inter[2*i+1]=r[i];}
 n=encode(a,0);m=encode(b,1);if(n!=m||memcmp(a,b,n))return 7;
 while(off<n){unsigned x=((unsigned)a[off]<<24)|((unsigned)a[off+1]<<16)|((unsigned)a[off+2]<<8)|a[off+3];
 if((x&0xfffe0000u)!=0xfffc0000u||((x>>10)&3)!=1||((x>>12)&15)!=10)return 8;
 off+=144000*192/48000+((x>>9)&1);frames++;}
 if(off!=n||frames!=6)return 9;
 for(i=0;i<n;i++)h=(h^a[i])*16777619u;
 printf("BASELINE_OK frames=%d bytes=%d fnv=%08x planar_interleaved_equal=1\n",frames,n,h);
 twolame_options *o=init(44100,48000);int rc=twolame_init_params(o);twolame_close(&o);
 if(rc>=0)return 10;puts("GAP_CONFIRMED unequal_rates_rejected=1");return 0;}
