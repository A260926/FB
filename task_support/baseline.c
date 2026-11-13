#include "soxr.h"
#include <math.h>
#include <stdio.h>
int main(void){float in[4410],out[6000];size_t i,id=0,od=0;double e=0;soxr_io_spec_t io=soxr_io_spec(SOXR_FLOAT32_I,SOXR_FLOAT32_I);for(i=0;i<4410;i++)in[i]=(float)(.25*sin(6.283185307179586*1000*i/44100));if(soxr_oneshot(44100,48000,1,in,4410,&id,out,6000,&od,&io,0,0))return 1;for(i=0;i<od;i++)e+=out[i]*out[i];if(id!=4410||od!=4800||e<100)return 2;printf("SOXR_BASELINE consumed=%zu produced=%zu energy=%.6f\n",id,od,e);return 0;}
