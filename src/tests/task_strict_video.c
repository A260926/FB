/* Regression cases for strict video mode through the CPU frame, tractor,
 * luma transition and consumer path.
 *
 * Material is produced by C producer callbacks generating tiny yuv422 frames;
 * no media files, devices, Qt, FFmpeg or optional modules are involved.
 */

#include "task_support/check.h"

#include <framework/mlt.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

extern mlt_transition transition_luma_init(mlt_profile, mlt_service_type, const char *, char *);

#define WIDTH 16
#define HEIGHT 8

/* ------------------------------------------------------------------ */
/* test material: a tiny yuv422 source driven by C callbacks           */

typedef struct
{
    int y;          /* luma fill value */
    int fail_at;    /* position whose get_image fails, or -1 */
    int code;       /* error code returned on failure */
    int null_image; /* get_image reports success without an image */
    int blank;      /* get_frame pushes no get_image callback at all */
    atomic_int created;
    atomic_int destroyed;
    atomic_int images;
    atomic_int offthread;
    pthread_t caller;
    /* gate to force out-of-order worker completion */
    pthread_mutex_t gate_mutex;
    pthread_cond_t gate_cond;
    int gate_open;
    int wait_pos;   /* get_image at this position blocks until the gate opens */
    int signal_pos; /* get_image at this position opens the gate */
} source_state;

typedef struct
{
    source_state *state;
} frame_token;

static void release_token(void *opaque)
{
    frame_token *token = opaque;
    atomic_fetch_add(&token->state->destroyed, 1);
    free(token);
}

static int source_get_image(mlt_frame frame,
                            uint8_t **image,
                            mlt_image_format *format,
                            int *width,
                            int *height,
                            int writable)
{
    (void) writable;
    mlt_properties props = MLT_FRAME_PROPERTIES(frame);
    source_state *s = mlt_properties_get_data(props, "test.state", NULL);
    mlt_position pos = mlt_frame_get_position(frame);
    atomic_fetch_add(&s->images, 1);
    if (!pthread_equal(pthread_self(), s->caller))
        atomic_fetch_add(&s->offthread, 1);
    if (pos == s->wait_pos) {
        pthread_mutex_lock(&s->gate_mutex);
        while (!s->gate_open)
            pthread_cond_wait(&s->gate_cond, &s->gate_mutex);
        pthread_mutex_unlock(&s->gate_mutex);
    }
    if (mlt_properties_get_int(props, "test.fail")) {
        *image = NULL;
        return s->code;
    }
    if (s->null_image) {
        *image = NULL;
        return 0;
    }
    *width = WIDTH;
    *height = HEIGHT;
    *format = mlt_image_yuv422;
    *image = mlt_pool_alloc(WIDTH * HEIGHT * 2);
    for (int i = 0; i < WIDTH * HEIGHT * 2; i += 2) {
        (*image)[i] = s->y;
        (*image)[i + 1] = 128;
    }
    mlt_frame_set_image(frame, *image, WIDTH * HEIGHT * 2, mlt_pool_release);
    if (pos == s->signal_pos) {
        pthread_mutex_lock(&s->gate_mutex);
        s->gate_open = 1;
        pthread_cond_broadcast(&s->gate_cond);
        pthread_mutex_unlock(&s->gate_mutex);
    }
    return 0;
}

static int source_get_frame(mlt_producer producer, mlt_frame *frame, int index)
{
    (void) index;
    source_state *s = mlt_properties_get_data(MLT_PRODUCER_PROPERTIES(producer),
                                              "test.state",
                                              NULL);
    *frame = mlt_frame_init(MLT_PRODUCER_SERVICE(producer));
    mlt_position pos = mlt_producer_position(producer);
    mlt_frame_set_position(*frame, pos);
    mlt_properties p = MLT_FRAME_PROPERTIES(*frame);
    mlt_properties_set_data(p, "test.state", s, 0, NULL, NULL);
    mlt_properties_set_int(p, "test.fail", pos == s->fail_at);
    frame_token *token = malloc(sizeof(*token));
    token->state = s;
    mlt_properties_set_data(p, "test.token", token, 0, release_token, NULL);
    atomic_fetch_add(&s->created, 1);
    if (!s->blank)
        mlt_frame_push_get_image(*frame, source_get_image);
    mlt_producer_prepare_next(producer);
    return 0;
}

