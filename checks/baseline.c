#include "rnnoise.h"
#include <math.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
int main(void){DenoiseState *s=rnnoise_create(0);float x[480],y[480];double e=0;uint32_t z=1;assert(s&&rnnoise_get_frame_size()==480);for(int f=0;f<24;f++){for(int i=0;i<480;i++){z=1664525*z+1013904223;x[i]=f<20?1800*sinf((float)((f*480+i)*0.027))+((int)(z>>16)-32768)*.02f:0;}float v=rnnoise_process_frame(s,y,x);assert(isfinite(v)&&v>=0&&v<=1);for(int i=0;i<480;i++){assert(isfinite(y[i]));e+=(double)y[i]*y[i];}}assert(e>1);rnnoise_destroy(s);printf("RNNOISE_BASELINE_OK frames=24 energy=%.3f\n",e);return 0;}
