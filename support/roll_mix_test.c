/*
 * Regression tests for mlt_playlist_roll_mix.
 *
 * Builds real producer/cut/playlist/tractor graphs without any modules and
 * exercises rolling a mix created by mlt_playlist_mix() forward and backward,
 * including failure cases that must leave everything untouched.
 */

#include <framework/mlt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) \
    do { \
        if (!(x)) { \
            fprintf(stderr, "check failed line %d: %s\n", __LINE__, #x); \
            exit(1); \
        } \
    } while (0)

/* --------------------------------------------------------------------------
 * A source producer whose get_frame records the source id and the absolute
 * source frame number of every frame it produces.
 * ------------------------------------------------------------------------ */

#define LOG_CAPACITY 8192
static int g_log_id[LOG_CAPACITY];
static mlt_position g_log_frame[LOG_CAPACITY];
static int g_log_count;

static void log_reset(void)
{
    g_log_count = 0;
}

static int log_has(int id, mlt_position frame)
{
    int i;
    for (i = 0; i < g_log_count; i++)
        if (g_log_id[i] == id && g_log_frame[i] == frame)
            return 1;
    return 0;
}

static int source_get_frame(mlt_producer self, mlt_frame_ptr frame, int index)
{
    mlt_properties properties = MLT_PRODUCER_PROPERTIES(self);
    int id = mlt_properties_get_int(properties, "test.source_id");
    mlt_position source_frame = mlt_producer_frame(self);
    (void) index;
    if (g_log_count < LOG_CAPACITY) {
        g_log_id[g_log_count] = id;
        g_log_frame[g_log_count] = source_frame;
        g_log_count++;
    }
    *frame = mlt_frame_init(MLT_PRODUCER_SERVICE(self));
    mlt_frame_set_position(*frame, mlt_producer_position(self));
    mlt_properties_set_int(MLT_FRAME_PROPERTIES(*frame), "test.source_id", id);
    mlt_properties_set_position(MLT_FRAME_PROPERTIES(*frame), "test.source_frame", source_frame);
    mlt_producer_prepare_next(self);
    return 0;
}

static mlt_producer new_source(mlt_profile profile, int id, mlt_position length)
{
    mlt_producer self = mlt_producer_new(profile);
    mlt_properties properties = MLT_PRODUCER_PROPERTIES(self);
    mlt_properties_set_int(properties, "test.source_id", id);
    mlt_properties_set_position(properties, "length", length);
    mlt_producer_set_in_and_out(self, 0, length - 1);
    self->get_frame = source_get_frame;
    return self;
}

/* --------------------------------------------------------------------------
 * The standard scenario: A=[100,119], B=[200,229] with a 6 frame mix, so
 * left body [100,113], tracks [114,119] and [200,205], right body [206,229]
 * for a 44 frame program with the mix at program frame 14.
 * ------------------------------------------------------------------------ */

typedef struct
{
    mlt_profile profile;
    mlt_producer a;
    mlt_producer b;
    mlt_playlist playlist;
    mlt_producer left;  /* body cut of a, entry 0 */
    mlt_producer mix;   /* mix entry producer, entry 1 */
    mlt_producer right; /* body cut of b, entry 2 */
    mlt_tractor tractor;
    mlt_producer track_a;
    mlt_producer track_b;
} fixture;

static void fixture_open(fixture *f, mlt_position length_a, mlt_position length_b)
{
    memset(f, 0, sizeof(*f));
    f->profile = mlt_profile_init(NULL);
    CHECK(f->profile != NULL);
    f->a = new_source(f->profile, 1, length_a);
    f->b = new_source(f->profile, 2, length_b);
    f->playlist = mlt_playlist_new(f->profile);
    CHECK(f->a != NULL && f->b != NULL && f->playlist != NULL);
    CHECK(!mlt_playlist_append_io(f->playlist, f->a, 100, 119));
    CHECK(!mlt_playlist_append_io(f->playlist, f->b, 200, 229));
    CHECK(!mlt_playlist_mix(f->playlist, 0, 6, NULL));
    CHECK(mlt_playlist_count(f->playlist) == 3);
    CHECK(mlt_playlist_clip_is_mix(f->playlist, 1));
    f->left = mlt_playlist_get_clip(f->playlist, 0);
    f->mix = mlt_playlist_get_clip(f->playlist, 1);
    f->right = mlt_playlist_get_clip(f->playlist, 2);
    f->tractor = (mlt_tractor) mlt_producer_cut_parent(f->mix);
    f->track_a = mlt_tractor_get_track(f->tractor, 0);
    f->track_b = mlt_tractor_get_track(f->tractor, 1);
    CHECK(f->left != NULL && f->mix != NULL && f->right != NULL);
    CHECK(f->track_a != NULL && f->track_b != NULL);
}

static void fixture_close(fixture *f)
{
    mlt_playlist_close(f->playlist);
    mlt_producer_close(f->a);
    mlt_producer_close(f->b);
    mlt_profile_close(f->profile);
}

static void check_original_windows(fixture *f)
{
    CHECK(mlt_producer_get_in(f->left) == 100 && mlt_producer_get_out(f->left) == 113);
    CHECK(mlt_producer_get_in(f->track_a) == 114 && mlt_producer_get_out(f->track_a) == 119);
    CHECK(mlt_producer_get_in(f->track_b) == 200 && mlt_producer_get_out(f->track_b) == 205);
    CHECK(mlt_producer_get_in(f->right) == 206 && mlt_producer_get_out(f->right) == 229);
    CHECK(mlt_playlist_clip_start(f->playlist, 1) == 14);
    CHECK(mlt_playlist_clip_length(f->playlist, 0) == 14);
    CHECK(mlt_playlist_clip_length(f->playlist, 1) == 6);
    CHECK(mlt_playlist_clip_length(f->playlist, 2) == 24);
    CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(f->playlist)) == 44);
}