static mlt_profile test_profile(void)
{
    /* Initialize the real framework pool once without loading machine plugins. */
    static atomic_int factory_once = 0;
    if (!atomic_exchange(&factory_once, 1))
        mlt_factory_init(".task-build/empty-modules");
    mlt_profile p = calloc(1, sizeof(*p));
    p->frame_rate_num = 25;
    p->frame_rate_den = 1;
    p->width = WIDTH;
    p->height = HEIGHT;
    p->progressive = 1;
    p->sample_aspect_num = 1;
    p->sample_aspect_den = 1;
    p->display_aspect_num = 2;
    p->display_aspect_den = 1;
    p->colorspace = 601;
    return p;
}

static void source_init(source_state *s, int y, int fail_at, int code)
{
    memset(s, 0, sizeof(*s));
    s->y = y;
    s->fail_at = fail_at;
    s->code = code;
    s->wait_pos = -1;
    s->signal_pos = -1;
    pthread_mutex_init(&s->gate_mutex, NULL);
    pthread_cond_init(&s->gate_cond, NULL);
    s->caller = pthread_self();
}

static void source_close(source_state *s)
{
    pthread_mutex_destroy(&s->gate_mutex);
    pthread_cond_destroy(&s->gate_cond);
}

static mlt_producer source_new(mlt_profile profile, source_state *s)
{
    mlt_producer p = mlt_producer_new(profile);
    s->caller = pthread_self();
    p->get_frame = source_get_frame;
    mlt_properties props = MLT_PRODUCER_PROPERTIES(p);
    mlt_properties_set_data(props, "test.state", s, 0, NULL, NULL);
    mlt_properties_set_position(props, "length", 1000);
    mlt_producer_set_in_and_out(p, 0, 999);
    mlt_producer_set_speed(p, 1);
    return p;
}

/* ------------------------------------------------------------------ */
/* two-level tractor graph with a real luma dissolve                   */

typedef struct
{
    mlt_producer a, b;
    mlt_tractor inner, outer;
    mlt_transition luma;
} graph;

static graph graph_new(mlt_profile profile, source_state *a, source_state *b)
{
    graph g = {0};
    g.a = source_new(profile, a);
    g.b = source_new(profile, b);
    g.inner = mlt_tractor_new();
    g.outer = mlt_tractor_new();
    mlt_service_set_profile(MLT_TRACTOR_SERVICE(g.inner), profile);
    mlt_service_set_profile(MLT_TRACTOR_SERVICE(g.outer), profile);
    mlt_tractor_set_track(g.inner, g.a, 0);
    mlt_tractor_set_track(g.inner, g.b, 1);
    g.luma = transition_luma_init(profile, mlt_service_transition_type, "luma", NULL);
    mlt_service_set_profile(MLT_TRANSITION_SERVICE(g.luma), profile);
    mlt_transition_set_in_and_out(g.luma, 0, 999);
    mlt_properties_set_double(MLT_TRANSITION_PROPERTIES(g.luma), "fixed", .5);
    mlt_field_plant_transition(mlt_tractor_field(g.inner), g.luma, 0, 1);
    mlt_tractor_set_track(g.outer, MLT_TRACTOR_PRODUCER(g.inner), 0);
    mlt_producer_set_speed(MLT_TRACTOR_PRODUCER(g.outer), 1);
    return g;
}

static void graph_close(graph *g)
{
    mlt_tractor_close(g->outer);
    mlt_tractor_close(g->inner);
    mlt_transition_close(g->luma);
    mlt_producer_close(g->a);
    mlt_producer_close(g->b);
}

/* ------------------------------------------------------------------ */
/* pull consumer with the lifecycle callbacks of a real sink           */

