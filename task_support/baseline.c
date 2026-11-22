#include "dr_mp3.h"
#include "task_support/frame_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static unsigned char bytes[20000];static float pcm[64*1152*2];
int main(void){size_t size=0;drmp3 d;for(unsigned i=0;i<20;i++)size+=fixture_frame(bytes+size,0,2,i%2,0,i/3);
if(!drmp3_init_memory(&d,bytes,size,NULL))return 1;
drmp3_uint64 n=drmp3_read_pcm_frames_f32(&d,64*1152,pcm);double energy=0;for(size_t i=0;i<n*2;i++)energy+=fabs(pcm[i]);
if(n!=20*1152||energy<1)return 2;
printf("NATIVE_MP3 frames=%llu rate=%u channels=%u nonzero=1\n",(unsigned long long)n,d.sampleRate,d.channels);
if(!drmp3_seek_to_pcm_frame(&d,37))return 3;float tail[128];if(drmp3_read_pcm_frames_f32(&d,64,tail)!=64)return 4;
for(int i=0;i<128;i++)if(fabs(tail[i]-pcm[37*2+i])>1e-6)return 5;
printf("NATIVE_SEEK target=37 suffix=64 match=1\n");drmp3_uninit(&d);return 0;}
