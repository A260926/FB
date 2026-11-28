/*
 * task_strict_video.c -- regressions for strict CPU video rendering.
 *
 * Covers the frame-level sticky error, nested tractor/luma propagation, and
 * ordered strict export on the consumer. All sources are C producer callbacks
 * that synthesize tiny yuv422 images; no media, device, or plugin is used.
 */

#include "task_support/check.h"
#include "task_support/fixture.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Image group: direct frame fetches                                   */
/* ------------------------------------------------------------------ */

struct image_probe {
    int fail;          /* return an error from the get_image callback */
    int no_image;      /* return success without providing an image */
    int code;
    int calls;
};
static int probe_get_image(mlt_frame frame,
                           uint8_t **image,
                           mlt_image_format *format,
                           int *width,
                           int *height,
                           int writable)
{
    (void) writable;
    struct image_probe *probe = mlt_properties_get_data(MLT_FRAME_PROPERTIES(frame),
                                                        "probe.state",
                                                        NULL);
    probe->calls++;
    if (probe->fail) {
        *image = NULL;
        return probe->code;
    }
    if (probe->no_image)
        return 0;
    *width = 16;
    *height = 8;
    *format = mlt_image_yuv422;
    *image = mlt_pool_alloc(16 * 8 * 2);
    for (int i = 0; i < 16 * 8 * 2; i += 2) {
        (*image)[i] = 80;
        (*image)[i + 1] = 128;
    }
    mlt_frame_set_image(frame, *image, 16 * 8 * 2, mlt_pool_release);
    return 0;
}

static int probe_convert_fail(mlt_frame frame,
                              uint8_t **image,
                              mlt_image_format *input,
                              mlt_image_format output)
{
    (void) frame;
    (void) image;
    (void) input;
    (void) output;
    return 55;
}

static int case_image_callback_failure(void)
{
    mlt_profile profile = fixture_profile();
    struct source_state source = {.y = 80, .fail_at = -1, .code = 73};
    mlt_producer producer = fixture_source(profile, &source);
    struct image_probe probe = {.fail = 1, .code = 73};
    mlt_frame frame = mlt_frame_init(MLT_PRODUCER_SERVICE(producer));
    mlt_properties props = MLT_FRAME_PROPERTIES(frame);

    mlt_properties_set_data(props, "probe.state", &probe, 0, NULL, NULL);
    mlt_frame_push_get_image(frame, probe_get_image);
    mlt_properties_set_int(props, "consumer.strict_video", 1);

    /* A pre-existing image property must not mask a decode failure. */
    mlt_properties_set_data(props, "image", mlt_pool_alloc(16 * 8 * 2), 0, mlt_pool_release, NULL);

    uint8_t *image = (uint8_t *) frame;
    mlt_image_format format = mlt_image_yuv422;
    int width = 16, height = 8;
    int error = mlt_frame_get_image(frame, &image, &format, &width, &height, 0);
    TASK_CHECK(error == 73);
    TASK_CHECK(image == NULL);
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 73);
    TASK_CHECK(probe.calls == 1);

    /* The failure is sticky and the lazy stack is not consumed twice. */
    image = (uint8_t *) frame;
    error = mlt_frame_get_image(frame, &image, &format, &width, &height, 0);
    TASK_CHECK(error == 73);
    TASK_CHECK(image == NULL);
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 73);
    TASK_CHECK(probe.calls == 1);

    mlt_frame_close(frame);
    mlt_producer_close(producer);
    mlt_profile_close(profile);
    return 0;
}

static int case_image_convert_failure(void)
{
    mlt_profile profile = fixture_profile();
    struct source_state source = {.y = 80, .fail_at = -1, .code = 73};
    mlt_producer producer = fixture_source(profile, &source);
    struct image_probe probe = {0};
    mlt_frame frame = mlt_frame_init(MLT_PRODUCER_SERVICE(producer));
    mlt_properties props = MLT_FRAME_PROPERTIES(frame);

    mlt_properties_set_data(props, "probe.state", &probe, 0, NULL, NULL);
    mlt_frame_push_get_image(frame, probe_get_image);
    mlt_frame_prepend_convert_image(frame, probe_convert_fail);
    mlt_properties_set_int(props, "consumer.strict_video", 1);

    uint8_t *image = NULL;
    mlt_image_format format = mlt_image_rgba;
    int width = 16, height = 8;
    int error = mlt_frame_get_image(frame, &image, &format, &width, &height, 0);
    TASK_CHECK(error == 55);
    TASK_CHECK(image == NULL);
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 55);
    TASK_CHECK(probe.calls == 1);

    /* Repeated fetch is sticky too. */
    image = NULL;
    error = mlt_frame_get_image(frame, &image, &format, &width, &height, 0);
    TASK_CHECK(error == 55);
    TASK_CHECK(image == NULL);

    mlt_frame_close(frame);
    mlt_producer_close(producer);
    mlt_profile_close(profile);
    return 0;
}

