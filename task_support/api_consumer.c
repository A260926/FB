/* Fixed API consumer, not a playout implementation. */
#include "speex/speex_playout.h"
#include "speex/speex_jitter.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
int main(void){
 JitterBuffer *j=jitter_buffer_init(480);float a[480],b[480];JitterBufferPacket p={0},r={0};spx_int32_t off=999;
 for(unsigned z=0;z<480;z++)a[z]=.25f;
 if(!j||jitter_buffer_begin_pull(j,1000))return 21;
 p.data=(char*)a;p.len=sizeof(a);p.timestamp=1000;p.span=480;jitter_buffer_put(j,&p);
 r.data=(char*)b;r.len=sizeof(b);
 if(jitter_buffer_peek(j,&r,480,&off)!=JITTER_BUFFER_OK||off||memcmp(a,b,sizeof(a))||jitter_buffer_get_pointer_timestamp(j)!=1000)return 22;
 if(jitter_buffer_consume(j,120)||jitter_buffer_get_pointer_timestamp(j)!=1120)return 23;
 r.len=sizeof(b);if(jitter_buffer_peek(j,&r,360,&off)!=JITTER_BUFFER_OK||off!=-120||jitter_buffer_consume(j,360)||jitter_buffer_get_pointer_timestamp(j)!=1480)return 24;
 jitter_buffer_destroy(j);
 SpeexPlayout *s=speex_playout_init(2,1920);float in[960],out[960];SpeexPlayoutStats v;unsigned i,k;
 if(!s||speex_playout_reset(s,1000))return 31;
 for(i=0;i<480;i++){in[2*i]=.25f;in[2*i+1]=-.25f;}
 for(k=0;k<4;k++)if(speex_playout_put(s,1000+480*k,in,480))return 32;
 if(speex_playout_pull(s,out,480)||speex_playout_get_stats(s,&v))return 33;
 double energy=0;for(i=0;i<480;i++){if(!isfinite(out[2*i])||fabs(out[2*i]+out[2*i+1])>1e-5)return 34;energy+=out[2*i]*out[2*i];}
 if(energy<1||v.delivered!=480||v.rendered!=480||v.quanta!=1||!v.source_consumed||v.media_timestamp!=(uint32_t)(1000+v.source_consumed)||v.correction_ppm!=0)return 35;
 speex_playout_destroy(s);puts("PLAYOUT_API_NORMAL_OK");return 0;
}