static void check_rolled_cut_windows(fixture *f)
{
    /* After +3: body [100,116], tracks [117,122] and [203,208], body [209,229]. */
    CHECK(mlt_producer_get_in(f->left) == 100 && mlt_producer_get_out(f->left) == 116);
    CHECK(mlt_producer_get_in(f->track_a) == 117 && mlt_producer_get_out(f->track_a) == 122);
    CHECK(mlt_producer_get_in(f->track_b) == 203 && mlt_producer_get_out(f->track_b) == 208);
    CHECK(mlt_producer_get_in(f->right) == 209 && mlt_producer_get_out(f->right) == 229);
}

static void check_rolled_windows(fixture *f)
{
    mlt_playlist_clip_info info;
    check_rolled_cut_windows(f);
    CHECK(mlt_playlist_clip_start(f->playlist, 0) == 0);
    CHECK(mlt_playlist_clip_start(f->playlist, 1) == 17);
    CHECK(mlt_playlist_clip_start(f->playlist, 2) == 23);
    CHECK(mlt_playlist_clip_length(f->playlist, 0) == 17);
    CHECK(mlt_playlist_clip_length(f->playlist, 1) == 6);
    CHECK(mlt_playlist_clip_length(f->playlist, 2) == 21);
    CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(f->playlist)) == 44);
    /* The entry cache must agree with the service windows. */
    CHECK(!mlt_playlist_get_clip_info(f->playlist, &info, 0));
    CHECK(info.frame_in == 100 && info.frame_out == 116 && info.frame_count == 17);
    CHECK(!mlt_playlist_get_clip_info(f->playlist, &info, 1));
    CHECK(info.frame_in == 0 && info.frame_out == 5 && info.frame_count == 6);
    CHECK(!mlt_playlist_get_clip_info(f->playlist, &info, 2));
    CHECK(info.frame_in == 209 && info.frame_out == 229 && info.frame_count == 21);
}

/* Pull one frame from the playlist at an absolute program position. */
static void pull_frame(mlt_playlist playlist, mlt_position position)
{
    mlt_producer producer = MLT_PLAYLIST_PRODUCER(playlist);
    mlt_frame frame = NULL;
    log_reset();
    CHECK(!mlt_producer_seek(producer, position));
    CHECK(!mlt_service_get_frame(MLT_PRODUCER_SERVICE(producer), &frame, 0));
    CHECK(frame != NULL);
    mlt_frame_close(frame);
}

/* --------------------------------------------------------------------------
 * Notification probes
 * ------------------------------------------------------------------------ */

static void count_notifications(mlt_properties owner, void *userdata, mlt_event_data data)
{
    int *count = (int *) userdata;
    (void) owner;
    (void) data;
    (*count)++;
}

typedef struct
{
    int count;
    int windows_final;
    int cache_final;
    fixture *f;
} notify_probe;

static void probe_notifications(mlt_properties owner, void *userdata, mlt_event_data data)
{
    notify_probe *probe = (notify_probe *) userdata;
    fixture *f = probe->f;
    mlt_playlist_clip_info info;
    (void) owner;
    (void) data;
    probe->count++;
    /* The four windows must already be final when the notification arrives. */
    probe->windows_final = mlt_producer_get_in(f->left) == 100
                           && mlt_producer_get_out(f->left) == 116
                           && mlt_producer_get_in(f->track_a) == 117
                           && mlt_producer_get_out(f->track_a) == 122
                           && mlt_producer_get_in(f->track_b) == 203
                           && mlt_producer_get_out(f->track_b) == 208
                           && mlt_producer_get_in(f->right) == 209
                           && mlt_producer_get_out(f->right) == 229;
    /* The cached clip values must agree. */
    probe->cache_final = !mlt_playlist_get_clip_info(f->playlist, &info, 0)
                         && info.frame_in == 100 && info.frame_out == 116
                         && info.frame_count == 17
                         && !mlt_playlist_get_clip_info(f->playlist, &info, 1)
                         && info.frame_in == 0 && info.frame_out == 5 && info.frame_count == 6
                         && !mlt_playlist_get_clip_info(f->playlist, &info, 2)
                         && info.frame_in == 209 && info.frame_out == 229
                         && info.frame_count == 21
                         && mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(f->playlist)) == 44;
}

/* --------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------ */