static int case_image_success_without_image(void)
{
    mlt_profile profile = fixture_profile();
    struct source_state source = {.y = 80, .fail_at = -1, .code = 73};
    mlt_producer producer = fixture_source(profile, &source);
    struct image_probe probe = {.no_image = 1};
    mlt_frame frame = mlt_frame_init(MLT_PRODUCER_SERVICE(producer));
    mlt_properties props = MLT_FRAME_PROPERTIES(frame);

    mlt_properties_set_data(props, "probe.state", &probe, 0, NULL, NULL);
    mlt_frame_push_get_image(frame, probe_get_image);
    mlt_properties_set_int(props, "consumer.strict_video", 1);

    uint8_t *image = NULL;
    mlt_image_format format = mlt_image_yuv422;
    int width = 16, height = 8;
    int error = mlt_frame_get_image(frame, &image, &format, &width, &height, 0);
    TASK_CHECK(error == 1);
    TASK_CHECK(image == NULL);
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 1);
    TASK_CHECK(mlt_properties_get_int(props, "test_image") == 0);

    mlt_frame_close(frame);
    mlt_producer_close(producer);
    mlt_profile_close(profile);
    return 0;
}

static int case_image_blank_frame(void)
{
    mlt_profile profile = fixture_profile();
    struct source_state source = {.y = 80, .fail_at = -1, .code = 73};
    mlt_producer producer = fixture_source(profile, &source);

    /* A frame with no image source at all is a timeline gap, not a decode
     * failure: strict mode still produces the ordinary test card. */
    mlt_frame frame = mlt_frame_init(MLT_PRODUCER_SERVICE(producer));
    mlt_properties props = MLT_FRAME_PROPERTIES(frame);
    mlt_properties_set_int(props, "consumer.strict_video", 1);

    uint8_t *image = NULL;
    mlt_image_format format = mlt_image_yuv422;
    int width = 16, height = 8;
    int error = mlt_frame_get_image(frame, &image, &format, &width, &height, 0);
    TASK_CHECK(error == 0);
    TASK_CHECK(image != NULL);
    TASK_CHECK(mlt_properties_get_int(props, "test_image") == 1);
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 0);

    mlt_frame_close(frame);
    mlt_producer_close(producer);
    mlt_profile_close(profile);
    return 0;
}

static int case_image_preview_fallback_kept(void)
{
    /* With strict video off, the original preview fallback is unchanged:
     * the callback fails but the caller gets a test image and success. */
    mlt_profile profile = fixture_profile();
    struct source_state source = {.y = 80, .fail_at = -1, .code = 73};
    mlt_producer producer = fixture_source(profile, &source);
    struct image_probe probe = {.fail = 1, .code = 73};
    mlt_frame frame = mlt_frame_init(MLT_PRODUCER_SERVICE(producer));
    mlt_properties props = MLT_FRAME_PROPERTIES(frame);

    mlt_properties_set_data(props, "probe.state", &probe, 0, NULL, NULL);
    mlt_frame_push_get_image(frame, probe_get_image);

    uint8_t *image = NULL;
    mlt_image_format format = mlt_image_yuv422;
    int width = 16, height = 8;
    int error = mlt_frame_get_image(frame, &image, &format, &width, &height, 0);
    TASK_CHECK(error == 0);
    TASK_CHECK(image != NULL);
    TASK_CHECK(mlt_properties_get_int(props, "test_image") == 1);

    mlt_frame_close(frame);
    mlt_producer_close(producer);
    mlt_profile_close(profile);
    return 0;
}

