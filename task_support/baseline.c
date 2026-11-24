#include "fixture.h"
#include <stdio.h>
#include <math.h>
#define REQUIRE(c) do{if(!(c)){fprintf(stderr,"BASELINE_FAIL %d\n",__LINE__);return 1;}}while(0)
int main(void){
 mlt_profile profile=fixture_profile(25,1);struct fixture_state s={44100,2,10,0,0,0};mlt_producer p=fixture_source(profile,&s);
 mlt_frame f=NULL,g=NULL;mlt_producer_seek(p,1);REQUIRE(!mlt_service_get_frame(MLT_PRODUCER_SERVICE(p),&f,0)&&f);
 mlt_producer_seek(p,4);REQUIRE(!mlt_service_get_frame(MLT_PRODUCER_SERVICE(p),&g,0)&&g);
 uint8_t *image=NULL;mlt_image_format im=mlt_image_yuv422;int w=16,h=8;
 REQUIRE(!mlt_frame_get_image(f,&image,&im,&w,&h,0)&&image&&image[0]==41);
 void *audio=NULL;mlt_audio_format fmt=mlt_audio_f32le;int rate=44100,ch=2,count=0;
 REQUIRE(!mlt_frame_get_audio(f,&audio,&fmt,&rate,&ch,&count)&&audio&&count==1764&&ch==2);
 REQUIRE(fabs(((float*)audio)[0]-.768f)<1e-6&&fabs(((float*)audio)[1]-.868f)<1e-6);
 printf("MLT_LAZY_BASELINE frame=1 video=41 samples=%d first=%.3f,%.3f source_cursor=%d\n",count,((float*)audio)[0],((float*)audio)[1],mlt_producer_position(p));
 int a=mlt_audio_calculate_frame_samples(30000.0f/1001,48000,0),b=mlt_audio_calculate_frame_samples(30000.0f/1001,48000,1);
 REQUIRE(a==1602&&b==1601);printf("MLT_SAMPLE_WINDOWS ntsc=%d,%d\n",a,b);
 mlt_frame_close(g);mlt_frame_close(f);mlt_producer_close(p);REQUIRE(s.created==2&&s.destroyed==2);
 printf("MLT_LIFETIME created=%d destroyed=%d\n",s.created,s.destroyed);mlt_profile_close(profile);mlt_factory_close();return 0;
}
