#include "fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#define C(x) do{if(!(x)){fprintf(stderr,"BASELINE_FAIL %d %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){fixture f;OggVorbis_File a,b;int bit,n=0,m=0;float **p;float ref[32768];double energy=0;long k;
 C(fixture_encode(&f,2,48000,16384)==0);C(fixture_open(&f,&a,1)==0);C(ov_seekable(&a));
 while((k=ov_read_float(&a,&p,113,&bit))>0){C(n+k<=16384);for(int i=0;i<k;i++)for(int j=0;j<2;j++)ref[2*(n+i)+j]=p[j][i];n+=k;}
 C(k==0&&n==16384);C(ov_pcm_seek(&a,4097)==0);k=ov_read_float(&a,&p,17,&bit);C(k>0);
 for(int i=0;i<k;i++)for(int j=0;j<2;j++)C(fabsf(p[j][i]-ref[2*(4097+i)+j])<2e-6f);ov_clear(&a);
 C(fixture_open(&f,&b,0)==0);C(!ov_seekable(&b));ogg_int64_t before=ov_pcm_tell(&b);C(ov_pcm_seek(&b,0)==OV_ENOSEEK);C(ov_pcm_tell(&b)==before);
 while((k=ov_read_float(&b,&p,7,&bit))>0){C(m+k<=n);for(int i=0;i<k;i++)for(int j=0;j<2;j++){C(fabsf(p[j][i]-ref[2*(m+i)+j])<2e-6f);energy+=p[j][i]*p[j][i];}m+=k;}
 C(k==0&&m==n&&energy>100);ov_clear(&b);printf("VORBIS_BASELINE_OK frames=%d seekable_suffix=1 nonseekable_refused=1 short_reads=%ld bytes=%zu\n",m,f.reads,f.size);fixture_free(&f);return 0;}