static int consumer_start_cb(mlt_consumer c)
{
    return mlt_properties_set_int(MLT_CONSUMER_PROPERTIES(c), "test.running", 1);
}

static int consumer_stop_cb(mlt_consumer c)
{
    return mlt_properties_set_int(MLT_CONSUMER_PROPERTIES(c), "test.running", 0);
}

static int consumer_stopped_cb(mlt_consumer c)
{
    return !mlt_properties_get_int(MLT_CONSUMER_PROPERTIES(c), "test.running");
}

static mlt_consumer consumer_new(mlt_profile profile, mlt_producer producer, int real_time, int strict)
{
    mlt_consumer c = mlt_consumer_new(profile);
    mlt_service_set_profile(MLT_CONSUMER_SERVICE(c), profile);
    c->start = consumer_start_cb;
    c->stop = consumer_stop_cb;
    c->is_stopped = consumer_stopped_cb;
    mlt_properties p = MLT_CONSUMER_PROPERTIES(c);
    mlt_properties_set_int(p, "real_time", real_time);
    mlt_properties_set_int(p, "strict_video", strict);
    mlt_properties_set_int(p, "audio_off", 1);
    mlt_properties_set_int(p, "buffer", 4);
    mlt_properties_set_int(p, "prefill", 1);
    mlt_properties_set(p, "mlt_image_format", "yuv422");
    mlt_service_connect_producer(MLT_CONSUMER_SERVICE(c), MLT_PRODUCER_SERVICE(producer), 0);
    return c;
}

static void thread_started_cb(mlt_properties props, void *opaque, mlt_event_data data)
{
    (void) props;
    (void) data;
    atomic_fetch_add((atomic_int *) opaque, 1);
}

typedef struct
{
    int count;
    int valid;
    int retain;
    pthread_t caller;
    mlt_position expect_pos;
    int expect_code;
    mlt_frame retained;
} event_state;

static void on_video_error(mlt_properties props, void *opaque, mlt_event_data data)
{
    event_state *s = opaque;
    s->count++;
    mlt_frame f = mlt_event_data_to_frame(data);
    int ok = f && pthread_equal(pthread_self(), s->caller)
             && mlt_frame_get_position(f) == s->expect_pos
             && mlt_properties_get_int(MLT_FRAME_PROPERTIES(f), "video_error") == s->expect_code
             && mlt_properties_get_int(props, "video_error") == s->expect_code
             && mlt_properties_get_position(props, "video_error_position") == s->expect_pos;
    s->valid = s->valid && ok;
    if (s->retain && f) {
        mlt_properties_inc_ref(MLT_FRAME_PROPERTIES(f));
        s->retained = f;
    }
}

/* ------------------------------------------------------------------ */
/* image group                                                         */

static int image_callback_failure(void)
{
    mlt_profile profile = test_profile();
    source_state s;
    source_init(&s, 80, 0, 73);
    mlt_producer p = source_new(profile, &s);
    mlt_frame f = NULL;
    TASK_CHECK(mlt_service_get_frame(MLT_PRODUCER_SERVICE(p), &f, 0) == 0 && f);
    mlt_properties props = MLT_FRAME_PROPERTIES(f);
    mlt_properties_set_int(props, "consumer.strict_video", 1);
    uint8_t *image = NULL;
    mlt_image_format format = mlt_image_yuv422;
    int w = WIDTH, h = HEIGHT;
    TASK_CHECK(mlt_frame_get_image(f, &image, &format, &w, &h, 0) == 73);
    TASK_CHECK(image == NULL);
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 73);
    TASK_CHECK(atomic_load(&s.images) == 1);
    /* the failure is sticky: the consumed lazy stack is not re-run */
    image = (uint8_t *) f;
    TASK_CHECK(mlt_frame_get_image(f, &image, &format, &w, &h, 0) == 73);
    TASK_CHECK(image == NULL);
    TASK_CHECK(atomic_load(&s.images) == 1);
    mlt_frame_close(f);
    mlt_producer_close(p);
    TASK_CHECK(atomic_load(&s.created) == atomic_load(&s.destroyed));
    source_close(&s);
    mlt_profile_close(profile);
    return 0;
}

