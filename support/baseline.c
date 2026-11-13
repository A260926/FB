#include "ltc.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){LTCEncoder*e=ltc_encoder_create(48000,25,LTC_TV_625_50,0);CHECK(e);ltc_encoder_set_filter(e,0);
 size_t cap=ltc_encoder_get_buffersize(e)*4;ltcsnd_sample_t *buf=calloc(cap,1);CHECK(buf);int n=0;
 for(int i=0;i<3;i++){ltc_encoder_encode_frame(e);n+=ltc_encoder_copy_buffer(e,buf+n);ltc_encoder_inc_timecode(e);}
 CHECK(!ltc_encoder_end_encode(e));n+=ltc_encoder_copy_buffer(e,buf+n);CHECK(n>5760);
 LTCDecoder*d=ltc_decoder_create(1920,8);CHECK(d);ltc_decoder_write(d,buf,n,0);LTCFrameExt frame;int count=0;while(ltc_decoder_read(d,&frame))count++;
 CHECK(count==3);printf("baseline pcm-samples=%d decoded=%d next-frame=",n,count);SMPTETimecode tc;ltc_encoder_get_timecode(e,&tc);printf("%u\n",tc.frame);CHECK(tc.frame==3);
 ltc_decoder_free(d);ltc_encoder_free(e);free(buf);return 0;}