static void test_roll_forward(void)
{
    fixture f;
    mlt_producer mix_cut, multitrack;
    mlt_tractor tractor;
    mlt_field field;
    mlt_position tractor_in, tractor_out, mt_in, mt_out, mix_in, mix_out;

    fixture_open(&f, 1000, 1000);
    check_original_windows(&f);

    /* Remember the objects and local points that must survive the roll. */
    mix_cut = f.mix;
    tractor = f.tractor;
    field = mlt_tractor_field(tractor);
    multitrack = MLT_MULTITRACK_PRODUCER(mlt_tractor_multitrack(tractor));
    tractor_in = mlt_producer_get_in(MLT_TRACTOR_PRODUCER(tractor));
    tractor_out = mlt_producer_get_out(MLT_TRACTOR_PRODUCER(tractor));
    mt_in = mlt_producer_get_in(multitrack);
    mt_out = mlt_producer_get_out(multitrack);
    mix_in = mlt_producer_get_in(f.mix);
    mix_out = mlt_producer_get_out(f.mix);

    /* Park the play head at an absolute program frame. */
    CHECK(!mlt_producer_seek(MLT_PLAYLIST_PRODUCER(f.playlist), 20));
    CHECK(mlt_producer_position(MLT_PLAYLIST_PRODUCER(f.playlist)) == 20);

    CHECK(mlt_playlist_roll_mix(f.playlist, 1, 3) == 0);
    check_rolled_windows(&f);
    CHECK(mlt_playlist_clip_is_mix(f.playlist, 1));

    /* The play position keeps its absolute frame - no re-seek. */
    CHECK(mlt_producer_position(MLT_PLAYLIST_PRODUCER(f.playlist)) == 20);

    /* The tractor, field, and the four cuts keep their object identity. */
    CHECK(mlt_playlist_get_clip(f.playlist, 0) == f.left);
    CHECK(mlt_playlist_get_clip(f.playlist, 1) == mix_cut);
    CHECK(mlt_playlist_get_clip(f.playlist, 2) == f.right);
    CHECK((mlt_tractor) mlt_producer_cut_parent(mlt_playlist_get_clip(f.playlist, 1)) == tractor);
    CHECK(mlt_tractor_get_track(tractor, 0) == f.track_a);
    CHECK(mlt_tractor_get_track(tractor, 1) == f.track_b);
    CHECK(mlt_tractor_field(tractor) == field);

    /* The tractor, multitrack, and mix entry keep their local in/out. */
    CHECK(mlt_producer_get_in(MLT_TRACTOR_PRODUCER(tractor)) == tractor_in);
    CHECK(mlt_producer_get_out(MLT_TRACTOR_PRODUCER(tractor)) == tractor_out);
    CHECK(mlt_producer_get_in(multitrack) == mt_in);
    CHECK(mlt_producer_get_out(multitrack) == mt_out);
    CHECK(mlt_producer_get_in(f.mix) == mix_in);
    CHECK(mlt_producer_get_out(f.mix) == mix_out);

    /* The source parents are untouched. */
    CHECK(mlt_producer_get_in(f.a) == 0 && mlt_producer_get_out(f.a) == 999);
    CHECK(mlt_producer_get_length(f.a) == 1000);
    CHECK(mlt_producer_get_in(f.b) == 0 && mlt_producer_get_out(f.b) == 999);
    CHECK(mlt_producer_get_length(f.b) == 1000);

    /* The mix_in/mix_out links still point both ways. */
    CHECK(mlt_properties_get_data(MLT_TRACTOR_PROPERTIES(tractor), "mix_in", NULL) == f.left);
    CHECK(mlt_properties_get_data(MLT_TRACTOR_PROPERTIES(tractor), "mix_out", NULL) == f.right);
    CHECK(mlt_properties_get_data(MLT_PRODUCER_PROPERTIES(f.left), "mix_out", NULL) == tractor);
    CHECK(mlt_properties_get_data(MLT_PRODUCER_PROPERTIES(f.right), "mix_in", NULL) == tractor);

    /* Pull frames in shuffled order and check the recorded source frames. */
    pull_frame(f.playlist, 22); /* last mix frame: A 122 + B 208 */
    CHECK(g_log_count == 2 && log_has(1, 122) && log_has(2, 208));
    pull_frame(f.playlist, 0); /* first frame of the left body */
    CHECK(g_log_count == 1 && log_has(1, 100));
    pull_frame(f.playlist, 43); /* last program frame */
    CHECK(g_log_count == 1 && log_has(2, 229));
    pull_frame(f.playlist, 17); /* first mix frame: A 117 + B 203 */
    CHECK(g_log_count == 2 && log_has(1, 117) && log_has(2, 203));
    pull_frame(f.playlist, 16); /* last left body frame */
    CHECK(g_log_count == 1 && log_has(1, 116));
    pull_frame(f.playlist, 23); /* first right body frame */
    CHECK(g_log_count == 1 && log_has(2, 209));
    pull_frame(f.playlist, 30);
    CHECK(g_log_count == 1 && log_has(2, 216));
    pull_frame(f.playlist, 5);
    CHECK(g_log_count == 1 && log_has(1, 105));

    fixture_close(&f);
    printf("ok test_roll_forward\n");
}

static void test_roll_backward_and_restore(void)
{
    fixture f;
    fixture_open(&f, 1000, 1000);

    /* Roll backward: the mix moves from program frame 14 to 12. */
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, -2) == 0);
    CHECK(mlt_producer_get_in(f.left) == 100 && mlt_producer_get_out(f.left) == 111);
    CHECK(mlt_producer_get_in(f.track_a) == 112 && mlt_producer_get_out(f.track_a) == 117);
    CHECK(mlt_producer_get_in(f.track_b) == 198 && mlt_producer_get_out(f.track_b) == 203);
    CHECK(mlt_producer_get_in(f.right) == 204 && mlt_producer_get_out(f.right) == 229);
    CHECK(mlt_playlist_clip_start(f.playlist, 1) == 12);
    CHECK(mlt_playlist_clip_length(f.playlist, 0) == 12);
    CHECK(mlt_playlist_clip_length(f.playlist, 2) == 26);
    CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(f.playlist)) == 44);

    /* Rolling forward by the same amount restores the original layout. */
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, 2) == 0);
    check_original_windows(&f);

    /* Forward then backward restores as well. */
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, 3) == 0);
    check_rolled_windows(&f);
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, -3) == 0);
    check_original_windows(&f);

    /* Frame pulls see the restored windows. */
    pull_frame(f.playlist, 14); /* first mix frame again */
    CHECK(g_log_count == 2 && log_has(1, 114) && log_has(2, 200));
    pull_frame(f.playlist, 13);
    CHECK(g_log_count == 1 && log_has(1, 113));
    pull_frame(f.playlist, 20);
    CHECK(g_log_count == 1 && log_has(2, 206));

    fixture_close(&f);
    printf("ok test_roll_backward_and_restore\n");
}

static void test_delta_zero(void)
{
    fixture f;
    int count = 0;
    fixture_open(&f, 1000, 1000);
    mlt_events_listen(MLT_PLAYLIST_PROPERTIES(f.playlist),
                      &count,
                      "producer-changed",
                      (mlt_listener) count_notifications);

    /* A zero delta succeeds on a valid structure without any notification. */
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, 0) == 0);
    CHECK(count == 0);
    check_original_windows(&f);

    fixture_close(&f);
    printf("ok test_delta_zero\n");
}

static void test_delta_zero_broken(void)
{
    fixture f;
    fixture_open(&f, 1000, 1000);
    /* A zero delta still fails when the structure is broken. */
    mlt_properties_set_data(MLT_TRACTOR_PROPERTIES(f.tractor), "mix_in", NULL, 0, NULL, NULL);
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, 0) != 0);
    fixture_close(&f);
    printf("ok test_delta_zero_broken\n");
}

static void test_notification(void)
{
    fixture f;
    notify_probe probe;
    fixture_open(&f, 1000, 1000);
    memset(&probe, 0, sizeof(probe));
    probe.f = &f;
    mlt_events_listen(MLT_PLAYLIST_PROPERTIES(f.playlist),
                      &probe,
                      "producer-changed",
                      (mlt_listener) probe_notifications);

    CHECK(mlt_playlist_roll_mix(f.playlist, 1, 3) == 0);
    /* Exactly one notification, and the callback already saw the final state. */
    CHECK(probe.count == 1);
    CHECK(probe.windows_final);
    CHECK(probe.cache_final);

    fixture_close(&f);
    printf("ok test_notification\n");
}