static int fail_convert(mlt_frame self, uint8_t **image, mlt_image_format *input, mlt_image_format output)
{
    (void) self;
    (void) image;
    (void) input;
    (void) output;
    return 1;
}

static int image_convert_failure(void)
{
    mlt_profile profile = test_profile();
    source_state s;
    source_init(&s, 80, -1, 73);
    mlt_producer p = source_new(profile, &s);
    mlt_frame f = NULL;
    TASK_CHECK(mlt_service_get_frame(MLT_PRODUCER_SERVICE(p), &f, 0) == 0 && f);
    mlt_properties props = MLT_FRAME_PROPERTIES(f);
    mlt_properties_set_int(props, "consumer.strict_video", 1);
    mlt_frame_prepend_convert_image(f, fail_convert);
    uint8_t *image = NULL;
    mlt_image_format format = mlt_image_yuv422;
    int w = WIDTH, h = HEIGHT;
    TASK_CHECK(mlt_frame_get_image(f, &image, &format, &w, &h, 0) == 1);
    TASK_CHECK(image == NULL);
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 1);
    /* the conversion failure is sticky as well */
    TASK_CHECK(mlt_frame_get_image(f, &image, &format, &w, &h, 0) == 1);
    TASK_CHECK(image == NULL);
    TASK_CHECK(atomic_load(&s.images) == 1);
    mlt_frame_close(f);
    mlt_producer_close(p);
    TASK_CHECK(atomic_load(&s.created) == atomic_load(&s.destroyed));
    source_close(&s);
    mlt_profile_close(profile);
    return 0;
}

static int image_success_without_image(void)
{
    mlt_profile profile = test_profile();
    source_state s;
    source_init(&s, 80, -1, 73);
    s.null_image = 1;
    mlt_producer p = source_new(profile, &s);
    mlt_frame f = NULL;
    TASK_CHECK(mlt_service_get_frame(MLT_PRODUCER_SERVICE(p), &f, 0) == 0 && f);
    mlt_properties props = MLT_FRAME_PROPERTIES(f);
    mlt_properties_set_int(props, "consumer.strict_video", 1);
    uint8_t *image = NULL;
    mlt_image_format format = mlt_image_yuv422;
    int w = WIDTH, h = HEIGHT;
    /* success reported without an image is handled as error code 1 */
    TASK_CHECK(mlt_frame_get_image(f, &image, &format, &w, &h, 0) == 1);
    TASK_CHECK(image == NULL);
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 1);
    TASK_CHECK(mlt_properties_get_int(props, "test_image") == 0);
    mlt_frame_close(f);
    mlt_producer_close(p);
    TASK_CHECK(atomic_load(&s.created) == atomic_load(&s.destroyed));
    source_close(&s);
    mlt_profile_close(profile);
    return 0;
}

static int image_blank_frame(void)
{
    mlt_profile profile = test_profile();
    source_state s;
    source_init(&s, 80, -1, 73);
    s.blank = 1;
    mlt_producer p = source_new(profile, &s);
    mlt_frame f = NULL;
    TASK_CHECK(mlt_service_get_frame(MLT_PRODUCER_SERVICE(p), &f, 0) == 0 && f);
    mlt_properties props = MLT_FRAME_PROPERTIES(f);
    mlt_properties_set_int(props, "consumer.strict_video", 1);
    uint8_t *image = NULL;
    mlt_image_format format = mlt_image_yuv422;
    int w = WIDTH, h = HEIGHT;
    /* a genuinely blank frame still gets a test card, even in strict mode */
    TASK_CHECK(mlt_frame_get_image(f, &image, &format, &w, &h, 0) == 0);
    TASK_CHECK(image != NULL);
    TASK_CHECK(mlt_properties_get_int(props, "test_image") == 1);
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 0);
    mlt_frame_close(f);
    mlt_producer_close(p);
    TASK_CHECK(atomic_load(&s.created) == atomic_load(&s.destroyed));
    source_close(&s);
    mlt_profile_close(profile);
    return 0;
}