static int case_image_normal_success(void)
{
    mlt_profile profile = fixture_profile();
    struct source_state source = {.y = 80, .fail_at = -1, .code = 73};
    mlt_producer producer = fixture_source(profile, &source);
    struct image_probe probe = {0};
    mlt_frame frame = mlt_frame_init(MLT_PRODUCER_SERVICE(producer));
    mlt_properties props = MLT_FRAME_PROPERTIES(frame);

    mlt_properties_set_data(props, "probe.state", &probe, 0, NULL, NULL);
    mlt_frame_push_get_image(frame, probe_get_image);
    mlt_properties_set_int(props, "consumer.strict_video", 1);

    uint8_t *image = NULL;
    mlt_image_format format = mlt_image_yuv422;
    int width = 16, height = 8;
    int error = mlt_frame_get_image(frame, &image, &format, &width, &height, 0);
    TASK_CHECK(error == 0);
    TASK_CHECK(image != NULL);
    TASK_CHECK(image[0] == 80);
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 0);
    TASK_CHECK(mlt_properties_get_int(props, "test_image") == 0);

    mlt_frame_close(frame);
    mlt_producer_close(producer);
    mlt_profile_close(profile);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Graph group: nested tractor + luma transition                       */
/* ------------------------------------------------------------------ */

static int case_graph_luma_side_failure(void)
{
    for (int side = 0; side < 2; ++side) {
        mlt_profile profile = fixture_profile();
        struct source_state a = {.y = 40, .fail_at = side == 0 ? 0 : -1,
                                 .code = side == 0 ? 73 : 999};
        struct source_state b = {.y = 200, .fail_at = side == 1 ? 0 : -1,
                                 .code = side == 1 ? 74 : 999};
        struct graph graph = fixture_graph(profile, &a, &b);
        mlt_frame frame = NULL;
        int expected = side == 0 ? 73 : 74;

        TASK_CHECK(mlt_service_get_frame(MLT_TRACTOR_SERVICE(graph.outer), &frame, 0) == 0);
        TASK_CHECK(frame != NULL);
        mlt_properties_set_int(MLT_FRAME_PROPERTIES(frame), "consumer.strict_video", 1);

        uint8_t *image = NULL;
        mlt_image_format format = mlt_image_yuv422;
        int width = 16, height = 8;
        int error = mlt_frame_get_image(frame, &image, &format, &width, &height, 0);
        TASK_CHECK(error == expected);
        TASK_CHECK(image == NULL);
        TASK_CHECK(mlt_properties_get_int(MLT_FRAME_PROPERTIES(frame),
                                          "consumer.strict_video")
                   == 1);

        /* Neither side may blend: the failing side is fetched but the good
         * side is not blended into a published image. The failing side was
         * fetched once and the other side at most once. */
        TASK_CHECK(atomic_load(&a.images) <= 1);
        TASK_CHECK(atomic_load(&b.images) <= 1);
        TASK_CHECK(side == 0 ? atomic_load(&b.images) == 0 : atomic_load(&a.images) == 1);

        mlt_frame_close(frame);
        fixture_graph_close(&graph);
        TASK_CHECK(atomic_load(&a.created) == atomic_load(&a.destroyed));
        TASK_CHECK(atomic_load(&b.created) == atomic_load(&b.destroyed));
        mlt_profile_close(profile);
    }
    return 0;
}

static int case_graph_luma_normal_mix(void)
{
    mlt_profile profile = fixture_profile();
    struct source_state a = {.y = 40, .fail_at = -1, .code = 73};
    struct source_state b = {.y = 200, .fail_at = -1, .code = 74};
    struct graph graph = fixture_graph(profile, &a, &b);
    mlt_frame frame = NULL;

    TASK_CHECK(mlt_service_get_frame(MLT_TRACTOR_SERVICE(graph.outer), &frame, 0) == 0);
    mlt_properties_set_int(MLT_FRAME_PROPERTIES(frame), "consumer.strict_video", 1);

    uint8_t *image = NULL;
    mlt_image_format format = mlt_image_yuv422;
    int width = 16, height = 8;
    int error = mlt_frame_get_image(frame, &image, &format, &width, &height, 0);
    TASK_CHECK(error == 0);
    TASK_CHECK(image != NULL);
    TASK_CHECK(mlt_properties_get_int(MLT_FRAME_PROPERTIES(frame), "video_error") == 0);
    TASK_CHECK(width == 16 && height == 8 && format == mlt_image_yuv422);
    TASK_CHECK(image[0] >= 119 && image[0] <= 121);
    TASK_CHECK(image[1] == 128);
    TASK_CHECK(atomic_load(&a.images) == 1);
    TASK_CHECK(atomic_load(&b.images) == 1);

    mlt_frame_close(frame);
    fixture_graph_close(&graph);
    TASK_CHECK(atomic_load(&a.created) == atomic_load(&a.destroyed));
    TASK_CHECK(atomic_load(&b.created) == atomic_load(&b.destroyed));
    mlt_profile_close(profile);
    return 0;
}

