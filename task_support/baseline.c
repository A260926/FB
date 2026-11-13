#include "sonic.h"
#include <stdio.h>
#include <math.h>
int main(void){short in[16000],out[32000];int n=0,k;sonicStream s=sonicCreateStream(16000,1);if(!s)return 1;for(k=0;k<16000;k++)in[k]=(short)(10000*sin(2*3.141592653589793*220*k/16000));sonicSetSpeed(s,1.5f);if(!sonicWriteShortToStream(s,in,16000)||!sonicFlushStream(s))return 2;while((k=sonicReadShortFromStream(s,out+n,32000-n))>0)n+=k;sonicDestroyStream(s);if(n<10000||n>11500)return 3;printf("SONIC_BASELINE input=16000 output=%d speed=1.5\n",n);return 0;}