static void test_media_margin(void)
{
    fixture f;
    int count = 0;

    /* Source A ends exactly at the mix: no headroom for a forward roll. */
    fixture_open(&f, 120, 1000);
    mlt_events_listen(MLT_PLAYLIST_PROPERTIES(f.playlist),
                      &count,
                      "producer-changed",
                      (mlt_listener) count_notifications);
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, 1) != 0);
    CHECK(count == 0);
    check_original_windows(&f);
    /* Backward still works and leaves margin behind. */
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, -2) == 0);
    CHECK(count == 1);
    CHECK(mlt_producer_get_out(f.left) == 111);
    CHECK(mlt_producer_get_in(f.track_a) == 112 && mlt_producer_get_out(f.track_a) == 117);
    fixture_close(&f);
    printf("ok test_media_margin\n");
}

static void test_body_limits(void)
{
    fixture f;
    fixture_open(&f, 1000, 1000);

    /* The left body has 14 frames: -13 leaves exactly one, -14 empties it. */
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, -14) != 0);
    check_original_windows(&f);
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, -13) == 0);
    CHECK(mlt_producer_get_in(f.left) == 100 && mlt_producer_get_out(f.left) == 100);
    CHECK(mlt_producer_get_in(f.track_a) == 101 && mlt_producer_get_out(f.track_a) == 106);
    CHECK(mlt_producer_get_in(f.track_b) == 187 && mlt_producer_get_out(f.track_b) == 192);
    CHECK(mlt_producer_get_in(f.right) == 193 && mlt_producer_get_out(f.right) == 229);
    CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(f.playlist)) == 44);
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, 13) == 0);
    check_original_windows(&f);

    /* The right body has 24 frames: +23 leaves exactly one, +24 empties it. */
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, 24) != 0);
    check_original_windows(&f);
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, 23) == 0);
    CHECK(mlt_producer_get_in(f.right) == 229 && mlt_producer_get_out(f.right) == 229);
    CHECK(mlt_producer_get_in(f.left) == 100 && mlt_producer_get_out(f.left) == 136);
    CHECK(mlt_producer_get_in(f.track_a) == 137 && mlt_producer_get_out(f.track_a) == 142);
    CHECK(mlt_producer_get_in(f.track_b) == 223 && mlt_producer_get_out(f.track_b) == 228);
    CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(f.playlist)) == 44);

    fixture_close(&f);
    printf("ok test_body_limits\n");
}

static void test_overflow(void)
{
    fixture f;
    int count = 0;
    fixture_open(&f, 1000, 1000);
    mlt_events_listen(MLT_PLAYLIST_PROPERTIES(f.playlist),
                      &count,
                      "producer-changed",
                      (mlt_listener) count_notifications);

    /* Deltas that cannot be represented as mlt_position sums must fail. */
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, INT32_MAX) != 0);
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, INT32_MIN) != 0);
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, INT32_MAX - 100) != 0);
    CHECK(count == 0);
    check_original_windows(&f);

    fixture_close(&f);
    printf("ok test_overflow\n");
}

/* Snapshot the four windows after corrupting the structure, then verify that
 * failed rolls change nothing and fire no notification. */
static void break_and_check(void (*breaker)(fixture *))
{
    fixture f;
    int count = 0;
    mlt_position w[8];
    mlt_position total;
    int i;

    fixture_open(&f, 1000, 1000);
    mlt_events_listen(MLT_PLAYLIST_PROPERTIES(f.playlist),
                      &count,
                      "producer-changed",
                      (mlt_listener) count_notifications);
    breaker(&f);

    /* The breaker itself may legitimately fire notifications; only the
     * failed rolls that follow must stay silent. */
    count = 0;
    w[0] = mlt_producer_get_in(f.left);
    w[1] = mlt_producer_get_out(f.left);
    w[2] = mlt_producer_get_in(f.track_a);
    w[3] = mlt_producer_get_out(f.track_a);
    w[4] = mlt_producer_get_in(f.track_b);
    w[5] = mlt_producer_get_out(f.track_b);
    w[6] = mlt_producer_get_in(f.right);
    w[7] = mlt_producer_get_out(f.right);
    total = mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(f.playlist));

    CHECK(mlt_playlist_roll_mix(f.playlist, 1, 1) != 0);
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, -1) != 0);
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, 0) != 0);
    CHECK(count == 0);

    CHECK(mlt_producer_get_in(f.left) == w[0] && mlt_producer_get_out(f.left) == w[1]);
    CHECK(mlt_producer_get_in(f.track_a) == w[2] && mlt_producer_get_out(f.track_a) == w[3]);
    CHECK(mlt_producer_get_in(f.track_b) == w[4] && mlt_producer_get_out(f.track_b) == w[5]);
    CHECK(mlt_producer_get_in(f.right) == w[6] && mlt_producer_get_out(f.right) == w[7]);
    CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(f.playlist)) == total);
    for (i = 0; i < 3; i++)
        CHECK(mlt_playlist_get_clip(f.playlist, i) != NULL);

    fixture_close(&f);
}

static void break_mix_in(fixture *f)
{
    mlt_properties_set_data(MLT_TRACTOR_PROPERTIES(f->tractor), "mix_in", NULL, 0, NULL, NULL);
}

static void break_mix_out(fixture *f)
{
    mlt_properties_set_data(MLT_TRACTOR_PROPERTIES(f->tractor), "mix_out", NULL, 0, NULL, NULL);
}

static void break_body_mix_out(fixture *f)
{
    mlt_properties_set_data(MLT_PRODUCER_PROPERTIES(f->left), "mix_out", NULL, 0, NULL, NULL);
}

static void break_body_mix_in(fixture *f)
{
    mlt_properties_set_data(MLT_PRODUCER_PROPERTIES(f->right), "mix_in", NULL, 0, NULL, NULL);
}