static int image_normal_blend(void)
{
    mlt_profile profile = test_profile();
    source_state a, b;
    source_init(&a, 40, -1, 73);
    source_init(&b, 200, -1, 74);
    graph g = graph_new(profile, &a, &b);
    mlt_frame f = NULL;
    TASK_CHECK(mlt_service_get_frame(MLT_TRACTOR_SERVICE(g.outer), &f, 0) == 0 && f);
    mlt_properties props = MLT_FRAME_PROPERTIES(f);
    mlt_properties_set_int(props, "consumer.strict_video", 1);
    uint8_t *image = NULL;
    mlt_image_format format = mlt_image_yuv422;
    int w = WIDTH, h = HEIGHT;
    TASK_CHECK(mlt_frame_get_image(f, &image, &format, &w, &h, 0) == 0);
    TASK_CHECK(image != NULL && w == WIDTH && h == HEIGHT && format == mlt_image_yuv422);
    /* dissolve of y=40 and y=200 at mix 0.5 */
    TASK_CHECK(image[0] >= 119 && image[0] <= 120 && image[1] == 128);
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 0);
    TASK_CHECK(mlt_properties_get_int(props, "test_image") == 0);
    mlt_frame_close(f);
    graph_close(&g);
    TASK_CHECK(atomic_load(&a.created) == atomic_load(&a.destroyed));
    TASK_CHECK(atomic_load(&b.created) == atomic_load(&b.destroyed));
    source_close(&a);
    source_close(&b);
    mlt_profile_close(profile);
    return 0;
}

/* ------------------------------------------------------------------ */
/* graph group                                                         */

static int graph_luma_fail(int side)
{
    mlt_profile profile = test_profile();
    source_state a, b;
    source_init(&a, 40, side == 0 ? 0 : -1, 73);
    source_init(&b, 200, side == 1 ? 0 : -1, 74);
    graph g = graph_new(profile, &a, &b);
    mlt_frame f = NULL;
    TASK_CHECK(mlt_service_get_frame(MLT_TRACTOR_SERVICE(g.outer), &f, 0) == 0 && f);
    mlt_properties props = MLT_FRAME_PROPERTIES(f);
    mlt_properties_set_int(props, "consumer.strict_video", 1);
    uint8_t *image = NULL;
    mlt_image_format format = mlt_image_yuv422;
    int w = WIDTH, h = HEIGHT;
    int expect = side == 0 ? 73 : 74;
    TASK_CHECK(mlt_frame_get_image(f, &image, &format, &w, &h, 0) == expect);
    TASK_CHECK(image == NULL);
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == expect);
    /* the first error is kept: a failing A side does not fetch B at all */
    TASK_CHECK(atomic_load(&a.images) == 1);
    TASK_CHECK(atomic_load(&b.images) == (side == 0 ? 0 : 1));
    /* sticky on the outer frame without re-running the consumed stacks */
    TASK_CHECK(mlt_frame_get_image(f, &image, &format, &w, &h, 0) == expect);
    TASK_CHECK(image == NULL);
    TASK_CHECK(atomic_load(&a.images) == 1);
    TASK_CHECK(atomic_load(&b.images) == (side == 0 ? 0 : 1));
    mlt_frame_close(f);
    graph_close(&g);
    TASK_CHECK(atomic_load(&a.created) == atomic_load(&a.destroyed));
    TASK_CHECK(atomic_load(&b.created) == atomic_load(&b.destroyed));
    source_close(&a);
    source_close(&b);
    mlt_profile_close(profile);
    return 0;
}

static int graph_luma_fail_a(void)
{
    return graph_luma_fail(0);
}

static int graph_luma_fail_b(void)
{
    return graph_luma_fail(1);
}