static int case_graph_black_is_not_failure(void)
{
    mlt_profile profile = fixture_profile();
    struct source_state a = {.y = 0, .fail_at = -1, .code = 73};
    struct source_state b = {.y = 0, .fail_at = -1, .code = 74};
    struct graph graph = fixture_graph(profile, &a, &b);
    mlt_frame frame = NULL;

    TASK_CHECK(mlt_service_get_frame(MLT_TRACTOR_SERVICE(graph.outer), &frame, 0) == 0);
    mlt_properties_set_int(MLT_FRAME_PROPERTIES(frame), "consumer.strict_video", 1);

    uint8_t *image = NULL;
    mlt_image_format format = mlt_image_yuv422;
    int width = 16, height = 8;
    int error = mlt_frame_get_image(frame, &image, &format, &width, &height, 0);
    TASK_CHECK(error == 0);
    TASK_CHECK(image != NULL);
    TASK_CHECK(mlt_properties_get_int(MLT_FRAME_PROPERTIES(frame), "video_error") == 0);
    TASK_CHECK(image[0] == 0);

    mlt_frame_close(frame);
    fixture_graph_close(&graph);
    mlt_profile_close(profile);
    return 0;
}

/* Producers whose frames carry no image source at all. */
static int blank_frame_get(mlt_producer producer, mlt_frame *frame, int index)
{
    (void) index;
    *frame = mlt_frame_init(MLT_PRODUCER_SERVICE(producer));
    mlt_frame_set_position(*frame, mlt_producer_position(producer));
    mlt_producer_prepare_next(producer);
    return 0;
}
static mlt_producer blank_source(mlt_profile profile)
{
    mlt_producer producer = mlt_producer_new(profile);
    producer->get_frame = blank_frame_get;
    mlt_properties_set_position(MLT_PRODUCER_PROPERTIES(producer), "length", 1000);
    mlt_producer_set_in_and_out(producer, 0, 999);
    mlt_producer_set_speed(producer, 1);
    return producer;
}

