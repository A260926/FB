#include <stddef.h>
#include <stdio.h>
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t bytes[512]; static unsigned pos;
static void bits(unsigned v,unsigned n){for(unsigned i=n;i;i--){bytes[pos/8]|=((v>>(i-1))&1)<<(7-pos%8);pos++;}}
static void start(unsigned c){while(pos%8)bits(0,1);bits(1,24);bits(c,8);}
int main(void){
 start(0xb3);bits(16,12);bits(16,12);bits(1,4);bits(3,4);
 bits(1,18);bits(1,1);bits(20,10);bits(0,1);bits(0,1);bits(0,1);
 start(0);bits(0,10);bits(1,3);bits(65535,16);bits(0,1);
 start(1);bits(8,5);bits(0,1);bits(1,1);bits(1,1);
 for(int block=0;block<6;block++){bits(block<4?4:0,block<4?3:2);bits(2,2);}
 start(0); /* terminates the complete first picture, without decoding another */
 plm_buffer_t *b=plm_buffer_create_with_memory(bytes,sizeof(bytes),0);
 plm_video_t *v=plm_video_create_with_buffer(b,1);assert(plm_video_has_header(v));
 assert(plm_video_get_width(v)==16 && plm_video_get_height(v)==16);
 plm_video_set_no_delay(v,1);plm_frame_t *f=plm_video_decode(v);assert(f);
 for(unsigned i=0;i<f->y.width*f->y.height;i++)assert(f->y.data[i]==128);
 for(unsigned i=0;i<f->cb.width*f->cb.height;i++)assert(f->cb.data[i]==128 && f->cr.data[i]==128);
 printf("PLM_INTRA_BASELINE_OK size=%ux%u luma=128 chroma=128\n",f->width,f->height);
 plm_video_destroy(v);
}
