#include "soundpipe.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void) {
  sp_data *sp;sp_butlp *lo;sp_buthp *hi;sp_compressor *comp;
  SPFLOAT in,low,high,out;
  double energy=0,err=0;
  int i;
  sp_create(&sp);sp->sr=48000;
  sp_butlp_create(&lo);sp_buthp_create(&hi);sp_compressor_create(&comp);
  sp_butlp_init(sp,lo);sp_buthp_init(sp,hi);sp_compressor_init(sp,comp);
  lo->freq=1200;hi->freq=1200;
  *comp->ratio=1;*comp->thresh=-12;*comp->atk=0.01;*comp->rel=0.1;
  for(i=0;i<4096;i++) {
    in=(SPFLOAT)(0.2*sin(i*0.13)+0.1*cos(i*0.57));
    assert(sp_butlp_compute(sp,lo,&in,&low)==SP_OK);
    assert(sp_buthp_compute(sp,hi,&in,&high)==SP_OK);
    assert(sp_compressor_compute(sp,comp,&low,&out)==SP_OK);
    assert(isfinite(low)&&isfinite(high)&&isfinite(out));
    energy+=(double)low*low+(double)high*high;
    err+=fabs((double)out-low);
  }
  assert(energy>1 && err<0.001);
  sp_butlp_destroy(&lo);sp_buthp_destroy(&hi);sp_compressor_destroy(&comp);
  sp_destroy(&sp);
  printf("SOUNDPIPE_BASELINE_OK samples=4096 unity-compressor=1\n");
  return 0;
}
