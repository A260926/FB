#include <aubio.h>
#include "fft_probe.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"baseline:%d %s\n",__LINE__,#x); exit(1); } } while(0)
int main(void) {
  const unsigned n=512,h=128,blocks=48,delay=n-h;
  aubio_pvoc_t *pv=new_aubio_pvoc(n,h);
  fvec_t *in=new_fvec(h), *out=new_fvec(h);
  cvec_t *grain=new_cvec(n);
  double max_error=0,energy=0;
  REQUIRE(pv && in && out && grain);
  task_fft_reset();
  for(unsigned b=0;b<blocks;b++) {
    for(unsigned j=0;j<h;j++) {
      unsigned k=b*h+j;
      in->data[j]=(smpl_t)(.2*sin(2*3.141592653589793*13*k/n)+.1*cos(2*3.141592653589793*27*k/n));
    }
    aubio_pvoc_do(pv,in,grain);
    aubio_pvoc_rdo(pv,grain,out);
    for(unsigned j=0;j<h;j++) {
      unsigned t=b*h+j;
      REQUIRE(isfinite(out->data[j]));
      if(t>=n) {
        double k=t-delay;
        double expected=.2*sin(2*3.141592653589793*13*k/n)+.1*cos(2*3.141592653589793*27*k/n);
        double e=fabs(out->data[j]-expected); if(e>max_error)max_error=e;
        energy+=out->data[j]*out->data[j];
      }
    }
  }
  REQUIRE(max_error<0.0001 && energy>1);
  REQUIRE(task_fft_forward()==blocks && task_fft_inverse()==blocks);
  printf("AUBIO_PVOC_BASELINE_OK blocks=%u forward=%zu inverse=%zu max_error=%.9g\n",blocks,task_fft_forward(),task_fft_inverse(),max_error);
  del_cvec(grain);del_fvec(in);del_fvec(out);del_aubio_pvoc(pv);aubio_cleanup();
  return 0;
}