static void break_wrong_mix_in(fixture *f)
{
    /* Point the tractor at the wrong body producer. */
    mlt_properties_set_data(MLT_TRACTOR_PROPERTIES(f->tractor),
                            "mix_in",
                            f->right,
                            0,
                            NULL,
                            NULL);
}

static void break_marker(fixture *f)
{
    mlt_properties_set_data(MLT_TRACTOR_PROPERTIES(f->tractor), "mlt_mix", NULL, 0, NULL, NULL);
}

static void break_marker_target(fixture *f)
{
    /* The mlt_mix marker no longer references the tractor itself. */
    mlt_properties_set_data(MLT_TRACTOR_PROPERTIES(f->tractor),
                            "mlt_mix",
                            f->left,
                            0,
                            NULL,
                            NULL);
}

static void break_repeat(fixture *f)
{
    CHECK(!mlt_playlist_repeat_clip(f->playlist, 0, 2));
}

static void break_contiguity(fixture *f)
{
    /* Open a gap between the right track and the right body. */
    mlt_producer_set_in_and_out(f->right, 207, 229);
}

static void test_broken_links(void)
{
    break_and_check(break_mix_in);
    break_and_check(break_mix_out);
    break_and_check(break_body_mix_out);
    break_and_check(break_body_mix_in);
    break_and_check(break_wrong_mix_in);
    break_and_check(break_marker);
    break_and_check(break_marker_target);
    break_and_check(break_repeat);
    break_and_check(break_contiguity);
    printf("ok test_broken_links\n");
}

static void test_duplicate_entries(void)
{
    fixture f;
    playlist_entry *saved;
    fixture_open(&f, 1000, 1000);

    /* Two playlist slots sharing one entry must be rejected. */
    saved = f.playlist->list[0];
    f.playlist->list[0] = f.playlist->list[2];
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, 1) != 0);
    f.playlist->list[0] = saved;

    check_original_windows(&f);
    fixture_close(&f);
    printf("ok test_duplicate_entries\n");
}

static void test_shared_body_cut(void)
{
    /* Both body entries reference the same cut: a duplicate that must fail. */
    mlt_profile profile = mlt_profile_init(NULL);
    mlt_producer a = new_source(profile, 1, 1000);
    mlt_playlist p = mlt_playlist_new(profile);
    mlt_producer cut = mlt_producer_cut(a, 100, 119);
    CHECK(!mlt_playlist_append(p, cut));
    CHECK(!mlt_playlist_append(p, cut));
    CHECK(!mlt_playlist_mix(p, 0, 6, NULL));
    CHECK(mlt_playlist_count(p) == 3);
    CHECK(mlt_playlist_get_clip(p, 0) == cut);
    CHECK(mlt_playlist_get_clip(p, 2) == cut);
    CHECK(mlt_playlist_roll_mix(p, 1, 1) != 0);
    CHECK(mlt_playlist_roll_mix(p, 1, -1) != 0);
    mlt_playlist_close(p);
    mlt_producer_close(cut);
    mlt_producer_close(a);
    mlt_profile_close(profile);
    printf("ok test_shared_body_cut\n");
}

static void test_blank_neighbor(void)
{
    fixture f;
    mlt_producer blank;
    mlt_properties blank_props;
    fixture_open(&f, 1000, 1000);

    /* Inject a blank producer so the playlist can create blank entries
     * without the factory (no modules are built in this configuration). */
    blank = mlt_producer_new(f.profile);
    blank_props = MLT_PRODUCER_PROPERTIES(blank);
    mlt_properties_set(blank_props, "resource", "blank");
    mlt_properties_set_position(blank_props, "length", 1000);
    mlt_producer_set_in_and_out(blank, 0, 999);
    mlt_properties_set_data(MLT_PLAYLIST_PROPERTIES(f.playlist), "_blank", blank, 0, NULL, NULL);

    /* [left body, blank, mix, right body] */
    mlt_playlist_insert_blank(f.playlist, 1, 4);
    CHECK(mlt_playlist_count(f.playlist) == 4);
    CHECK(mlt_playlist_is_blank(f.playlist, 1));

    /* The mix is now at index 2 with a blank left neighbor. */
    CHECK(mlt_playlist_roll_mix(f.playlist, 2, 1) != 0);
    CHECK(mlt_playlist_roll_mix(f.playlist, 2, -1) != 0);
    CHECK(mlt_playlist_roll_mix(f.playlist, 2, 0) != 0);

    /* The mix windows are untouched. */
    CHECK(mlt_producer_get_out(f.left) == 113);
    CHECK(mlt_producer_get_in(f.track_a) == 114 && mlt_producer_get_out(f.track_a) == 119);
    CHECK(mlt_producer_get_in(f.track_b) == 200 && mlt_producer_get_out(f.track_b) == 205);
    CHECK(mlt_producer_get_in(f.right) == 206);

    mlt_producer_close(blank);
    fixture_close(&f);
    printf("ok test_blank_neighbor\n");
}

static void test_missing_side(void)
{
    fixture f;
    fixture_open(&f, 1000, 1000);

    /* Index errors: no left body, no mix at the index, out of range. */
    CHECK(mlt_playlist_roll_mix(f.playlist, 0, 1) != 0);
    CHECK(mlt_playlist_roll_mix(f.playlist, 2, 1) != 0);
    CHECK(mlt_playlist_roll_mix(f.playlist, 3, 1) != 0);
    CHECK(mlt_playlist_roll_mix(f.playlist, -1, 1) != 0);
    CHECK(mlt_playlist_roll_mix(f.playlist, 100, 1) != 0);
    CHECK(mlt_playlist_roll_mix(NULL, 1, 1) != 0);
    check_original_windows(&f);

    /* Remove the right body: the mix is left without a side. */
    CHECK(!mlt_playlist_remove(f.playlist, 2));
    CHECK(mlt_playlist_count(f.playlist) == 2);
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, 1) != 0);
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, 0) != 0);

    fixture_close(&f);
    printf("ok test_missing_side\n");
}