static int case_graph_blank_test_image(void)
{
    mlt_profile profile = fixture_profile();
    mlt_producer a = blank_source(profile);
    mlt_producer b = blank_source(profile);
    mlt_tractor inner = mlt_tractor_new();
    mlt_tractor outer = mlt_tractor_new();
    mlt_transition luma;

    mlt_service_set_profile(MLT_TRACTOR_SERVICE(inner), profile);
    mlt_service_set_profile(MLT_TRACTOR_SERVICE(outer), profile);
    mlt_tractor_set_track(inner, a, 0);
    mlt_tractor_set_track(inner, b, 1);
    luma = transition_luma_init(profile, mlt_service_transition_type, "luma", NULL);
    mlt_service_set_profile(MLT_TRANSITION_SERVICE(luma), profile);
    mlt_transition_set_in_and_out(luma, 0, 999);
    mlt_properties_set_double(MLT_TRANSITION_PROPERTIES(luma), "fixed", .5);
    mlt_field_plant_transition(mlt_tractor_field(inner), luma, 0, 1);
    mlt_tractor_set_track(outer, MLT_TRACTOR_PRODUCER(inner), 0);
    mlt_producer_set_speed(MLT_TRACTOR_PRODUCER(outer), 1);

    mlt_frame frame = NULL;
    TASK_CHECK(mlt_service_get_frame(MLT_TRACTOR_SERVICE(outer), &frame, 0) == 0);
    mlt_properties_set_int(MLT_FRAME_PROPERTIES(frame), "consumer.strict_video", 1);

    uint8_t *image = NULL;
    mlt_image_format format = mlt_image_yuv422;
    int width = 16, height = 8;
    int error = mlt_frame_get_image(frame, &image, &format, &width, &height, 0);
    TASK_CHECK(error == 0);
    TASK_CHECK(image != NULL);
    /* A totally empty timeline is a gap, not a decode failure. */
    TASK_CHECK(mlt_properties_get_int(MLT_FRAME_PROPERTIES(frame), "video_error") == 0);
    TASK_CHECK(mlt_properties_get_int(MLT_FRAME_PROPERTIES(frame), "test_image") == 1);

    mlt_frame_close(frame);
    mlt_tractor_close(outer);
    mlt_tractor_close(inner);
    mlt_transition_close(luma);
    mlt_producer_close(a);
    mlt_producer_close(b);
    mlt_profile_close(profile);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Export group: strict consumer delivery                              */
/* ------------------------------------------------------------------ */

struct export_event {
    int count;
    int valid;
    pthread_t caller;
    int expected_position;
    int expected_code;
    mlt_frame retained;
};
static void on_video_error(mlt_properties props, void *opaque, mlt_event_data data)
{
    struct export_event *ev = opaque;
    mlt_frame frame = mlt_event_data_to_frame(data);
    ev->count++;
    if (frame
        && pthread_equal(pthread_self(), ev->caller)
        && mlt_frame_get_position(frame) == ev->expected_position
        && mlt_properties_get_int(MLT_FRAME_PROPERTIES(frame), "video_error")
               == ev->expected_code
        && mlt_properties_get_int(props, "video_error") == ev->expected_code
        && mlt_properties_get_position(props, "video_error_position")
               == ev->expected_position) {
        ev->valid = 1;
        /* Retain the failing frame through the framework's reference count;
         * the consumer's own reference is released right after the event. */
        if (!ev->retained) {
            mlt_properties_inc_ref(MLT_FRAME_PROPERTIES(frame));
            ev->retained = frame;
        }
    }
}

static int consume_prefix(mlt_consumer consumer, struct export_event *ev, int prefix)
{
    for (int n = 0; n < prefix; ++n) {
        mlt_frame frame = mlt_consumer_rt_frame(consumer);
        TASK_CHECK(frame != NULL);
        TASK_CHECK(mlt_frame_get_position(frame) == n);
        TASK_CHECK(mlt_properties_get_int(MLT_FRAME_PROPERTIES(frame), "rendered") == 1);
        TASK_CHECK(mlt_properties_get_data(MLT_FRAME_PROPERTIES(frame), "image", NULL)
                   != NULL);
        mlt_frame_close(frame);
    }
    return 0;
}

static int finish_failure(mlt_consumer consumer, struct export_event *ev)
{
    TASK_CHECK(mlt_consumer_rt_frame(consumer) == NULL);
    TASK_CHECK(mlt_consumer_rt_frame(consumer) == NULL);
    TASK_CHECK(ev->count == 1);
    TASK_CHECK(ev->valid == 1);
    TASK_CHECK(ev->retained != NULL);
    TASK_CHECK(mlt_properties_get_int(MLT_CONSUMER_PROPERTIES(consumer), "video_error")
               == ev->expected_code);
    TASK_CHECK(mlt_properties_get_position(MLT_CONSUMER_PROPERTIES(consumer),
                                           "video_error_position")
               == ev->expected_position);
    return 0;
}

static int run_export_mode(int mode)
{
    mlt_profile profile = fixture_profile();
    struct source_state source = {.y = 80, .fail_at = 1, .code = 74};
    mlt_producer producer = fixture_source(profile, &source);
    mlt_consumer consumer = fixture_consumer(profile, producer, mode, 1);
    mlt_properties props = MLT_CONSUMER_PROPERTIES(consumer);
    struct export_event ev = {.caller = pthread_self(),
                              .expected_position = 1,
                              .expected_code = 74};

    mlt_events_register(props, "consumer-video-error");
    mlt_events_listen(props, &ev, "consumer-video-error", (mlt_listener) on_video_error);
    TASK_CHECK(mlt_consumer_start(consumer) == 0);
    if (consume_prefix(consumer, &ev, 1))
        return 1;
    if (finish_failure(consumer, &ev))
        return 1;
    mlt_frame_close(ev.retained);

    TASK_CHECK(mlt_consumer_stop(consumer) == 0);
    mlt_consumer_close(consumer);
    mlt_producer_close(producer);
    TASK_CHECK(atomic_load(&source.created) == atomic_load(&source.destroyed));
    mlt_profile_close(profile);
    return 0;
}

static int case_export_mode0(void)
{
    return run_export_mode(0);
}
static int case_export_mode_minus1(void)
{
    return run_export_mode(-1);
}
static int case_export_mode_minus2(void)
{
    return run_export_mode(-2);
}

/* Ordered producer able to force out-of-order worker completion. */
struct ordering_state {
    atomic_int created;
    atomic_int destroyed;
    atomic_int images;
    atomic_int offthread;
    int fail_at;
    int code;
    int gate;       /* position 1 waits for the failure before rendering */
    int fail_done;
    int one_started;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t caller;
};
struct ordering_token {
    struct ordering_state *state;
};
static void ordering_release(void *opaque)
{
    struct ordering_token *token = opaque;
    atomic_fetch_add(&token->state->destroyed, 1);
    free(token);
}
static int ordering_get_image(mlt_frame frame,
                              uint8_t **image,
                              mlt_image_format *format,
                              int *width,
                              int *height,
                              int writable)
{
    (void) writable;
    struct ordering_state *state
        = mlt_properties_get_data(MLT_FRAME_PROPERTIES(frame), "order.state", NULL);
    int position = (int) mlt_frame_get_position(frame);
    atomic_fetch_add(&state->images, 1);
    if (!pthread_equal(pthread_self(), state->caller))
        atomic_fetch_add(&state->offthread, 1);

    if (state->gate && position == 1) {
        pthread_mutex_lock(&state->mutex);
        state->one_started = 1;
        pthread_cond_broadcast(&state->cond);
        while (!state->fail_done)
            pthread_cond_wait(&state->cond, &state->mutex);
        pthread_mutex_unlock(&state->mutex);
    }
    if (state->fail_at >= 0
        && (position == state->fail_at || position == state->fail_at + 1)) {
        pthread_mutex_lock(&state->mutex);
        state->fail_done = 1;
        pthread_cond_broadcast(&state->cond);
        pthread_mutex_unlock(&state->mutex);
        *image = NULL;
        return position == state->fail_at ? state->code : state->code + 1;
    }

    *width = 16;
    *height = 8;
    *format = mlt_image_yuv422;
    *image = mlt_pool_alloc(16 * 8 * 2);
    for (int i = 0; i < 16 * 8 * 2; i += 2) {
        (*image)[i] = (uint8_t) (20 + position * 10);
        (*image)[i + 1] = 128;
    }
    mlt_frame_set_image(frame, *image, 16 * 8 * 2, mlt_pool_release);
    return 0;
}
static int ordering_frame(mlt_producer producer, mlt_frame *frame, int index)
{
    (void) index;
    struct ordering_state *state
        = mlt_properties_get_data(MLT_PRODUCER_PROPERTIES(producer), "order.state", NULL);
    *frame = mlt_frame_init(MLT_PRODUCER_SERVICE(producer));
    mlt_frame_set_position(*frame, mlt_producer_position(producer));
    mlt_properties_set_data(MLT_FRAME_PROPERTIES(*frame), "order.state", state, 0, NULL, NULL);
    struct ordering_token *token = malloc(sizeof(*token));
    token->state = state;
    mlt_properties_set_data(MLT_FRAME_PROPERTIES(*frame), "test.token", token, 0,
                            ordering_release, NULL);
    atomic_fetch_add(&state->created, 1);
    mlt_frame_push_get_image(*frame, ordering_get_image);
    mlt_producer_prepare_next(producer);
    return 0;
}
static mlt_producer ordering_source(mlt_profile profile, struct ordering_state *state)
{
    state->caller = pthread_self();
    pthread_mutex_init(&state->mutex, NULL);
    pthread_cond_init(&state->cond, NULL);
    mlt_producer producer = mlt_producer_new(profile);
    producer->get_frame = ordering_frame;
    mlt_properties props = MLT_PRODUCER_PROPERTIES(producer);
    mlt_properties_set_data(props, "order.state", state, 0, NULL, NULL);
    mlt_properties_set_position(props, "length", 1000);
    mlt_producer_set_in_and_out(producer, 0, 999);
    mlt_producer_set_speed(producer, 1);
    return producer;
}

static int case_export_out_of_order(void)
{
    mlt_profile profile = fixture_profile();
    struct ordering_state state = {.fail_at = 2, .code = 74, .gate = 1};
    mlt_producer producer = ordering_source(profile, &state);
    mlt_consumer consumer = fixture_consumer(profile, producer, -2, 1);
    mlt_properties props = MLT_CONSUMER_PROPERTIES(consumer);
    struct export_event ev = {.caller = pthread_self(),
                              .expected_position = 2,
                              .expected_code = 74};

    mlt_events_register(props, "consumer-video-error");
    mlt_events_listen(props, &ev, "consumer-video-error", (mlt_listener) on_video_error);

    TASK_CHECK(mlt_consumer_start(consumer) == 0);
    /* Frame 2 fails and completes first; frames 0 and 1 are held back by the
     * ordering gate. The caller must still see 0, 1 before the failure. */
    TASK_CHECK(consume_prefix(consumer, &ev, 2) == 0);
    TASK_CHECK(state.one_started == 1);
    TASK_CHECK(state.fail_done == 1);
    /* Frame 3 also failed (with a different code); the worker recorded it
     * first, but nothing has been reported to the caller yet and the
     * consumer properties are only published at the delivery point. */
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 0);
    TASK_CHECK(mlt_properties_get_position(props, "video_error_position") == 0);
    TASK_CHECK(ev.count == 0);
    if (finish_failure(consumer, &ev))
        return 1;

    /* The event must have fired on the rt_frame calling thread, not a worker. */
    TASK_CHECK(pthread_equal(ev.caller, pthread_self()));
    TASK_CHECK(atomic_load(&state.offthread) > 0);
    TASK_CHECK(mlt_frame_get_position(ev.retained) == 2);
    TASK_CHECK(mlt_properties_get_int(MLT_FRAME_PROPERTIES(ev.retained), "rendered") == 0);
    mlt_frame_close(ev.retained);

    TASK_CHECK(mlt_consumer_stop(consumer) == 0);

    /* Restart clears the previous failure and resumes delivery after seek. */
    mlt_producer_seek(producer, 0);
    state.fail_at = -1;
    state.gate = 0;
    state.fail_done = 0;
    state.one_started = 0;
    ev.retained = NULL;
    ev.count = 0;
    ev.valid = 0;
    mlt_properties_clear(props, "video_error");
    mlt_properties_clear(props, "video_error_position");
    TASK_CHECK(mlt_consumer_start(consumer) == 0);
    for (int n = 0; n < 3; ++n) {
        mlt_frame frame = mlt_consumer_rt_frame(consumer);
        TASK_CHECK(frame != NULL);
        TASK_CHECK(mlt_frame_get_position(frame) == n);
        TASK_CHECK(mlt_properties_get_int(MLT_FRAME_PROPERTIES(frame), "rendered") == 1);
        mlt_frame_close(frame);
    }
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 0);
    TASK_CHECK(mlt_consumer_stop(consumer) == 0);
    TASK_CHECK(ev.count == 0);

    mlt_consumer_close(consumer);
    mlt_producer_close(producer);
    TASK_CHECK(atomic_load(&state.created) == atomic_load(&state.destroyed));
    pthread_mutex_destroy(&state.mutex);
    pthread_cond_destroy(&state.cond);
    mlt_profile_close(profile);
    return 0;
}

