#include "dr_mp3_stems.h"
#include "task_support/codec.h"
#include "task_support/dense_fixture.h"
#include <stdio.h>
#include <math.h>
static unsigned char b[20000];static float r[23040*2],l[23040*2],h[23040*2],a[256],c[256];
int main(void){size_t z=0;for(unsigned i=0;i<20;i++)z+=fixture_frame_dense(b+z,0,2,i%2,0,i/3);drmp3 d;
if(!drmp3_init_memory(&d,b,z,0))return 1;if(drmp3_read_pcm_frames_f32(&d,23040,r)!=23040)return 2;drmp3_uninit(&d);
drmp3_stem_point p[]={{0,8},{5,24},{9,0},{13,32}};drmp3_stems *s=drmp3_stems_open_memory(b,z,p,4);if(!s)return 3;
if(drmp3_stems_channels(s)!=2||drmp3_stems_sample_rate(s)!=44100||drmp3_stems_length(s)!=23040)return 4;
if(drmp3_stems_tell(s)||drmp3_stems_read(s,0,0,0))return 5;
if(drmp3_stems_read(s,23040,l,h)!=23040||drmp3_stems_tell(s)!=23040)return 6;double el=0,eh=0;
for(int i=0;i<23040*2;i++){if(fabs(l[i]+h[i]-r[i])>3e-5)return 7;el+=fabs(l[i]);eh+=fabs(h[i]);}if(el<1e-5||eh<1e-5)return 8;
if(!drmp3_stems_seek(s,3001)||drmp3_stems_tell(s)!=3001)return 9;
if(drmp3_stems_read(s,128,a,c)!=128)return 10;for(int i=0;i<256;i++)if(fabs(a[i]-l[6002+i])>3e-5||fabs(c[i]-h[6002+i])>3e-5)return 11;
if(drmp3_stems_seek(s,23041)||drmp3_stems_tell(s)!=3129)return 12;
if(!drmp3_stems_seek(s,23040)||drmp3_stems_read(s,1,a,c))return 13;drmp3_stems_close(s);
drmp3dec dec;drmp3dec_frame_info info;float lo[2304],hi[2304];unsigned cuts[2]={8,24};drmp3dec_init(&dec);task_spectral_passes=0;
if(drmp3dec_decode_frame_stems(&dec,b,(int)z,lo,hi,&info,cuts)!=1152||info.frame_bytes<=0||task_spectral_passes!=2)return 14;
puts("STEMS_API_NORMAL_OK nonzero_both=1 seek_suffix=1 spectral_passes=2");return 0;}
