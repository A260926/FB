/* Input producers and ownership counters, not a strict-video implementation. */
#ifndef TASK_FIXTURE_H
#define TASK_FIXTURE_H
#include <framework/mlt.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

extern mlt_transition transition_luma_init(mlt_profile, mlt_service_type, const char *, char *);
struct source_state { int y, fail_at, code; atomic_int created, destroyed, images, offthread;
                      pthread_t caller; };
struct frame_token { struct source_state *state; };
static void release_token(void *opaque)
{
    struct frame_token *token = opaque;
    atomic_fetch_add(&token->state->destroyed, 1);
    free(token);
}
static int fixture_image(mlt_frame frame, uint8_t **image, mlt_image_format *fmt,
                         int *width, int *height, int writable)
{
    (void) writable;
    mlt_properties props = MLT_FRAME_PROPERTIES(frame);
    struct source_state *s = mlt_properties_get_data(props, "fixture.state", NULL);
    atomic_fetch_add(&s->images, 1);
    if (!pthread_equal(pthread_self(), s->caller)) atomic_fetch_add(&s->offthread, 1);
    if (mlt_properties_get_int(props, "fixture.fail")) { *image = NULL; return s->code; }
    *width = 16; *height = 8; *fmt = mlt_image_yuv422;
    *image = mlt_pool_alloc(16 * 8 * 2);
    for (int i = 0; i < 16 * 8 * 2; i += 2) { (*image)[i] = s->y; (*image)[i + 1] = 128; }
    mlt_frame_set_image(frame, *image, 16 * 8 * 2, mlt_pool_release);
    return 0;
}
static int fixture_frame(mlt_producer producer, mlt_frame *frame, int index)
{
    (void) index;
    struct source_state *s = mlt_properties_get_data(MLT_PRODUCER_PROPERTIES(producer),
                                                   "fixture.state", NULL);
    *frame = mlt_frame_init(MLT_PRODUCER_SERVICE(producer));
    mlt_position pos = mlt_producer_position(producer);
    mlt_frame_set_position(*frame, pos);
    mlt_properties p = MLT_FRAME_PROPERTIES(*frame);
    mlt_properties_set_data(p, "fixture.state", s, 0, NULL, NULL);
    mlt_properties_set_int(p, "fixture.fail", pos == s->fail_at);
    struct frame_token *token = malloc(sizeof(*token)); token->state = s;
    mlt_properties_set_data(p, "fixture.token", token, 0, release_token, NULL);
    atomic_fetch_add(&s->created, 1);
    mlt_frame_push_get_image(*frame, fixture_image);
    mlt_producer_prepare_next(producer);
    return 0;
}
static mlt_profile fixture_profile(void)
{
    /* Initialize the real framework pool without loading machine plugins. */
    mlt_factory_init(".task-build/empty-modules");
    mlt_profile p = calloc(1, sizeof(*p));
    p->frame_rate_num = 25; p->frame_rate_den = 1;
    p->width = 16; p->height = 8; p->progressive = 1;
    p->sample_aspect_num = 1; p->sample_aspect_den = 1;
    p->display_aspect_num = 2; p->display_aspect_den = 1; p->colorspace = 601;
    return p;
}
static mlt_producer fixture_source(mlt_profile profile, struct source_state *s)
{
    mlt_producer p = mlt_producer_new(profile);
    s->caller = pthread_self();
    p->get_frame = fixture_frame;
    mlt_properties props = MLT_PRODUCER_PROPERTIES(p);
    mlt_properties_set_data(props, "fixture.state", s, 0, NULL, NULL);
    mlt_properties_set_position(props, "length", 1000);
    mlt_producer_set_in_and_out(p, 0, 999);
    mlt_producer_set_speed(p, 1);
    return p;
}
struct graph { mlt_producer a, b; mlt_tractor inner, outer; mlt_transition luma; };
static struct graph fixture_graph(mlt_profile profile, struct source_state *a,
                                  struct source_state *b)
{
    struct graph g = {0};
    g.a = fixture_source(profile, a); g.b = fixture_source(profile, b);
    g.inner = mlt_tractor_new(); g.outer = mlt_tractor_new();
    mlt_service_set_profile(MLT_TRACTOR_SERVICE(g.inner), profile);
    mlt_service_set_profile(MLT_TRACTOR_SERVICE(g.outer), profile);
    mlt_tractor_set_track(g.inner, g.a, 0); mlt_tractor_set_track(g.inner, g.b, 1);
    g.luma = transition_luma_init(profile, mlt_service_transition_type, "luma", NULL);
    mlt_service_set_profile(MLT_TRANSITION_SERVICE(g.luma), profile);
    mlt_transition_set_in_and_out(g.luma, 0, 999);
    mlt_properties_set_double(MLT_TRANSITION_PROPERTIES(g.luma), "fixed", .5);
    mlt_field_plant_transition(mlt_tractor_field(g.inner), g.luma, 0, 1);
    mlt_tractor_set_track(g.outer, MLT_TRACTOR_PRODUCER(g.inner), 0);
    mlt_producer_set_speed(MLT_TRACTOR_PRODUCER(g.outer), 1);
    return g;
}
static void fixture_graph_close(struct graph *g)
{
    mlt_tractor_close(g->outer); mlt_tractor_close(g->inner);
    mlt_transition_close(g->luma); mlt_producer_close(g->a); mlt_producer_close(g->b);
}
/* A pull consumer still needs the same lifecycle callbacks as a real sink. */
static int fixture_consumer_start(mlt_consumer c)
{ return mlt_properties_set_int(MLT_CONSUMER_PROPERTIES(c), "fixture.running", 1); }
static int fixture_consumer_stop(mlt_consumer c)
{ return mlt_properties_set_int(MLT_CONSUMER_PROPERTIES(c), "fixture.running", 0); }
static int fixture_consumer_stopped(mlt_consumer c)
{ return !mlt_properties_get_int(MLT_CONSUMER_PROPERTIES(c), "fixture.running"); }
static void fixture_thread_started(mlt_properties props, void *counter, mlt_event_data data)
{ (void)props; (void)data; atomic_fetch_add((atomic_int *)counter,1); }
static mlt_consumer fixture_consumer(mlt_profile profile, mlt_producer producer, int mode, int strict)
{
    mlt_consumer c = mlt_consumer_new(profile);
    mlt_service_set_profile(MLT_CONSUMER_SERVICE(c), profile);
    c->start = fixture_consumer_start; c->stop = fixture_consumer_stop;
    c->is_stopped = fixture_consumer_stopped;
    mlt_properties p = MLT_CONSUMER_PROPERTIES(c);
    mlt_properties_set_int(p, "real_time", mode);
    mlt_properties_set_int(p, "strict_video", strict);
    mlt_properties_set_int(p, "audio_off", 1);
    mlt_properties_set_int(p, "buffer", 4); mlt_properties_set_int(p, "prefill", 1);
    mlt_properties_set(p, "mlt_image_format", "yuv422");
    mlt_service_connect_producer(MLT_CONSUMER_SERVICE(c), MLT_PRODUCER_SERVICE(producer), 0);
    return c;
}
#endif