static void test_plain_tractor_is_not_a_mix(void)
{
    mlt_profile profile = mlt_profile_init(NULL);
    mlt_producer a = new_source(profile, 1, 1000);
    mlt_producer b = new_source(profile, 2, 1000);
    mlt_playlist p = mlt_playlist_new(profile);
    mlt_tractor t = mlt_tractor_new();
    mlt_producer cut_a = mlt_producer_cut(a, 114, 119);
    mlt_producer cut_b = mlt_producer_cut(b, 200, 205);
    mlt_producer entry;

    CHECK(!mlt_playlist_append_io(p, a, 100, 119));
    CHECK(!mlt_playlist_append_io(p, b, 200, 229));

    /* A hand-built tractor between two clips is not a rollable mix. */
    mlt_service_set_profile(MLT_TRACTOR_SERVICE(t),
                            mlt_service_profile(MLT_PLAYLIST_SERVICE(p)));
    CHECK(!mlt_tractor_set_track(t, cut_a, 0));
    CHECK(!mlt_tractor_set_track(t, cut_b, 1));
    CHECK(!mlt_playlist_insert(p, MLT_TRACTOR_PRODUCER(t), 1, -1, -1));
    mlt_producer_close(cut_a);
    mlt_producer_close(cut_b);
    mlt_tractor_close(t);
    CHECK(mlt_playlist_count(p) == 3);

    CHECK(mlt_playlist_roll_mix(p, 1, 1) != 0);
    CHECK(mlt_playlist_roll_mix(p, 1, -1) != 0);
    CHECK(mlt_playlist_roll_mix(p, 1, 0) != 0);

    entry = mlt_playlist_get_clip(p, 0);
    CHECK(mlt_producer_get_in(entry) == 100 && mlt_producer_get_out(entry) == 119);
    entry = mlt_playlist_get_clip(p, 2);
    CHECK(mlt_producer_get_in(entry) == 200 && mlt_producer_get_out(entry) == 229);

    mlt_playlist_close(p);
    mlt_producer_close(a);
    mlt_producer_close(b);
    mlt_profile_close(profile);
    printf("ok test_plain_tractor_is_not_a_mix\n");
}

static void test_track_not_a_cut(void)
{
    fixture f;
    fixture_open(&f, 1000, 1000);

    /* Replace track 0 with the raw source: no longer two ordinary cuts. */
    CHECK(!mlt_tractor_set_track(f.tractor, f.a, 0));
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, 1) != 0);
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, -1) != 0);
    CHECK(mlt_producer_get_out(f.left) == 113);
    CHECK(mlt_producer_get_in(f.track_b) == 200 && mlt_producer_get_out(f.track_b) == 205);
    CHECK(mlt_producer_get_in(f.right) == 206);

    fixture_close(&f);
    printf("ok test_track_not_a_cut\n");
}

static void test_track_count(void)
{
    fixture f;
    mlt_producer extra;
    fixture_open(&f, 1000, 1000);

    /* A third track breaks the exactly-two-tracks rule. */
    extra = mlt_producer_cut(f.a, 500, 505);
    CHECK(!mlt_tractor_set_track(f.tractor, extra, 2));
    mlt_producer_close(extra);
    CHECK(mlt_playlist_roll_mix(f.playlist, 1, 1) != 0);
    CHECK(mlt_producer_get_out(f.left) == 113);
    CHECK(mlt_producer_get_in(f.right) == 206);

    fixture_close(&f);
    printf("ok test_track_count\n");
}

static void test_playlist_parent_rejected(void)
{
    mlt_profile profile = mlt_profile_init(NULL);
    mlt_producer a = new_source(profile, 1, 1000);
    mlt_producer b = new_source(profile, 2, 1000);
    mlt_playlist inner = mlt_playlist_new(profile);
    mlt_playlist p = mlt_playlist_new(profile);

    /* The left clip is a cut of another playlist. */
    CHECK(!mlt_playlist_append_io(inner, a, 100, 119));
    CHECK(!mlt_playlist_append_io(p, MLT_PLAYLIST_PRODUCER(inner), 0, 19));
    CHECK(!mlt_playlist_append_io(p, b, 200, 229));
    CHECK(!mlt_playlist_mix(p, 0, 6, NULL));
    CHECK(mlt_playlist_count(p) == 3);
    CHECK(mlt_playlist_roll_mix(p, 1, 1) != 0);
    CHECK(mlt_playlist_roll_mix(p, 1, -1) != 0);

    mlt_playlist_close(p);
    mlt_playlist_close(inner);
    mlt_producer_close(a);
    mlt_producer_close(b);
    mlt_profile_close(profile);
    printf("ok test_playlist_parent_rejected\n");
}

static void test_tractor_parent_rejected(void)
{
    mlt_profile profile = mlt_profile_init(NULL);
    mlt_producer a = new_source(profile, 1, 1000);
    mlt_producer b = new_source(profile, 2, 1000);
    mlt_playlist p = mlt_playlist_new(profile);
    mlt_tractor t2 = mlt_tractor_new();
    mlt_producer track = mlt_producer_cut(b, 200, 229);

    /* The right clip is a cut of a tractor. */
    mlt_service_set_profile(MLT_TRACTOR_SERVICE(t2), profile);
    CHECK(!mlt_tractor_set_track(t2, track, 0));
    mlt_producer_close(track);
    CHECK(!mlt_playlist_append_io(p, a, 100, 119));
    CHECK(!mlt_playlist_append_io(p, MLT_TRACTOR_PRODUCER(t2), 0, 29));
    CHECK(!mlt_playlist_mix(p, 0, 6, NULL));
    CHECK(mlt_playlist_count(p) == 3);
    CHECK(mlt_playlist_roll_mix(p, 1, 1) != 0);
    CHECK(mlt_playlist_roll_mix(p, 1, -1) != 0);

    mlt_playlist_close(p);
    mlt_tractor_close(t2);
    mlt_producer_close(a);
    mlt_producer_close(b);
    mlt_profile_close(profile);
    printf("ok test_tractor_parent_rejected\n");
}

