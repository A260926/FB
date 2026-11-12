#include "state.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
  oc_theora_state s;
  oc_fragment f[4];
  ptrdiff_t off[4]={0,8,256,264};
  unsigned char pixels[32*32];
  signed char bv[256];
  int x,y;
  memset(&s,0,sizeof s);memset(f,0,sizeof f);
  for(y=0;y<32;y++) for(x=0;x<32;x++) pixels[y*32+x]=x<8?100:104;
  s.frags=f;s.frag_buf_offs=off;s.ref_frame_data[0]=pixels;
  s.ref_ystride[0]=32;s.fplanes[0].nhfrags=2;
  s.fplanes[0].nvfrags=2;s.fplanes[0].nfrags=4;
  f[0].coded=1;
  oc_loop_filter_init_c(bv,10);
  oc_state_loop_filter_frag_rows_c(&s,bv,0,0,0,2);
  printf("THEORA_FILTER_EDGE left=%u right=%u\n",pixels[7],pixels[8]);
  fflush(stdout);
  assert(pixels[7]==101 && pixels[8]==103);
  assert(pixels[7+16*32]==100 && pixels[8+16*32]==104);
  return 0;
}
