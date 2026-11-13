/* Original API witness only. Does not assemble a repaired DAB stream. */
#include "twolame.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define S 1152
#define CAP 16384
static short pcm[3*S*2];
static void need(int ok,int line){if(!ok){fprintf(stderr,"WITNESS_FAILED line=%d\n",line);exit(2);}}
#define CHECK(x) need((x),__LINE__)
static twolame_options *open_encoder(int rate,int bitrate,int channels,int crc){
 twolame_options *o=twolame_init();CHECK(o!=NULL);
 CHECK(!twolame_set_num_channels(o,channels));
 CHECK(!twolame_set_in_samplerate(o,rate));CHECK(!twolame_set_out_samplerate(o,rate));
 CHECK(!twolame_set_bitrate(o,bitrate));CHECK(!twolame_set_mode(o,channels==1?TWOLAME_MONO:TWOLAME_STEREO));
 CHECK(!twolame_set_verbosity(o,0));CHECK(!twolame_set_padding(o,TWOLAME_PAD_ALL));
 CHECK(!twolame_set_DAB(o,1));CHECK(!twolame_set_DAB_crc_length(o,crc));
 CHECK(!twolame_set_num_ancillary_bits(o,8*(crc+2)));
 CHECK(!twolame_init_params(o));return o;
}
static void snapshot(twolame_options *o,unsigned char *crc,int len){
 unsigned char probe[16];memset(probe,0xa5,sizeof probe);
 CHECK(!twolame_set_DAB_scf_crc(o,probe,sizeof probe));
 CHECK(probe[14]==0xa5&&probe[15]==0xa5);
 memcpy(crc,probe+14-len,(size_t)len);
}
static void run(int rate,int bitrate,int channels,int crc){
 twolame_options *a=open_encoder(rate,bitrate,channels,crc),*b=open_encoder(rate,bitrate,channels,crc);
 unsigned char one[3][CAP],bulk[CAP],state[3][4],last[4];
 int n[3],total=0,i,j,ret,changed=0;
 uint32_t rng=0x12345678u;
 for(i=0;i<3*S*channels;i++){rng=rng*1664525u+1013904223u;pcm[i]=(short)((int)(rng>>18)-8192);}
 for(i=0;i<3;i++){
  n[i]=twolame_encode_buffer_interleaved(a,pcm+i*S*channels,S,one[i],CAP);CHECK(n[i]>0);
  snapshot(a,state[i],crc);
  for(j=n[i]-crc-2;j<n[i];j++)CHECK(one[i][j]==0);
  total+=n[i];
 }
 ret=twolame_encode_buffer_interleaved(b,pcm,3*S,bulk,CAP);CHECK(ret==total);
 snapshot(b,last,crc);CHECK(!memcmp(last,state[2],(size_t)crc));
 CHECK(memcmp(state[1],state[2],(size_t)crc)!=0);
 total=0;
 for(i=0;i<3;i++){CHECK(!memcmp(one[i],bulk+total,(size_t)n[i]));total+=n[i];}
 /* Apply existing helper once to an isolated copy of frame 0, using state after frame 2.
    This is deliberately the wrong successor. The witness checks that distinction only. */
 CHECK(!twolame_set_DAB_scf_crc(b,one[0],n[0]));
 CHECK(!memcmp(one[0]+n[0]-crc-2,state[2],(size_t)crc));
 CHECK(memcmp(one[0]+n[0]-crc-2,state[1],(size_t)crc)!=0);
 for(j=0;j<crc;j++)changed|=state[2][j];CHECK(changed);
 CHECK(twolame_encode_flush(a,one[0],CAP)==0);CHECK(twolame_encode_flush(b,bulk,CAP)==0);
 printf("DAB_GAP rate=%d bitrate=%d channels=%d crc=%d frame_bytes=%d,%d,%d raw_ancillary_zero=1 bulk_equals_raw_single=1 overwritten_successor_crc=1 flush_without_partial=0\n",rate,bitrate,channels,crc,n[0],n[1],n[2]);
 printf("DAB_CRC snapshots=");for(i=0;i<3;i++){if(i)putchar('/');for(j=0;j<crc;j++)printf("%02x",state[i][j]);}putchar('\n');
 twolame_close(&a);twolame_close(&b);
}
int main(void){run(48000,96,2,2);run(44100,192,2,4);run(24000,96,1,4);puts("DAB_GAP_OK cases=3 original_api_only=1");return 0;}
