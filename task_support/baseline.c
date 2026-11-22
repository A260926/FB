#include "fixture.h"
#include <stdio.h>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "BASELINE_FAIL line=%d %s\n", __LINE__, #x); return 1; } } while (0)
int main(int argc, char **argv)
{
    int normal_only = argc > 1 && strcmp(argv[1], "--normal-only") == 0;
    mlt_profile profile = fixture_profile();
    struct source_state a = {.y=40,.fail_at=-1,.code=73};
    struct source_state b = {.y=200,.fail_at=-1,.code=74};
    struct graph g = fixture_graph(profile, &a, &b);
    mlt_frame frame = NULL;
    REQUIRE(mlt_service_get_frame(MLT_TRACTOR_SERVICE(g.outer), &frame, 0) == 0 && frame);
    uint8_t *image = NULL; mlt_image_format fmt = mlt_image_yuv422; int w=16,h=8;
    REQUIRE(mlt_frame_get_image(frame, &image, &fmt, &w, &h, 0) == 0);
    REQUIRE(image && w==16 && h==8 && fmt==mlt_image_yuv422);
    REQUIRE(image[0] >= 119 && image[0] <= 120 && image[1] == 128);
    printf("CPU_DISSOLVE y=%u u=%u width=%d height=%d\n", image[0],image[1],w,h);
    mlt_frame_close(frame); fixture_graph_close(&g);
    REQUIRE(atomic_load(&a.created)==atomic_load(&a.destroyed));
    REQUIRE(atomic_load(&b.created)==atomic_load(&b.destroyed));
    printf("GRAPH_LIFETIME a=%d/%d b=%d/%d\n",a.created,a.destroyed,b.created,b.destroyed);
    for (int mode=0; mode>=-2; --mode) {
        struct source_state s = {.y=80,.fail_at=-1,.code=73};
        mlt_producer p = fixture_source(profile,&s);
        mlt_consumer c = fixture_consumer(profile,p,mode,0);
        atomic_int threads=0;
        mlt_events_listen(MLT_CONSUMER_PROPERTIES(c),&threads,"consumer-thread-started",
                         (mlt_listener)fixture_thread_started);
        REQUIRE(mlt_consumer_start(c)==0);
        for (int n=0;n<4;n++) {
            frame=mlt_consumer_rt_frame(c); REQUIRE(frame);
            REQUIRE(mlt_frame_get_position(frame)==n);
            fmt=mlt_image_yuv422; w=16; h=8; image=NULL;
            REQUIRE(mlt_frame_get_image(frame,&image,&fmt,&w,&h,0)==0 && image && image[0]==80);
            mlt_frame_close(frame);
        }
        REQUIRE(mlt_consumer_stop(c)==0); mlt_consumer_close(c); mlt_producer_close(p);
        REQUIRE(atomic_load(&s.created)==atomic_load(&s.destroyed));
        REQUIRE(mode==0 ? s.offthread==0 : s.offthread>0);
        REQUIRE(threads==(mode==-2?2:1));
        printf("CPU_EXPORT mode=%d frames=4 balanced=1 offthread=%d threads=%d\n",mode,s.offthread>0,threads);
    }
    if (!normal_only) {
        struct source_state s = {.y=80,.fail_at=0,.code=73};
        mlt_producer p=fixture_source(profile,&s);
        REQUIRE(mlt_service_get_frame(MLT_PRODUCER_SERVICE(p),&frame,0)==0);
        mlt_properties_set_int(MLT_FRAME_PROPERTIES(frame),"consumer.strict_video",1);
        image=NULL; fmt=mlt_image_yuv422; w=16; h=8;
        int result=mlt_frame_get_image(frame,&image,&fmt,&w,&h,0);
        printf("STARTING_POINT callback=73 returned=%d image=%d test_image=%d\n", result,image!=NULL,
               mlt_properties_get_int(MLT_FRAME_PROPERTIES(frame),"test_image"));
        REQUIRE(result==0 && image && mlt_properties_get_int(MLT_FRAME_PROPERTIES(frame),"test_image"));
        mlt_frame_close(frame); mlt_producer_close(p);
        REQUIRE(atomic_load(&s.created)==atomic_load(&s.destroyed));
        for (int side=0;side<2;side++) {
            struct source_state x={.y=40,.fail_at=side==0?0:-1,.code=73};
            struct source_state y={.y=200,.fail_at=side==1?0:-1,.code=74};
            struct graph bad=fixture_graph(profile,&x,&y);
            REQUIRE(mlt_service_get_frame(MLT_TRACTOR_SERVICE(bad.outer),&frame,0)==0);
            mlt_properties_set_int(MLT_FRAME_PROPERTIES(frame),"consumer.strict_video",1);
            image=NULL;fmt=mlt_image_yuv422;w=16;h=8;
            result=mlt_frame_get_image(frame,&image,&fmt,&w,&h,0);
            REQUIRE(result==0 && image && x.images==1 && y.images==1);
            printf("STARTING_POINT nested_luma side=%c callback=%d returned=%d image=1\n",
                   side==0?'A':'B',side==0?73:74,result);
            mlt_frame_close(frame);fixture_graph_close(&bad);
            REQUIRE(x.created==x.destroyed && y.created==y.destroyed);
        }
        for (int mode=0;mode>=-2;--mode) {
            struct source_state x={.y=40,.fail_at=-1,.code=73};
            struct source_state y={.y=200,.fail_at=2,.code=74};
            struct graph bad=fixture_graph(profile,&x,&y);
            mlt_consumer c=fixture_consumer(profile,MLT_TRACTOR_PRODUCER(bad.outer),mode,1);
            REQUIRE(mlt_consumer_start(c)==0);
            for (int n=0;n<3;n++) {
                frame=mlt_consumer_rt_frame(c);REQUIRE(frame && mlt_frame_get_position(frame)==n);
                image=NULL;fmt=mlt_image_yuv422;w=16;h=8;
                REQUIRE(mlt_frame_get_image(frame,&image,&fmt,&w,&h,0)==0 && image);
                if(n==2) printf("STARTING_POINT export mode=%d failed_frame_delivered=1 rendered=%d error=%d\n",
                    mode,mlt_properties_get_int(MLT_FRAME_PROPERTIES(frame),"rendered"),
                    mlt_properties_get_int(MLT_CONSUMER_PROPERTIES(c),"video_error"));
                mlt_frame_close(frame);
            }
            REQUIRE(mlt_consumer_stop(c)==0);mlt_consumer_close(c);fixture_graph_close(&bad);
            REQUIRE(x.created==x.destroyed && y.created==y.destroyed);
        }
    }
    mlt_profile_close(profile); mlt_factory_close();
    return 0;
}