static int graph_luma_fail_later(void)
{
    mlt_profile profile = test_profile();
    source_state a, b;
    source_init(&a, 40, -1, 73);
    source_init(&b, 200, 2, 74);
    graph g = graph_new(profile, &a, &b);
    uint8_t *image = NULL;
    mlt_image_format format = mlt_image_yuv422;
    int w = WIDTH, h = HEIGHT;
    /* position 0 blends fine */
    mlt_frame f = NULL;
    TASK_CHECK(mlt_service_get_frame(MLT_TRACTOR_SERVICE(g.outer), &f, 0) == 0 && f);
    mlt_properties_set_int(MLT_FRAME_PROPERTIES(f), "consumer.strict_video", 1);
    TASK_CHECK(mlt_frame_get_image(f, &image, &format, &w, &h, 0) == 0 && image);
    mlt_frame_close(f);
    /* the failure appears when the timeline reaches the bad frame */
    TASK_CHECK(mlt_producer_seek(MLT_TRACTOR_PRODUCER(g.outer), 2) == 0);
    f = NULL;
    TASK_CHECK(mlt_service_get_frame(MLT_TRACTOR_SERVICE(g.outer), &f, 0) == 0 && f);
    TASK_CHECK(mlt_frame_get_position(f) == 2);
    mlt_properties props = MLT_FRAME_PROPERTIES(f);
    mlt_properties_set_int(props, "consumer.strict_video", 1);
    image = NULL;
    TASK_CHECK(mlt_frame_get_image(f, &image, &format, &w, &h, 0) == 74);
    TASK_CHECK(image == NULL);
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 74);
    mlt_frame_close(f);
    graph_close(&g);
    TASK_CHECK(atomic_load(&a.created) == atomic_load(&a.destroyed));
    TASK_CHECK(atomic_load(&b.created) == atomic_load(&b.destroyed));
    source_close(&a);
    source_close(&b);
    mlt_profile_close(profile);
    return 0;
}

/* ------------------------------------------------------------------ */
/* export group                                                        */

static int export_strict(int mode)
{
    mlt_profile profile = test_profile();
    source_state a, b;
    source_init(&a, 40, -1, 73);
    source_init(&b, 200, 2, 74);
    graph g = graph_new(profile, &a, &b);
    mlt_consumer c = consumer_new(profile, MLT_TRACTOR_PRODUCER(g.outer), mode, 1);
    mlt_properties props = MLT_CONSUMER_PROPERTIES(c);
    event_state ev;
    memset(&ev, 0, sizeof(ev));
    ev.valid = 1;
    ev.retain = mode == -2;
    ev.caller = pthread_self();
    ev.expect_pos = 2;
    ev.expect_code = 74;
    atomic_int threads = 0;
    mlt_events_listen(props, &threads, "consumer-thread-started", (mlt_listener) thread_started_cb);
    mlt_events_listen(props, &ev, "consumer-video-error", (mlt_listener) on_video_error);
    TASK_CHECK(mlt_consumer_start(c) == 0);
    /* the good prefix is delivered in program order */
    for (int n = 0; n < 2; n++) {
        mlt_frame f = mlt_consumer_rt_frame(c);
        TASK_CHECK(f != NULL);
        TASK_CHECK(mlt_frame_get_position(f) == n);
        TASK_CHECK(mlt_properties_get_int(MLT_FRAME_PROPERTIES(f), "rendered") == 1);
        TASK_CHECK(mlt_properties_get_data(MLT_FRAME_PROPERTIES(f), "image", NULL) != NULL);
        TASK_CHECK(mlt_properties_get_int(MLT_FRAME_PROPERTIES(f), "video_error") == 0);
        mlt_frame_close(f);
    }
    /* the failing frame and everything after it is withheld */
    TASK_CHECK(mlt_consumer_rt_frame(c) == NULL);
    TASK_CHECK(mlt_consumer_rt_frame(c) == NULL);
    TASK_CHECK(ev.count == 1 && ev.valid);
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 74);
    TASK_CHECK(mlt_properties_get_position(props, "video_error_position") == 2);
    /* a listener may retain the failing frame through the reference count */
    if (ev.retained) {
        TASK_CHECK(mlt_frame_get_position(ev.retained) == 2);
        TASK_CHECK(mlt_properties_get_int(MLT_FRAME_PROPERTIES(ev.retained), "video_error") == 74);
        mlt_frame_close(ev.retained);
    }
    TASK_CHECK(mlt_consumer_stop(c) == 0);
    mlt_consumer_close(c);
    graph_close(&g);
    TASK_CHECK(atomic_load(&a.created) == atomic_load(&a.destroyed));
    TASK_CHECK(atomic_load(&b.created) == atomic_load(&b.destroyed));
    TASK_CHECK(mode == 0 ? atomic_load(&a.offthread) == 0 : atomic_load(&a.offthread) > 0);
    TASK_CHECK(atomic_load(&threads) == (mode == -2 ? 2 : 1));
    source_close(&a);
    source_close(&b);
    mlt_profile_close(profile);
    return 0;
}