static void test_same_source_reuse(void)
{
    fixture f;
    mlt_producer extra_a, extra_b;
    fixture_open(&f, 1000, 1000);

    /* Cut the same sources in elsewhere on the same playlist. */
    CHECK(!mlt_playlist_append_io(f.playlist, f.a, 300, 309));
    CHECK(!mlt_playlist_append_io(f.playlist, f.b, 400, 409));
    extra_a = mlt_playlist_get_clip(f.playlist, 3);
    extra_b = mlt_playlist_get_clip(f.playlist, 4);
    CHECK(mlt_producer_cut_parent(extra_a) == f.a);
    CHECK(mlt_producer_cut_parent(extra_b) == f.b);

    CHECK(mlt_playlist_roll_mix(f.playlist, 1, 3) == 0);
    check_rolled_cut_windows(&f);
    CHECK(mlt_playlist_clip_length(f.playlist, 0) == 17);
    CHECK(mlt_playlist_clip_length(f.playlist, 1) == 6);
    CHECK(mlt_playlist_clip_length(f.playlist, 2) == 21);

    /* The other cuts and the parents are unaffected. */
    CHECK(mlt_producer_get_in(extra_a) == 300 && mlt_producer_get_out(extra_a) == 309);
    CHECK(mlt_producer_get_in(extra_b) == 400 && mlt_producer_get_out(extra_b) == 409);
    CHECK(mlt_producer_get_in(f.a) == 0 && mlt_producer_get_out(f.a) == 999);
    CHECK(mlt_producer_get_length(f.a) == 1000);
    CHECK(mlt_producer_get_in(f.b) == 0 && mlt_producer_get_out(f.b) == 999);
    CHECK(mlt_producer_get_length(f.b) == 1000);
    CHECK(mlt_playlist_clip_length(f.playlist, 3) == 10);
    CHECK(mlt_playlist_clip_length(f.playlist, 4) == 10);
    CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(f.playlist)) == 64);

    fixture_close(&f);
    printf("ok test_same_source_reuse\n");
}

static void test_shared_source(void)
{
    /* Both sides of the mix come from the same source parent. */
    mlt_profile profile = mlt_profile_init(NULL);
    mlt_producer a = new_source(profile, 1, 1000);
    mlt_playlist p = mlt_playlist_new(profile);
    mlt_producer mix, left, right;
    mlt_tractor t;

    CHECK(!mlt_playlist_append_io(p, a, 100, 119));
    CHECK(!mlt_playlist_append_io(p, a, 200, 219));
    CHECK(!mlt_playlist_mix(p, 0, 6, NULL));
    mix = mlt_playlist_get_clip(p, 1);
    t = (mlt_tractor) mlt_producer_cut_parent(mix);
    left = mlt_playlist_get_clip(p, 0);
    right = mlt_playlist_get_clip(p, 2);
    CHECK(mlt_producer_cut_parent(mlt_tractor_get_track(t, 0)) == a);
    CHECK(mlt_producer_cut_parent(mlt_tractor_get_track(t, 1)) == a);
    CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(p)) == 34);

    CHECK(mlt_playlist_roll_mix(p, 1, 2) == 0);
    CHECK(mlt_producer_get_in(left) == 100 && mlt_producer_get_out(left) == 115);
    CHECK(mlt_producer_get_in(mlt_tractor_get_track(t, 0)) == 116);
    CHECK(mlt_producer_get_out(mlt_tractor_get_track(t, 0)) == 121);
    CHECK(mlt_producer_get_in(mlt_tractor_get_track(t, 1)) == 202);
    CHECK(mlt_producer_get_out(mlt_tractor_get_track(t, 1)) == 207);
    CHECK(mlt_producer_get_in(right) == 208 && mlt_producer_get_out(right) == 219);
    CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(p)) == 34);

    CHECK(mlt_playlist_roll_mix(p, 1, -2) == 0);
    CHECK(mlt_producer_get_out(left) == 113);
    CHECK(mlt_producer_get_in(right) == 206);
    CHECK(mlt_producer_get_in(a) == 0 && mlt_producer_get_out(a) == 999);

    mlt_playlist_close(p);
    mlt_producer_close(a);
    mlt_profile_close(profile);
    printf("ok test_shared_source\n");
}