static int case_export_video_off(void)
{
    mlt_profile profile = fixture_profile();
    struct source_state source = {.y = 80, .fail_at = 1, .code = 74};
    mlt_producer producer = fixture_source(profile, &source);
    mlt_consumer consumer = fixture_consumer(profile, producer, -2, 1);
    mlt_properties props = MLT_CONSUMER_PROPERTIES(consumer);
    struct export_event ev = {.caller = pthread_self(), .expected_position = 1,
                              .expected_code = 74};

    mlt_properties_set_int(props, "video_off", 1);
    mlt_events_register(props, "consumer-video-error");
    mlt_events_listen(props, &ev, "consumer-video-error", (mlt_listener) on_video_error);
    TASK_CHECK(mlt_consumer_start(consumer) == 0);
    for (int n = 0; n < 3; ++n) {
        mlt_frame frame = mlt_consumer_rt_frame(consumer);
        TASK_CHECK(frame != NULL);
        TASK_CHECK(mlt_frame_get_position(frame) == n);
        TASK_CHECK(mlt_properties_get_int(MLT_FRAME_PROPERTIES(frame), "rendered") == 1);
        mlt_frame_close(frame);
    }
    /* Strict video was never fetched, so the failing callback never ran. */
    TASK_CHECK(ev.count == 0);
    TASK_CHECK(atomic_load(&source.images) == 0);
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 0);
    TASK_CHECK(mlt_consumer_stop(consumer) == 0);
    mlt_consumer_close(consumer);
    mlt_producer_close(producer);
    TASK_CHECK(atomic_load(&source.created) == atomic_load(&source.destroyed));
    mlt_profile_close(profile);
    return 0;
}

