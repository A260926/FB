#include <stddef.h>
#include "adpcm-lib.h"
#include <stdio.h>
#include <math.h>
#include <stdint.h>
int main(void){int16_t in[514],out[600];uint8_t bits[1024];int i,n;size_t bytes=0;uint64_t sse=0;void *c=adpcm_create_context(2,16000,3,NOISE_SHAPING_OFF);if(!c)return 1;for(i=0;i<257;i++){in[2*i]=(int16_t)(9000*sin(.17*i));in[2*i+1]=(int16_t)(5000*cos(.23*i));}if(!adpcm_encode_block(c,bits,&bytes,in,257))return 2;n=adpcm_decode_block(out,bits,bytes,2);if(n!=257||!bytes)return 3;for(i=0;i<514;i++){int64_t d=(int32_t)in[i]-out[i];sse+=(uint64_t)(d*d);}if(sse>1000000000ULL)return 4;printf("ADPCM_BASELINE frames=%d bytes=%zu sse=%llu\n",n,bytes,(unsigned long long)sse);adpcm_free_context(c);return 0;}
