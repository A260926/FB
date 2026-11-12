/* Local deterministic caller of the unchanged public API. */
#include "ebur128.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"baseline assertion line %d\n",__LINE__); return 1; } } while(0)
int main(void) {
  const size_t rate=48000, hop=4800, n=rate*6;
  double *pcm=malloc(n*sizeof(*pcm));
  ebur128_state *whole=ebur128_init(1,rate,EBUR128_MODE_I|EBUR128_MODE_LRA);
  ebur128_state *split[15]={0}, *tiny[60]={0};
  double a,b,c,lra,fragmented;
  ebur128_state *parts=ebur128_init(1,rate,EBUR128_MODE_I|EBUR128_MODE_LRA);
  REQUIRE(pcm && whole && parts);
  for(size_t i=0;i<n;i++) pcm[i]=0.2+0.01*sin(6.283185307179586*997.0*(double)i/rate); /* DC onset plus a sustained quiet tone. */
  REQUIRE(!ebur128_add_frames_double(whole,pcm,n));
  for(size_t i=0;i<n;) { size_t k=137; if(k>n-i)k=n-i; REQUIRE(!ebur128_add_frames_double(parts,pcm+i,k));i+=k; }
  REQUIRE(!ebur128_loudness_global(whole,&a));
  REQUIRE(!ebur128_loudness_global(parts,&fragmented));
  REQUIRE(isfinite(a) && fabs(a-fragmented)<1e-9);
  for(size_t k=0;k<15;k++) { split[k]=ebur128_init(1,rate,EBUR128_MODE_I); REQUIRE(split[k]); REQUIRE(!ebur128_add_frames_double(split[k],pcm+k*4*hop,4*hop)); }
  for(size_t k=0;k<60;k++) { tiny[k]=ebur128_init(1,rate,EBUR128_MODE_I); REQUIRE(tiny[k]); REQUIRE(!ebur128_add_frames_double(tiny[k],pcm+k*hop,hop)); }
  REQUIRE(!ebur128_loudness_global_multiple(split,15,&b));
  REQUIRE(!ebur128_loudness_global_multiple(tiny,60,&c));
  REQUIRE(!ebur128_loudness_range(whole,&lra));
  printf("OLD_STREAM_OK whole=%.9f fragmented=%.9f lra=%.9f\n",a,fragmented,lra);
  printf("INDEPENDENT_SEGMENTS whole=%.9f separate400ms=%.9f delta=%.9f separate100ms=%g\n",a,b,b-a,c);
  REQUIRE(isfinite(b) && fabs(b-a)>0.01 && isinf(c) && c<0);
  for(size_t k=0;k<15;k++) ebur128_destroy(&split[k]);
  for(size_t k=0;k<60;k++) ebur128_destroy(&tiny[k]);
  ebur128_destroy(&whole);ebur128_destroy(&parts);free(pcm);
  puts("EBUR128_SEGMENT_BASELINE_OK");return 0;
}