static int case_export_restart_resets(void)
{
    mlt_profile profile = fixture_profile();
    struct source_state source = {.y = 80, .fail_at = 0, .code = 73};
    mlt_producer producer = fixture_source(profile, &source);
    mlt_consumer consumer = fixture_consumer(profile, producer, 0, 1);
    mlt_properties props = MLT_CONSUMER_PROPERTIES(consumer);
    struct export_event ev = {.caller = pthread_self(), .expected_position = 0,
                              .expected_code = 73};

    mlt_events_register(props, "consumer-video-error");
    mlt_events_listen(props, &ev, "consumer-video-error", (mlt_listener) on_video_error);
    TASK_CHECK(mlt_consumer_start(consumer) == 0);
    if (finish_failure(consumer, &ev))
        return 1;
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 73);
    mlt_frame_close(ev.retained);
    TASK_CHECK(mlt_consumer_stop(consumer) == 0);

    /* The caller may seek and adjust the media between exports. Start again
     * and the previous failure must be gone. */
    mlt_producer_seek(producer, 1);
    source.fail_at = -1;
    ev.retained = NULL;
    ev.count = 0;
    ev.valid = 0;
    TASK_CHECK(mlt_consumer_start(consumer) == 0);
    TASK_CHECK(mlt_properties_get_int(props, "video_error") == 0);
    TASK_CHECK(mlt_properties_get_int(props, "video_error_position") == 0);
    for (int i = 0; i < 3; ++i) {
        mlt_frame frame = mlt_consumer_rt_frame(consumer);
        TASK_CHECK(frame != NULL);
        TASK_CHECK(mlt_frame_get_position(frame) == 1 + i);
        TASK_CHECK(mlt_properties_get_int(MLT_FRAME_PROPERTIES(frame), "rendered") == 1);
        mlt_frame_close(frame);
    }
    TASK_CHECK(ev.count == 0);
    TASK_CHECK(mlt_consumer_stop(consumer) == 0);
    mlt_consumer_close(consumer);
    mlt_producer_close(producer);
    TASK_CHECK(atomic_load(&source.created) == atomic_load(&source.destroyed));
    mlt_profile_close(profile);
    return 0;
}