static int export_sync(void)
{
    return export_strict(0);
}

static int export_read_ahead(void)
{
    return export_strict(-1);
}

static int export_workers(void)
{
    return export_strict(-2);
}

static int export_out_of_order(void)
{
    mlt_profile profile = test_profile();
    source_state s;
    source_init(&s, 90, 3, 75);
    s.wait_pos = 0;   /* frame 0 only completes after frame 1 rendered */
    s.signal_pos = 1;
    mlt_producer p = source_new(profile, &s);
    mlt_consumer c = consumer_new(profile, p, -2, 1);
    mlt_properties props = MLT_CONSUMER_PROPERTIES(c);
    event_state ev;
    memset(&ev, 0, sizeof(ev));
    ev.valid = 1;
    ev.caller = pthread_self();
    ev.expect_pos = 3;
    ev.expect_code = 75;
    atomic_int threads = 0;
    mlt_events_listen(props, &threads, "consumer-thread-started", (mlt_listener) thread_started_cb);
    mlt_events_listen(props, &ev, "consumer-video-error", (mlt_listener) on_video_error);
    TASK_CHECK(mlt_consumer_start(c) == 0);
    /* program order is kept even though frame 1 finished rendering first */
    for (int n = 0; n < 3; n++) {
        mlt_frame f = mlt_consumer_rt_frame(c);
        TASK_CHECK(f != NULL);
        TASK_CHECK(mlt_frame_get_position(f) == n);
        TASK_CHECK(mlt_properties_get_int(MLT_FRAME_PROPERTIES(f), "rendered") == 1);
        mlt_frame_close(f);
    }
    TASK_CHECK(mlt_consumer_rt_frame(c) == NULL);
    TASK_CHECK(mlt_consumer_rt_frame(c) == NULL);
    TASK_CHECK(ev.count == 1 && ev.valid);
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 75);
    TASK_CHECK(mlt_properties_get_position(props, "video_error_position") == 3);
    TASK_CHECK(mlt_consumer_stop(c) == 0);
    mlt_consumer_close(c);
    mlt_producer_close(p);
    TASK_CHECK(atomic_load(&s.created) == atomic_load(&s.destroyed));
    TASK_CHECK(atomic_load(&threads) == 2);
    source_close(&s);
    mlt_profile_close(profile);
    return 0;
}

