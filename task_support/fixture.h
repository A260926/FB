/* Deterministic input producer only; no conform implementation. */
#ifndef TASK_CONFORM_FIXTURE_H
#define TASK_CONFORM_FIXTURE_H
#include <framework/mlt.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
struct fixture_state {int rate, channels, length, created, destroyed, audio_calls;};
struct fixture_token {struct fixture_state *state;};
static void fixture_release(void *p){struct fixture_token *t=p;t->state->destroyed++;free(t);}
static int fixture_audio(mlt_frame f,void **data,mlt_audio_format *fmt,int *rate,int *ch,int *samples){
 mlt_properties p=MLT_FRAME_PROPERTIES(f);struct fixture_state *s=mlt_properties_get_data(p,"fixture.state",NULL);
 int64_t start=mlt_properties_get_int64(p,"fixture.start");int count=mlt_properties_get_int(p,"fixture.count");
 float *v=mlt_pool_alloc(count*s->channels*sizeof(float));if(!v)return 1;
 for(int i=0;i<count;i++)for(int c=0;c<s->channels;c++)v[i*s->channels+c]=(float)((start+i)%997+1)/1000.0f+c*.1f;
 *data=v;*fmt=mlt_audio_f32le;*rate=s->rate;*ch=s->channels;*samples=count;s->audio_calls++;
 mlt_frame_set_audio(f,v,*fmt,count*s->channels*sizeof(float),mlt_pool_release);return 0;
}
static int fixture_image(mlt_frame f,uint8_t **v,mlt_image_format *fmt,int *w,int *h,int writable){
 (void)writable;*w=16;*h=8;*fmt=mlt_image_yuv422;*v=mlt_pool_alloc(256);if(!*v)return 1;
 int pos=mlt_properties_get_int(MLT_FRAME_PROPERTIES(f),"fixture.position");
 for(int i=0;i<256;i+=2){(*v)[i]=40+pos;(*v)[i+1]=128;}mlt_frame_set_image(f,*v,256,mlt_pool_release);return 0;
}
static int fixture_frame(mlt_producer p,mlt_frame *f,int track){
 (void)track;mlt_properties pp=MLT_PRODUCER_PROPERTIES(p);struct fixture_state *s=mlt_properties_get_data(pp,"fixture.state",NULL);
 int pos=mlt_producer_position(p);mlt_profile profile=mlt_service_profile(MLT_PRODUCER_SERVICE(p));
 int64_t start=mlt_audio_calculate_samples_to_position(mlt_profile_fps(profile),s->rate,pos);
 int64_t end=mlt_audio_calculate_samples_to_position(mlt_profile_fps(profile),s->rate,pos+1);
 *f=mlt_frame_init(MLT_PRODUCER_SERVICE(p));if(!*f)return 1;mlt_frame_set_position(*f,pos);
 mlt_properties fp=MLT_FRAME_PROPERTIES(*f);mlt_properties_set_data(fp,"fixture.state",s,0,NULL,NULL);
 mlt_properties_set_int64(fp,"fixture.start",start);mlt_properties_set_int(fp,"fixture.count",end-start);mlt_properties_set_int(fp,"fixture.position",pos);
 struct fixture_token *token=malloc(sizeof(*token));token->state=s;s->created++;
 mlt_properties_set_data(fp,"fixture.token",token,0,fixture_release,NULL);
 mlt_frame_push_get_image(*f,fixture_image);mlt_frame_push_audio(*f,fixture_audio);mlt_producer_prepare_next(p);return 0;
}
static mlt_profile fixture_profile(int num,int den){
 mlt_factory_init(".task-build/empty-modules");mlt_profile p=calloc(1,sizeof(*p));
 p->frame_rate_num=num;p->frame_rate_den=den;p->width=16;p->height=8;p->progressive=1;
 p->sample_aspect_num=p->sample_aspect_den=1;p->display_aspect_num=2;p->display_aspect_den=1;p->colorspace=601;return p;
}
static mlt_producer fixture_source(mlt_profile profile,struct fixture_state *s){
 mlt_producer p=mlt_producer_new(profile);p->get_frame=fixture_frame;
 mlt_properties_set_data(MLT_PRODUCER_PROPERTIES(p),"fixture.state",s,0,NULL,NULL);
 mlt_properties_set_int(MLT_PRODUCER_PROPERTIES(p),"length",s->length);
 mlt_producer_set_in_and_out(p,0,s->length-1);mlt_producer_set_speed(p,1);return p;
}
#endif
