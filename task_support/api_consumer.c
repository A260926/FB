#include "fixture.h"
#include <stdio.h>
#include <math.h>
#define C(x) do{if(!(x)){fprintf(stderr,"REWIND_API_FAIL %d %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){fixture f;OggVorbis_File v;float **p,ref[4096];int bit,n=0;long k;ogg_int64_t lo,hi;
 C(fixture_encode(&f,2,48000,8192)==0);C(fixture_open(&f,&v,0)==0);C(ov_rewind_enable(&v,65536)==0);
 while(n<2048){k=ov_read_float(&v,&p,2048-n,&bit);C(k>0);for(int i=0;i<k;i++)for(int j=0;j<2;j++)ref[2*(n+i)+j]=p[j][i];n+=k;}
 C(ov_rewind_range(&v,&lo,&hi)==0);C(lo<=1024&&hi>=2048);long reads=f.reads;
 C(ov_pcm_rewind(&v,1024)==0);C(f.reads==reads);C(ov_pcm_tell(&v)==1024);
 k=ov_read_float(&v,&p,17,&bit);C(k>0&&f.reads==reads);
 for(int i=0;i<k;i++)for(int j=0;j<2;j++)C(fabsf(p[j][i]-ref[2*(1024+i)+j])<2e-6f);
 C(ov_pcm_rewind(&v,hi+1)==OV_EINVAL);ov_clear(&v);fixture_free(&f);puts("VORBIS_REWIND_API_OK");return 0;}
