#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "layer3.h"
int main(void){
 shine_config_t c={0};c.wave.channels=PCM_STEREO;c.wave.samplerate=48000;
 shine_set_config_mpeg_defaults(&c.mpeg);c.mpeg.mode=STEREO;
 shine_t a=shine_initialise(&c),b=shine_initialise(&c);assert(a&&b);
 assert(shine_samples_per_pass(a)==1152);
 int16_t l[1152],r[1152],inter[2304];int16_t *planes[]={l,r};
 size_t total=0;
 for(int f=0;f<4;f++){
  for(int i=0;i<1152;i++){l[i]=(int16_t)(((i+f*7)%31-15)*57);r[i]=(int16_t)(((i+f*11)%41-20)*43);inter[2*i]=l[i];inter[2*i+1]=r[i];}
  int na,nb;unsigned char *pa=shine_encode_buffer(a,planes,&na),*pb=shine_encode_buffer_interleaved(b,inter,&nb);
  assert(na==384&&nb==na&&!memcmp(pa,pb,na));
  assert(pa[0]==255&&(pa[1]&0xfe)==0xfa&&(pa[1]&1)==1);
  total+=na;
 }
 int na,nb;shine_flush(a,&na);shine_flush(b,&nb);assert(na==0&&nb==0);
 assert(total==1536);shine_close(a);shine_close(b);
 puts("seed Shine MP3: 4 stereo frames planar=interleaved 1536 bytes ok");return 0;
}
