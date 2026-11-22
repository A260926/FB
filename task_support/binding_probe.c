/* Fixed minimum production consumers. This file supplies no feature code. */
#include "fixture.h"
#include <pthread.h>
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"BINDING_FAIL line=%d %s\n",__LINE__,#x); fflush(stderr); _Exit(1); } } while (0)
static int image_probe(mlt_profile profile)
{
    struct source_state s={.y=80,.fail_at=0,.code=73};
    mlt_producer p=fixture_source(profile,&s); mlt_frame f=NULL;
    CHECK(mlt_service_get_frame(MLT_PRODUCER_SERVICE(p),&f,0)==0 && f);
    mlt_properties props=MLT_FRAME_PROPERTIES(f);
    mlt_properties_set_int(props,"consumer.strict_video",1);
    uint8_t *image=NULL; mlt_image_format format=mlt_image_yuv422; int w=16,h=8;
    int e=mlt_frame_get_image(f,&image,&format,&w,&h,0);
    CHECK(e==73 && image==NULL && mlt_properties_get_int(props,"video_error")==73);
    CHECK(atomic_load(&s.images)==1);
    image=(uint8_t *)f;
    CHECK(mlt_frame_get_image(f,&image,&format,&w,&h,0)==73 && image==NULL);
    CHECK(atomic_load(&s.images)==1);
    mlt_frame_close(f); mlt_producer_close(p);
    CHECK(atomic_load(&s.created)==atomic_load(&s.destroyed));
    puts("BINDING_IMAGE sticky=73 calls=1 balanced=1"); return 0;
}
static int graph_probe(mlt_profile profile)
{
    for(int side=0;side<2;side++) {
        struct source_state a={.y=40,.fail_at=side==0?0:-1,.code=73};
        struct source_state b={.y=200,.fail_at=side==1?0:-1,.code=74};
        struct graph g=fixture_graph(profile,&a,&b); mlt_frame f=NULL;
        CHECK(mlt_service_get_frame(MLT_TRACTOR_SERVICE(g.outer),&f,0)==0 && f);
        mlt_properties_set_int(MLT_FRAME_PROPERTIES(f),"consumer.strict_video",1);
        uint8_t *image=NULL; mlt_image_format format=mlt_image_yuv422; int w=16,h=8;
        int e=mlt_frame_get_image(f,&image,&format,&w,&h,0);
        CHECK(e==(side==0?73:74) && image==NULL);
        CHECK(mlt_properties_get_int(MLT_FRAME_PROPERTIES(f),"video_error")==e);
        mlt_frame_close(f); fixture_graph_close(&g);
        CHECK(a.created==a.destroyed && b.created==b.destroyed);
    }
    puts("BINDING_GRAPH nested=2 luma=1 sides=2 balanced=1"); return 0;
}
struct event_state { int count, valid; pthread_t caller; };
static void on_error(mlt_properties props, void *opaque, mlt_event_data data)
{
    struct event_state *s=opaque; s->count++;
    mlt_frame f=mlt_event_data_to_frame(data);
    s->valid=f && pthread_equal(pthread_self(),s->caller)
      && mlt_frame_get_position(f)==2
      && mlt_properties_get_int(MLT_FRAME_PROPERTIES(f),"video_error")==74
      && mlt_properties_get_int(props,"video_error")==74
      && mlt_properties_get_position(props,"video_error_position")==2;
}
static int export_probe(mlt_profile profile,int mode)
{
    struct source_state a={.y=40,.fail_at=-1,.code=73};
    struct source_state b={.y=200,.fail_at=2,.code=74};
    struct graph g=fixture_graph(profile,&a,&b);
    mlt_consumer c=fixture_consumer(profile,MLT_TRACTOR_PRODUCER(g.outer),mode,1);
    mlt_properties props=MLT_CONSUMER_PROPERTIES(c);
    struct event_state ev={.caller=pthread_self()};
    atomic_int threads=0;
    mlt_events_listen(props,&threads,"consumer-thread-started",(mlt_listener)fixture_thread_started);
    /* Register is idempotent and lets the old starting library attach a listener. */
    mlt_events_register(props,"consumer-video-error");
    mlt_events_listen(props,&ev,"consumer-video-error",(mlt_listener)on_error);
    CHECK(mlt_consumer_start(c)==0);
    for(int n=0;n<2;n++) {
        mlt_frame f=mlt_consumer_rt_frame(c); CHECK(f);
        CHECK(mlt_frame_get_position(f)==n);
        CHECK(mlt_properties_get_int(MLT_FRAME_PROPERTIES(f),"rendered")==1);
        CHECK(mlt_properties_get_data(MLT_FRAME_PROPERTIES(f),"image",NULL)!=NULL);
        mlt_frame_close(f);
    }
    CHECK(mlt_consumer_rt_frame(c)==NULL);
    CHECK(mlt_consumer_rt_frame(c)==NULL);
    CHECK(ev.count==1 && ev.valid);
    CHECK(mlt_consumer_stop(c)==0);
    mlt_consumer_close(c); fixture_graph_close(&g);
    CHECK(a.created==a.destroyed && b.created==b.destroyed);
    CHECK(mode==0 ? a.offthread==0 : a.offthread>0);
    CHECK(threads==(mode==-2?2:1));
    printf("BINDING_EXPORT mode=%d prefix=2 error=74 events=1 balanced=1\n",mode);
    return 0;
}
int main(int argc,char **argv)
{
    mlt_profile profile=fixture_profile(); int result=0;
    const char *which=argc>1?argv[1]:"all";
    if(!strcmp(which,"all") || !strcmp(which,"image")) result=image_probe(profile);
    if(!result && (!strcmp(which,"all") || !strcmp(which,"graph"))) result=graph_probe(profile);
    for(int mode=0;!result && mode>=-2;--mode) {
        char name[16]; snprintf(name,sizeof(name),"export%d",-mode);
        if(!strcmp(which,"all") || !strcmp(which,name)) result=export_probe(profile,mode);
    }
    mlt_profile_close(profile); mlt_factory_close(); return result;
}
