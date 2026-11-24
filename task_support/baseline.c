#include "speex/speex_jitter.h"
#include "speex/speex_resampler.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
int main(void){
 float in[960], decoded[960], out[960]; int i,e,off=99;
 JitterBuffer *j=jitter_buffer_init(480); if(!j)return 1;
 for(i=0;i<480;i++){in[2*i]=(float)sin(i*.07);in[2*i+1]=-in[2*i];}
 JitterBufferPacket p={0},r={0};p.data=(char*)in;p.len=sizeof(in);p.timestamp=0xfffffe00U;p.span=480;
 jitter_buffer_put(j,&p);r.data=(char*)decoded;r.len=sizeof(decoded);
 if(jitter_buffer_get(j,&r,480,&off)!=JITTER_BUFFER_OK||r.timestamp!=p.timestamp||off||memcmp(in,decoded,sizeof(in)))return 2;
 jitter_buffer_remaining_span(j,0);
 SpeexResamplerState *s=speex_resampler_init(2,48000,48000,3,&e);if(!s||e)return 3;
 if(speex_resampler_set_rate_frac(s,1000100,1000000,48000,48000)||speex_resampler_skip_zeros(s))return 4;
 spx_uint32_t ni=480,no=480;if(speex_resampler_process_interleaved_float(s,decoded,&ni,out,&no)||!ni||!no)return 5;
 double energy=0,anti=0;for(i=0;i<(int)no;i++){energy+=out[2*i]*out[2*i];anti+=fabs(out[2*i]+out[2*i+1]);}
 if(energy<1||anti>1e-5)return 6;
 printf("PRODUCTION jitter_payload=verified offset=%d consumed=%u produced=%u stereo=verified\n",off,ni,no);
 speex_resampler_destroy(s);jitter_buffer_destroy(j);return 0;
}
