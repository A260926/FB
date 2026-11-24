/* Fixed public consumer: validates the minimum same-clock normal case. */
#include <framework/mlt.h>
#include "fixture.h"
#include <stdio.h>
#include <math.h>
#define REQUIRE(c) do{if(!(c)){fprintf(stderr,"CONFORM_BINDING_FAIL %d\n",__LINE__);return 1;}}while(0)
int main(void){
 mlt_profile profile=fixture_profile(25,1);struct fixture_state s={200,2,3,0,0,0};mlt_producer source=fixture_source(profile,&s);
 mlt_producer p=mlt_producer_conform(profile,source,200,200,2);REQUIRE(p&&mlt_producer_get_length(p)==3);
 mlt_frame f=NULL;REQUIRE(!mlt_service_get_frame(MLT_PRODUCER_SERVICE(p),&f,0)&&f);
 uint8_t *v=NULL;mlt_image_format im=mlt_image_yuv422;int w=16,h=8;REQUIRE(!mlt_frame_get_image(f,&v,&im,&w,&h,0)&&v&&v[0]==40);
 void *a=NULL;mlt_audio_format fmt=mlt_audio_f32le;int rate=200,ch=2,n=0;
 REQUIRE(!mlt_frame_get_audio(f,&a,&fmt,&rate,&ch,&n)&&a&&n==8&&rate==200&&ch==2);
 REQUIRE(fabs(((float*)a)[0]-.001f)<1e-6&&fabs(((float*)a)[1]-.101f)<1e-6);
 mlt_frame_close(f);mlt_producer_close(p);mlt_producer_close(source);REQUIRE(s.created==s.destroyed);
 mlt_profile_close(profile);mlt_factory_close();puts("CONFORM_API_BINDING_OK");return 0;
}