static void test_adjacent_mixes(void)
{
    mlt_profile profile = mlt_profile_init(NULL);
    mlt_producer a = new_source(profile, 1, 1000);
    mlt_producer b = new_source(profile, 2, 1000);
    mlt_producer c = new_source(profile, 3, 1000);
    mlt_playlist p = mlt_playlist_new(profile);
    mlt_producer mix1, mix2, body_a, body_b, body_c;
    mlt_tractor t1, t2;
    mlt_producer t1a, t1b, t2a, t2b;

    CHECK(!mlt_playlist_append_io(p, a, 100, 119));
    CHECK(!mlt_playlist_append_io(p, b, 200, 229));
    CHECK(!mlt_playlist_append_io(p, c, 300, 319));
    CHECK(!mlt_playlist_mix(p, 0, 6, NULL));
    CHECK(!mlt_playlist_mix(p, 2, 4, NULL));
    CHECK(mlt_playlist_count(p) == 5);

    mix1 = mlt_playlist_get_clip(p, 1);
    mix2 = mlt_playlist_get_clip(p, 3);
    t1 = (mlt_tractor) mlt_producer_cut_parent(mix1);
    t2 = (mlt_tractor) mlt_producer_cut_parent(mix2);
    t1a = mlt_tractor_get_track(t1, 0);
    t1b = mlt_tractor_get_track(t1, 1);
    t2a = mlt_tractor_get_track(t2, 0);
    t2b = mlt_tractor_get_track(t2, 1);
    body_a = mlt_playlist_get_clip(p, 0);
    body_b = mlt_playlist_get_clip(p, 2);
    body_c = mlt_playlist_get_clip(p, 4);

    /* mix1 = A[114,119]+B[200,205], mix2 = B[226,229]+C[300,303]. */
    CHECK(mlt_producer_get_in(t1a) == 114 && mlt_producer_get_out(t1a) == 119);
    CHECK(mlt_producer_get_in(t1b) == 200 && mlt_producer_get_out(t1b) == 205);
    CHECK(mlt_producer_get_in(t2a) == 226 && mlt_producer_get_out(t2a) == 229);
    CHECK(mlt_producer_get_in(t2b) == 300 && mlt_producer_get_out(t2b) == 303);
    CHECK(mlt_producer_get_in(body_b) == 206 && mlt_producer_get_out(body_b) == 225);
    CHECK(mlt_producer_get_in(body_c) == 304 && mlt_producer_get_out(body_c) == 319);
    CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(p)) == 60);

    /* Roll the first mix forward; the second mix must not move. */
    CHECK(mlt_playlist_roll_mix(p, 1, 3) == 0);
    CHECK(mlt_producer_get_out(body_a) == 116);
    CHECK(mlt_producer_get_in(t1a) == 117 && mlt_producer_get_out(t1a) == 122);
    CHECK(mlt_producer_get_in(t1b) == 203 && mlt_producer_get_out(t1b) == 208);
    CHECK(mlt_producer_get_in(body_b) == 209 && mlt_producer_get_out(body_b) == 225);
    CHECK(mlt_producer_get_in(t2a) == 226 && mlt_producer_get_out(t2a) == 229);
    CHECK(mlt_producer_get_in(t2b) == 300 && mlt_producer_get_out(t2b) == 303);
    CHECK(mlt_producer_get_in(body_c) == 304 && mlt_producer_get_out(body_c) == 319);
    CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(p)) == 60);
    CHECK(mlt_properties_get_data(MLT_TRACTOR_PROPERTIES(t2), "mix_in", NULL) == body_b);
    CHECK(mlt_properties_get_data(MLT_TRACTOR_PROPERTIES(t2), "mix_out", NULL) == body_c);

    /* Roll the second mix backward; the first mix must not move. */
    CHECK(mlt_playlist_roll_mix(p, 3, -2) == 0);
    CHECK(mlt_producer_get_in(body_b) == 209 && mlt_producer_get_out(body_b) == 223);
    CHECK(mlt_producer_get_in(t2a) == 224 && mlt_producer_get_out(t2a) == 227);
    CHECK(mlt_producer_get_in(t2b) == 298 && mlt_producer_get_out(t2b) == 301);
    CHECK(mlt_producer_get_in(body_c) == 302 && mlt_producer_get_out(body_c) == 319);
    CHECK(mlt_producer_get_out(body_a) == 116);
    CHECK(mlt_producer_get_in(t1a) == 117 && mlt_producer_get_out(t1a) == 122);
    CHECK(mlt_producer_get_in(t1b) == 203 && mlt_producer_get_out(t1b) == 208);
    CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(p)) == 60);
    CHECK(mlt_properties_get_data(MLT_TRACTOR_PROPERTIES(t1), "mix_in", NULL) == body_a);
    CHECK(mlt_properties_get_data(MLT_TRACTOR_PROPERTIES(t1), "mix_out", NULL) == body_b);

    mlt_playlist_close(p);
    mlt_producer_close(a);
    mlt_producer_close(b);
    mlt_producer_close(c);
    mlt_profile_close(profile);
    printf("ok test_adjacent_mixes\n");
}

static void test_transition_identity(void)
{
    mlt_profile profile = mlt_profile_init(NULL);
    mlt_producer a = new_source(profile, 1, 1000);
    mlt_producer b = new_source(profile, 2, 1000);
    mlt_playlist p = mlt_playlist_new(profile);
    mlt_transition transition = mlt_transition_new();
    mlt_properties transition_props = MLT_TRANSITION_PROPERTIES(transition);
    mlt_producer mix, left;
    mlt_tractor t;
    mlt_field field;
    const char *curve;

    CHECK(!mlt_playlist_append_io(p, a, 100, 119));
    CHECK(!mlt_playlist_append_io(p, b, 200, 229));
    mlt_properties_set(transition_props, "curve", "0=0;5=100");
    mlt_properties_set_int(transition_props, "custom_flag", 42);
    CHECK(!mlt_playlist_mix(p, 0, 6, transition));
    mix = mlt_playlist_get_clip(p, 1);
    t = (mlt_tractor) mlt_producer_cut_parent(mix);
    field = mlt_tractor_field(t);
    /* The mix owns a reference now; drop ours. */
    mlt_transition_close(transition);
    CHECK(mlt_transition_get_in(transition) == 0 && mlt_transition_get_out(transition) == 5);

    CHECK(mlt_playlist_roll_mix(p, 1, 3) == 0);

    /* The same transition object is still attached with its settings intact. */
    CHECK(mlt_tractor_field(t) == field);
    CHECK(mlt_service_producer(MLT_TRACTOR_SERVICE(t)) == MLT_TRANSITION_SERVICE(transition));
    CHECK(mlt_transition_get_in(transition) == 0 && mlt_transition_get_out(transition) == 5);
    CHECK(mlt_transition_get_a_track(transition) == 0);
    CHECK(mlt_transition_get_b_track(transition) == 1);
    CHECK(mlt_properties_get_int(transition_props, "custom_flag") == 42);
    curve = mlt_properties_get(transition_props, "curve");
    CHECK(curve != NULL && !strcmp(curve, "0=0;5=100"));

    /* The windows moved as usual. */
    left = mlt_playlist_get_clip(p, 0);
    CHECK(mlt_producer_get_out(left) == 116);
    CHECK(mlt_producer_get_in(mlt_tractor_get_track(t, 0)) == 117);
    CHECK(mlt_producer_get_out(mlt_tractor_get_track(t, 0)) == 122);

    mlt_playlist_close(p);
    mlt_producer_close(a);
    mlt_producer_close(b);
    mlt_profile_close(profile);
    printf("ok test_transition_identity\n");
}

int main(void)
{
    test_roll_forward();
    test_roll_backward_and_restore();
    test_delta_zero();
    test_delta_zero_broken();
    test_notification();
    test_media_margin();
    test_body_limits();
    test_overflow();
    test_broken_links();
    test_duplicate_entries();
    test_shared_body_cut();
    test_blank_neighbor();
    test_missing_side();
    test_plain_tractor_is_not_a_mix();
    test_track_not_a_cut();
    test_track_count();
    test_playlist_parent_rejected();
    test_tractor_parent_rejected();
    test_same_source_reuse();
    test_shared_source();
    test_adjacent_mixes();
    test_transition_identity();
    mlt_pool_close();
    printf("MLT_ROLL_MIX_OK\n");
    return 0;
}