static int export_restart(void)
{
    mlt_profile profile = test_profile();
    source_state a, b;
    source_init(&a, 40, -1, 73);
    source_init(&b, 200, 2, 74);
    graph g = graph_new(profile, &a, &b);
    mlt_consumer c = consumer_new(profile, MLT_TRACTOR_PRODUCER(g.outer), -1, 1);
    mlt_properties props = MLT_CONSUMER_PROPERTIES(c);
    event_state ev;
    memset(&ev, 0, sizeof(ev));
    ev.valid = 1;
    ev.caller = pthread_self();
    ev.expect_pos = 2;
    ev.expect_code = 74;
    mlt_events_listen(props, &ev, "consumer-video-error", (mlt_listener) on_video_error);
    TASK_CHECK(mlt_consumer_start(c) == 0);
    for (int n = 0; n < 2; n++) {
        mlt_frame f = mlt_consumer_rt_frame(c);
        TASK_CHECK(f != NULL);
        mlt_frame_close(f);
    }
    TASK_CHECK(mlt_consumer_rt_frame(c) == NULL);
    TASK_CHECK(ev.count == 1 && ev.valid);
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 74);
    TASK_CHECK(mlt_consumer_stop(c) == 0);

    /* between the two exports the caller seeks and adjusts the material */
    b.fail_at = -1;
    TASK_CHECK(mlt_producer_seek(MLT_TRACTOR_PRODUCER(g.outer), 0) == 0);

    TASK_CHECK(mlt_consumer_start(c) == 0);
    /* the previous error state was cleared by start */
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 0);
    TASK_CHECK(mlt_properties_get_position(props, "video_error_position") == 0);
    for (int n = 0; n < 4; n++) {
        mlt_frame f = mlt_consumer_rt_frame(c);
        TASK_CHECK(f != NULL);
        TASK_CHECK(mlt_frame_get_position(f) == n);
        TASK_CHECK(mlt_properties_get_int(MLT_FRAME_PROPERTIES(f), "rendered") == 1);
        mlt_frame_close(f);
    }
    TASK_CHECK(ev.count == 1);
    TASK_CHECK(mlt_consumer_stop(c) == 0);
    mlt_consumer_close(c);
    graph_close(&g);
    TASK_CHECK(atomic_load(&a.created) == atomic_load(&a.destroyed));
    TASK_CHECK(atomic_load(&b.created) == atomic_load(&b.destroyed));
    source_close(&a);
    source_close(&b);
    mlt_profile_close(profile);
    return 0;
}

static int export_video_off(void)
{
    mlt_profile profile = test_profile();
    source_state s;
    source_init(&s, 80, 1, 76);
    mlt_producer p = source_new(profile, &s);
    mlt_consumer c = consumer_new(profile, p, -1, 1);
    mlt_properties props = MLT_CONSUMER_PROPERTIES(c);
    mlt_properties_set_int(props, "video_off", 1);
    event_state ev;
    memset(&ev, 0, sizeof(ev));
    ev.valid = 1;
    ev.caller = pthread_self();
    mlt_events_listen(props, &ev, "consumer-video-error", (mlt_listener) on_video_error);
    TASK_CHECK(mlt_consumer_start(c) == 0);
    /* video_off=1 does not trigger the strict video fetch */
    for (int n = 0; n < 3; n++) {
        mlt_frame f = mlt_consumer_rt_frame(c);
        TASK_CHECK(f != NULL);
        TASK_CHECK(mlt_frame_get_position(f) == n);
        TASK_CHECK(mlt_properties_get_int(MLT_FRAME_PROPERTIES(f), "rendered") == 1);
        mlt_frame_close(f);
    }
    TASK_CHECK(ev.count == 0);
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 0);
    TASK_CHECK(atomic_load(&s.images) == 0);
    TASK_CHECK(mlt_consumer_stop(c) == 0);
    mlt_consumer_close(c);
    mlt_producer_close(p);
    TASK_CHECK(atomic_load(&s.created) == atomic_load(&s.destroyed));
    source_close(&s);
    mlt_profile_close(profile);
    return 0;
}

/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"image", "callback_failure", image_callback_failure},
    {"image", "convert_failure", image_convert_failure},
    {"image", "success_without_image", image_success_without_image},
    {"image", "blank_frame", image_blank_frame},
    {"image", "normal_blend", image_normal_blend},
    {"graph", "luma_fail_a", graph_luma_fail_a},
    {"graph", "luma_fail_b", graph_luma_fail_b},
    {"graph", "luma_fail_later", graph_luma_fail_later},
    {"export", "sync", export_sync},
    {"export", "read_ahead", export_read_ahead},
    {"export", "workers", export_workers},
    {"export", "out_of_order", export_out_of_order},
    {"export", "restart", export_restart},
    {"export", "video_off", export_video_off},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
