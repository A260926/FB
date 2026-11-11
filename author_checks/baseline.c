#include "xmp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(void) {
 xmp_context a=xmp_create_context(),b=xmp_create_context();
 assert(a && b); assert(xmp_load_module(a,"author_checks/tone.mod")==0);
 assert(xmp_load_module(b,"author_checks/tone.mod")==0);
 assert(xmp_start_player(a,48000,0)==0 && xmp_start_player(b,48000,0)==0);
 unsigned char first[4096],via_buffer[4096]; unsigned copied=0; long long energy=0;
 for(int i=0;i<24;i++) {
  assert(xmp_play_frame(a)==0); struct xmp_frame_info f; xmp_get_frame_info(a,&f);
  assert(f.buffer_size>0 && f.buffer);
  short *v=f.buffer; for(int j=0;j<f.buffer_size/2;j++) energy+=abs(v[j]);
  if(copied<sizeof(first)) {
   unsigned n=f.buffer_size; if(n>sizeof(first)-copied) n=sizeof(first)-copied;
   memcpy(first+copied,f.buffer,n); copied+=n;
  }
 }
 assert(energy>100000 && copied==sizeof(first));
 assert(xmp_play_buffer(b,via_buffer,sizeof(via_buffer),0)==0);
 assert(memcmp(first,via_buffer,sizeof(first))==0);
 printf("TRACKER_RENDER_OK ticks=24 channels=4 energy=%lld\n",energy);
 puts("TRACKER_FRAME_BUFFER_PREFIX_OK bytes=4096");
 xmp_end_player(a); xmp_end_player(b); xmp_release_module(a); xmp_release_module(b);
 xmp_free_context(a); xmp_free_context(b);
}