static int case_export_purge_close(void)
{
    /* An export left mid-flight must release queued frames and temporary
     * references on purge and close. */
    mlt_profile profile = fixture_profile();
    struct ordering_state state = {.fail_at = -1, .code = 1};
    mlt_producer producer = ordering_source(profile, &state);
    mlt_consumer consumer = fixture_consumer(profile, producer, -2, 1);

    TASK_CHECK(mlt_consumer_start(consumer) == 0);
    for (int i = 0; i < 2; ++i) {
        mlt_frame frame = mlt_consumer_rt_frame(consumer);
        TASK_CHECK(frame != NULL);
        mlt_frame_close(frame);
    }
    mlt_consumer_purge(consumer);
    TASK_CHECK(mlt_consumer_stop(consumer) == 0);
    mlt_consumer_close(consumer);
    mlt_producer_close(producer);
    TASK_CHECK(atomic_load(&state.created) == atomic_load(&state.destroyed));
    pthread_mutex_destroy(&state.mutex);
    pthread_cond_destroy(&state.cond);
    mlt_profile_close(profile);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
    {"image", "callback_failure", case_image_callback_failure},
    {"image", "convert_failure", case_image_convert_failure},
    {"image", "success_without_image", case_image_success_without_image},
    {"image", "blank_frame", case_image_blank_frame},
    {"image", "preview_fallback_kept", case_image_preview_fallback_kept},
    {"image", "normal_success", case_image_normal_success},
    {"graph", "luma_side_failure", case_graph_luma_side_failure},
    {"graph", "luma_normal_mix", case_graph_luma_normal_mix},
    {"graph", "black_not_failure", case_graph_black_is_not_failure},
    {"graph", "blank_test_image", case_graph_blank_test_image},
    {"export", "mode0_failure", case_export_mode0},
    {"export", "mode_minus1_failure", case_export_mode_minus1},
    {"export", "mode_minus2_failure", case_export_mode_minus2},
    {"export", "out_of_order", case_export_out_of_order},
    {"export", "video_off", case_export_video_off},
    {"export", "restart_resets", case_export_restart_resets},
    {"export", "purge_close", case_export_purge_close},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
