#include "soxr.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) {
  float in[8192],out[8192]; size_t n=0; double energy=0; int i;
  soxr_error_t error;
  soxr_quality_spec_t q=soxr_quality_spec(SOXR_HQ,SOXR_VR);
  soxr_t s=soxr_create(4,1,2,&error,NULL,&q,NULL);
  if(error||!s) return 1;
  for(i=0;i<4096;i++){in[2*i]=(float)sin(i*.071);in[2*i+1]=(float)cos(i*.113)*.5f;}
  if(soxr_set_io_ratio(s,1.5,0)) return 2;
  if(soxr_process(s,in,4096,NULL,out,1024,&n)||n!=1024) return 3;
  for(i=0;i<(int)(2*n);i++) energy+=out[i]*out[i];
  printf("SOXR_VR_BASELINE frames=%zu energy=%.6f engine=%s\n",n,energy,soxr_engine(s));
  if(!(energy>100)) return 4;
  if(soxr_set_io_ratio(s,.75,256)) return 5;
  if(soxr_process(s,in,0,NULL,out,512,&n)||n!=512) return 6;
  printf("SOXR_SLEW_BASELINE frames=%zu\n",n);
  soxr_delete(s); return 0;
}
