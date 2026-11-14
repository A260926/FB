/*
 * roll_mix_test.c - regression tests for mlt_playlist_roll_mix()
 *
 * Copyright (C) 2026 Meltytech, LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <framework/mlt.h>

#include <limits.h>
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

/* A producer get_frame callback that stamps every frame with the source id and
 * the absolute frame number of the underlying source producer. Cuts seek the
 * shared parent before pulling the frame, so the stamp reflects the real source
 * window rather than a computed endpoint. The per-id property survives a mix
 * tractor merging both tracks into one frame, while the shared properties are
 * convenient when a track cut is pulled directly. */

static int source_get_frame(mlt_producer producer, mlt_frame_ptr frame, int index)
{
    (void) index;
    mlt_producer root = mlt_producer_cut_parent(producer);
    *frame = mlt_frame_init(MLT_PRODUCER_SERVICE(producer));
    CHECK(*frame != NULL);
    int source_id = mlt_properties_get_int(MLT_PRODUCER_PROPERTIES(root), "test.source_id");
    mlt_position source_frame = mlt_producer_frame(root);
    mlt_frame_set_position(*frame, source_frame);
    mlt_properties frame_props = MLT_FRAME_PROPERTIES(*frame);
    mlt_properties_set_int(frame_props, "meta.test_source_id", source_id);
    mlt_properties_set_position(frame_props, "meta.test_source_frame", source_frame);
    char key[32];
    snprintf(key, sizeof(key), "meta.src.%d", source_id);
    mlt_properties_set_position(frame_props, key, source_frame);
    return 0;
}

static mlt_producer make_source(mlt_profile profile, int id, mlt_position length)
{
    mlt_producer producer = mlt_producer_new(profile);
    CHECK(producer != NULL);
    mlt_properties_set_int(MLT_PRODUCER_PROPERTIES(producer), "test.source_id", id);
    mlt_properties_set_position(MLT_PRODUCER_PROPERTIES(producer), "length", length);
    mlt_producer_set_in_and_out(producer, 0, length - 1);
    producer->get_frame = source_get_frame;
    return producer;
}

/* Minimal stand-in for the core module's blank producer. The regression build
 * loads no service modules, so blank requests are answered through the factory
 * event hook instead. */

static mlt_profile test_profile;

static int blank_get_frame(mlt_producer producer, mlt_frame_ptr frame, int index)
{
    (void) index;
    *frame = mlt_frame_init(MLT_PRODUCER_SERVICE(producer));
    CHECK(*frame != NULL);
    mlt_frame_set_position(*frame, mlt_producer_position(producer));
    return 0;
}

static void on_producer_create_request(mlt_properties owner, void *data, mlt_event_data event_data)
{
    (void) owner;
    (void) data;
    mlt_factory_event_data *request = (mlt_factory_event_data *) mlt_event_data_to_object(event_data);
    /* mlt_factory_producer() passes the address of its local producer in the
     * "service" field, so a listener creates the producer and writes through
     * that pointer. */
    if (request != NULL && request->input != NULL
        && !strcmp((const char *) request->input, "blank") && request->service != NULL) {
        mlt_producer *slot = (mlt_producer *) request->service;
        if (*slot == NULL) {
            mlt_producer blank = mlt_producer_new(test_profile);
            CHECK(blank != NULL);
            mlt_properties_set(MLT_PRODUCER_PROPERTIES(blank), "resource", "blank");
            mlt_properties_set(MLT_PRODUCER_PROPERTIES(blank), "mlt_service", "blank");
            blank->get_frame = blank_get_frame;
            *slot = blank;
        }
    }
}

/* Pull one program frame through the playlist and read the source stamps.
 * Inside the mix both source tracks are merged, so per-id keys carry both
 * windows; src_a and src_b are set to -1 when that source is not present. */

static void fetch_program(mlt_playlist playlist,
                          mlt_position program_frame,
                          mlt_position *src_a,
                          mlt_position *src_b)
{
    mlt_producer playlist_producer = MLT_PLAYLIST_PRODUCER(playlist);
    mlt_producer_seek(playlist_producer, program_frame);
    mlt_frame frame = NULL;
    CHECK(mlt_service_get_frame(MLT_PRODUCER_SERVICE(playlist_producer), &frame, 0) == 0);
    CHECK(frame != NULL);
    mlt_properties props = MLT_FRAME_PROPERTIES(frame);
    *src_a = mlt_properties_exists(props, "meta.src.1")
                 ? mlt_properties_get_position(props, "meta.src.1")
                 : -1;
    *src_b = mlt_properties_exists(props, "meta.src.2")
                 ? mlt_properties_get_position(props, "meta.src.2")
                 : -1;
    mlt_frame_close(frame);
}

/* Pull a frame from one of the mix tractor's track cuts and read its stamps. */

static void fetch_track(mlt_producer cut,
                        mlt_position local_frame,
                        int *source_id,
                        mlt_position *source_frame)
{
    mlt_producer_seek(cut, local_frame);
    mlt_frame frame = NULL;
    CHECK(mlt_service_get_frame(MLT_PRODUCER_SERVICE(cut), &frame, 0) == 0);
    CHECK(frame != NULL);
    mlt_properties props = MLT_FRAME_PROPERTIES(frame);
    *source_id = mlt_properties_get_int(props, "meta.test_source_id");
    *source_frame = mlt_properties_get_position(props, "meta.test_source_frame");
    mlt_frame_close(frame);
}

struct window_snapshot
{
    mlt_position body_left_in;
    mlt_position body_left_out;
    mlt_position cut_a_in;
    mlt_position cut_a_out;
    mlt_position cut_b_in;
    mlt_position cut_b_out;
    mlt_position body_right_in;
    mlt_position body_right_out;
};

static void snapshot_windows(mlt_playlist playlist, int mix_index, struct window_snapshot *snap)
{
    mlt_producer body_left = mlt_playlist_get_clip(playlist, mix_index - 1);
    mlt_producer mix_cut = mlt_playlist_get_clip(playlist, mix_index);
    mlt_producer body_right = mlt_playlist_get_clip(playlist, mix_index + 1);
    mlt_tractor tractor = (mlt_tractor) mlt_producer_cut_parent(mix_cut);
    mlt_producer cut_a = mlt_tractor_get_track(tractor, 0);
    mlt_producer cut_b = mlt_tractor_get_track(tractor, 1);
    snap->body_left_in = mlt_producer_get_in(body_left);
    snap->body_left_out = mlt_producer_get_out(body_left);
    snap->cut_a_in = mlt_producer_get_in(cut_a);
    snap->cut_a_out = mlt_producer_get_out(cut_a);
    snap->cut_b_in = mlt_producer_get_in(cut_b);
    snap->cut_b_out = mlt_producer_get_out(cut_b);
    snap->body_right_in = mlt_producer_get_in(body_right);
    snap->body_right_out = mlt_producer_get_out(body_right);
}

static int snapshots_equal(const struct window_snapshot *a, const struct window_snapshot *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

struct change_notice
{
    int count;
    mlt_playlist playlist;
    int mix_index;
    struct window_snapshot windows;
    int cache_coherent;
};

/* Listener for the playlist's producer-changed event. It reads the graph back
 * while the notification is being delivered. */

static void on_producer_changed(mlt_properties owner, void *data, mlt_event_data event_data)
{
    (void) owner;
    (void) event_data;
    struct change_notice *notice = data;
    notice->count++;
    snapshot_windows(notice->playlist, notice->mix_index, &notice->windows);

    notice->cache_coherent = 1;
    for (int i = notice->mix_index - 1; i <= notice->mix_index + 1; i++) {
        mlt_playlist_clip_info info;
        CHECK(mlt_playlist_get_clip_info(notice->playlist, &info, i) == 0);
        mlt_producer cut = mlt_playlist_get_clip(notice->playlist, i);
        if (info.frame_in != mlt_producer_get_in(cut)
            || info.frame_out != mlt_producer_get_out(cut)
            || info.frame_count != mlt_producer_get_playtime(cut)
            || info.frame_count != mlt_playlist_clip_length(notice->playlist, i))
            notice->cache_coherent = 0;
    }
}

static void check_clip_info(mlt_playlist playlist, int index)
{
    mlt_playlist_clip_info info;
    CHECK(mlt_playlist_get_clip_info(playlist, &info, index) == 0);
    mlt_producer cut = mlt_playlist_get_clip(playlist, index);
    CHECK(info.frame_in == mlt_producer_get_in(cut));
    CHECK(info.frame_out == mlt_producer_get_out(cut));
    CHECK(info.frame_count == mlt_producer_get_playtime(cut));
    CHECK(info.frame_count == mlt_playlist_clip_length(playlist, index));
    CHECK(info.start == mlt_playlist_clip_start(playlist, index));
}

/* Build the canonical graph from the task description: A=[100,119],
 * B=[200,229], a 6 frame mix and a 44 frame program. */

static void build_basic_mix(mlt_profile profile,
                            mlt_playlist playlist,
                            mlt_producer *a,
                            mlt_producer *b,
                            mlt_transition transition)
{
    *a = make_source(profile, 1, 1000);
    *b = make_source(profile, 2, 1000);
    CHECK(mlt_playlist_append_io(playlist, *a, 100, 119) == 0);
    CHECK(mlt_playlist_append_io(playlist, *b, 200, 229) == 0);
    CHECK(mlt_playlist_mix(playlist, 0, 6, transition) == 0);
    CHECK(mlt_playlist_count(playlist) == 3);
    CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(playlist)) == 44);
}

static void test_positive_and_negative_roll(mlt_profile profile)
{
    mlt_playlist playlist = mlt_playlist_new(profile);
    mlt_producer a = NULL, b = NULL;
    build_basic_mix(profile, playlist, &a, &b, NULL);

    mlt_producer body_left = mlt_playlist_get_clip(playlist, 0);
    mlt_producer mix_cut = mlt_playlist_get_clip(playlist, 1);
    mlt_producer body_right = mlt_playlist_get_clip(playlist, 2);
    mlt_tractor tractor = (mlt_tractor) mlt_producer_cut_parent(mix_cut);
    mlt_field field = mlt_tractor_field(tractor);
    mlt_producer cut_a = mlt_tractor_get_track(tractor, 0);
    mlt_producer cut_b = mlt_tractor_get_track(tractor, 1);

    CHECK(mlt_producer_get_in(body_left) == 100);
    CHECK(mlt_producer_get_out(body_left) == 113);
    CHECK(mlt_producer_get_in(cut_a) == 114);
    CHECK(mlt_producer_get_out(cut_a) == 119);
    CHECK(mlt_producer_get_in(cut_b) == 200);
    CHECK(mlt_producer_get_out(cut_b) == 205);
    CHECK(mlt_producer_get_in(body_right) == 206);
    CHECK(mlt_producer_get_out(body_right) == 229);

    /* Keep the absolute play head at a fixed program frame across the edit. */
    mlt_producer_seek(MLT_PLAYLIST_PRODUCER(playlist), 20);

    CHECK(mlt_playlist_roll_mix(playlist, 1, 3) == 0);

    CHECK(mlt_producer_frame(MLT_PLAYLIST_PRODUCER(playlist)) == 20);
    CHECK(mlt_producer_get_in(body_left) == 100);
    CHECK(mlt_producer_get_out(body_left) == 116);
    CHECK(mlt_producer_get_in(cut_a) == 117);
    CHECK(mlt_producer_get_out(cut_a) == 122);
    CHECK(mlt_producer_get_in(cut_b) == 203);
    CHECK(mlt_producer_get_out(cut_b) == 208);
    CHECK(mlt_producer_get_in(body_right) == 209);
    CHECK(mlt_producer_get_out(body_right) == 229);

    /* Mix length, tractor/multitrack local points and program length hold. */
    CHECK(mlt_producer_get_playtime(mix_cut) == 6);
    CHECK(mlt_producer_get_in(MLT_TRACTOR_PRODUCER(tractor)) == 0);
    CHECK(mlt_producer_get_out(MLT_TRACTOR_PRODUCER(tractor)) == 5);
    mlt_multitrack multitrack = mlt_tractor_multitrack(tractor);
    CHECK(mlt_producer_get_in(MLT_MULTITRACK_PRODUCER(multitrack)) == 0);
    CHECK(mlt_producer_get_out(MLT_MULTITRACK_PRODUCER(multitrack)) == 5);
    CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(playlist)) == 44);
    CHECK(mlt_playlist_clip_start(playlist, 1) == 17);

    for (int i = 0; i < 3; i++)
        check_clip_info(playlist, i);

    /* Real frames, pulled out of order, must match the rolled source windows. */
    const mlt_position program_positions[] = {43, 0, 22, 17, 5, 23, 16, 40};
    for (size_t i = 0; i < sizeof(program_positions) / sizeof(program_positions[0]); i++) {
        mlt_position src_a = 0, src_b = 0;
        fetch_program(playlist, program_positions[i], &src_a, &src_b);
        if (program_positions[i] <= 16) {
            /* Left body: only source A, windows [100,116]. */
            CHECK(src_a == 100 + program_positions[i]);
            CHECK(src_b == -1);
        } else if (program_positions[i] <= 22) {
            /* Mix: both tracks are present, A=[117,122], B=[203,208]. */
            mlt_position local = program_positions[i] - 17;
            CHECK(src_a == 117 + local);
            CHECK(src_b == 203 + local);
        } else {
            /* Right body: only source B, windows [209,229]. */
            CHECK(src_a == -1);
            CHECK(src_b == 209 + (program_positions[i] - 23));
        }
    }

    int id = 0;
    mlt_position source_frame = 0;
    fetch_track(cut_a, 0, &id, &source_frame);
    CHECK(id == 1 && source_frame == 117);
    fetch_track(cut_a, 5, &id, &source_frame);
    CHECK(id == 1 && source_frame == 122);
    fetch_track(cut_b, 0, &id, &source_frame);
    CHECK(id == 2 && source_frame == 203);
    fetch_track(cut_b, 5, &id, &source_frame);
    CHECK(id == 2 && source_frame == 208);
    /* Ordinary body clips through the playlist. */
    mlt_position src_a = 0, src_b = 0;
    fetch_program(playlist, 16, &src_a, &src_b);
    CHECK(src_a == 116 && src_b == -1);
    fetch_program(playlist, 23, &src_a, &src_b);
    CHECK(src_a == -1 && src_b == 209);

    /* Object identity is preserved: nothing was rebuilt. */
    CHECK(mlt_tractor_field(tractor) == field);
    CHECK(mlt_tractor_get_track(tractor, 0) == cut_a);
    CHECK(mlt_tractor_get_track(tractor, 1) == cut_b);
    CHECK(mlt_playlist_get_clip(playlist, 0) == body_left);
    CHECK(mlt_playlist_get_clip(playlist, 1) == mix_cut);
    CHECK(mlt_playlist_get_clip(playlist, 2) == body_right);

    /* Rolling back restores the original windows. */
    CHECK(mlt_playlist_roll_mix(playlist, 1, -3) == 0);
    CHECK(mlt_producer_get_in(body_left) == 100);
    CHECK(mlt_producer_get_out(body_left) == 113);
    CHECK(mlt_producer_get_in(cut_a) == 114);
    CHECK(mlt_producer_get_out(cut_a) == 119);
    CHECK(mlt_producer_get_in(cut_b) == 200);
    CHECK(mlt_producer_get_out(cut_b) == 205);
    CHECK(mlt_producer_get_in(body_right) == 206);
    CHECK(mlt_producer_get_out(body_right) == 229);
    CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(playlist)) == 44);
    CHECK(mlt_playlist_clip_start(playlist, 1) == 14);

    mlt_playlist_close(playlist);
    mlt_producer_close(a);
    mlt_producer_close(b);
}

static void test_alternating_rolls_and_delta_zero(mlt_profile profile)
{
    mlt_playlist playlist = mlt_playlist_new(profile);
    mlt_producer a = NULL, b = NULL;
    build_basic_mix(profile, playlist, &a, &b, NULL);

    struct change_notice notice = {0};
    notice.playlist = playlist;
    notice.mix_index = 1;
    CHECK(mlt_events_listen(MLT_PLAYLIST_PROPERTIES(playlist),
                            &notice,
                            "producer-changed",
                            (mlt_listener) on_producer_changed)
          != NULL);

    struct window_snapshot original;
    snapshot_windows(playlist, 1, &original);

    /* delta == 0 on a valid graph succeeds, changes nothing and stays silent. */
    CHECK(mlt_playlist_roll_mix(playlist, 1, 0) == 0);
    CHECK(notice.count == 0);
    struct window_snapshot current;
    snapshot_windows(playlist, 1, &current);
    CHECK(snapshots_equal(&original, &current));

    for (int step = 0; step < 5; step++)
        CHECK(mlt_playlist_roll_mix(playlist, 1, 1) == 0);
    CHECK(notice.count == 5);
    for (int step = 0; step < 5; step++)
        CHECK(mlt_playlist_roll_mix(playlist, 1, -1) == 0);
    CHECK(notice.count == 10);

    snapshot_windows(playlist, 1, &current);
    CHECK(snapshots_equal(&original, &current));
    CHECK(notice.cache_coherent == 1);
    CHECK(notice.count == 10);

    mlt_playlist_close(playlist);
    mlt_producer_close(a);
    mlt_producer_close(b);
}

static void test_single_committed_notification(mlt_profile profile)
{
    mlt_playlist playlist = mlt_playlist_new(profile);
    mlt_producer a = NULL, b = NULL;
    build_basic_mix(profile, playlist, &a, &b, NULL);

    struct change_notice notice = {0};
    notice.playlist = playlist;
    notice.mix_index = 1;
    mlt_events_listen(MLT_PLAYLIST_PROPERTIES(playlist),
                      &notice,
                      "producer-changed",
                      (mlt_listener) on_producer_changed);

    CHECK(mlt_playlist_roll_mix(playlist, 1, 4) == 0);
    CHECK(notice.count == 1);
    CHECK(notice.cache_coherent == 1);
    CHECK(notice.windows.body_left_in == 100);
    CHECK(notice.windows.body_left_out == 117);
    CHECK(notice.windows.cut_a_in == 118);
    CHECK(notice.windows.cut_a_out == 123);
    CHECK(notice.windows.cut_b_in == 204);
    CHECK(notice.windows.cut_b_out == 209);
    CHECK(notice.windows.body_right_in == 210);
    CHECK(notice.windows.body_right_out == 229);

    mlt_playlist_close(playlist);
    mlt_producer_close(a);
    mlt_producer_close(b);
}

static void test_body_cannot_be_emptied(mlt_profile profile)
{
    struct change_notice notice;
    struct window_snapshot before;
    struct window_snapshot after;

    /* Positive direction: the 24 frame right body shrinks as the mix moves. */
    {
        mlt_playlist playlist = mlt_playlist_new(profile);
        mlt_producer a = NULL, b = NULL;
        build_basic_mix(profile, playlist, &a, &b, NULL);

        memset(&notice, 0, sizeof(notice));
        notice.playlist = playlist;
        notice.mix_index = 1;
        mlt_events_listen(MLT_PLAYLIST_PROPERTIES(playlist),
                          &notice,
                          "producer-changed",
                          (mlt_listener) on_producer_changed);
        snapshot_windows(playlist, 1, &before);

        /* +24 would leave zero frames on the right body; +23 is the limit. */
        CHECK(mlt_playlist_roll_mix(playlist, 1, 24) != 0);
        struct window_snapshot unchanged;
        snapshot_windows(playlist, 1, &unchanged);
        CHECK(snapshots_equal(&before, &unchanged));
        CHECK(notice.count == 0);
        CHECK(mlt_playlist_roll_mix(playlist, 1, 23) == 0);

        snapshot_windows(playlist, 1, &after);
        CHECK(after.body_right_out - after.body_right_in + 1 == 1);
        CHECK(after.body_left_out - after.body_left_in + 1 == 37);
        CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(playlist)) == 44);
        CHECK(notice.count == 1);

        mlt_playlist_close(playlist);
        mlt_producer_close(a);
        mlt_producer_close(b);
    }

    /* Negative direction: the 14 frame left body shrinks as the mix moves. */
    {
        mlt_playlist playlist = mlt_playlist_new(profile);
        mlt_producer a = NULL, b = NULL;
        build_basic_mix(profile, playlist, &a, &b, NULL);

        memset(&notice, 0, sizeof(notice));
        notice.playlist = playlist;
        notice.mix_index = 1;
        mlt_events_listen(MLT_PLAYLIST_PROPERTIES(playlist),
                          &notice,
                          "producer-changed",
                          (mlt_listener) on_producer_changed);
        snapshot_windows(playlist, 1, &before);

        /* -14 would leave zero frames on the left body; -13 is the limit. */
        CHECK(mlt_playlist_roll_mix(playlist, 1, -14) != 0);
        struct window_snapshot unchanged;
        snapshot_windows(playlist, 1, &unchanged);
        CHECK(snapshots_equal(&before, &unchanged));
        CHECK(notice.count == 0);
        CHECK(mlt_playlist_roll_mix(playlist, 1, -13) == 0);

        snapshot_windows(playlist, 1, &after);
        CHECK(after.body_left_out - after.body_left_in + 1 == 1);
        CHECK(after.body_right_out - after.body_right_in + 1 == 37);
        CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(playlist)) == 44);
        CHECK(notice.count == 1);

        mlt_playlist_close(playlist);
        mlt_producer_close(a);
        mlt_producer_close(b);
    }
}

static void test_insufficient_source_headroom(mlt_profile profile)
{
    /* A ends at source frame 124: the A mix cut can only slide 5 frames. */
    mlt_playlist playlist = mlt_playlist_new(profile);
    mlt_producer a = make_source(profile, 1, 125);
    mlt_producer b = make_source(profile, 2, 1000);
    CHECK(mlt_playlist_append_io(playlist, a, 100, 119) == 0);
    CHECK(mlt_playlist_append_io(playlist, b, 200, 229) == 0);
    CHECK(mlt_playlist_mix(playlist, 0, 6, NULL) == 0);

    struct window_snapshot before;
    snapshot_windows(playlist, 1, &before);
    CHECK(mlt_playlist_roll_mix(playlist, 1, 5) == 0);
    CHECK(mlt_playlist_roll_mix(playlist, 1, 1) != 0);
    struct window_snapshot after;
    snapshot_windows(playlist, 1, &after);
    CHECK(after.cut_a_out == 124);
    CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(playlist)) == 44);

    mlt_playlist_close(playlist);
    mlt_producer_close(a);
    mlt_producer_close(b);

    /* B starts at source frame 5: the B mix cut can only slide 5 frames left. */
    playlist = mlt_playlist_new(profile);
    a = make_source(profile, 1, 1000);
    b = make_source(profile, 2, 1000);
    CHECK(mlt_playlist_append_io(playlist, a, 100, 119) == 0);
    CHECK(mlt_playlist_append_io(playlist, b, 5, 34) == 0);
    CHECK(mlt_playlist_mix(playlist, 0, 6, NULL) == 0);
    snapshot_windows(playlist, 1, &before);
    CHECK(mlt_playlist_roll_mix(playlist, 1, -5) == 0);
    CHECK(mlt_playlist_roll_mix(playlist, 1, -1) != 0);
    snapshot_windows(playlist, 1, &after);
    CHECK(after.cut_b_in == 0);
    CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(playlist)) == 44);

    mlt_playlist_close(playlist);
    mlt_producer_close(a);
    mlt_producer_close(b);
}

static void test_extreme_delta_overflow(mlt_profile profile)
{
    mlt_playlist playlist = mlt_playlist_new(profile);
    mlt_producer a = NULL, b = NULL;
    build_basic_mix(profile, playlist, &a, &b, NULL);

    struct change_notice notice = {0};
    notice.playlist = playlist;
    notice.mix_index = 1;
    mlt_events_listen(MLT_PLAYLIST_PROPERTIES(playlist),
                      &notice,
                      "producer-changed",
                      (mlt_listener) on_producer_changed);

    struct window_snapshot before;
    snapshot_windows(playlist, 1, &before);

    const mlt_position extremes[] = {INT32_MIN, INT32_MIN + 1, INT32_MAX - 1, INT32_MAX};
    for (size_t i = 0; i < sizeof(extremes) / sizeof(extremes[0]); i++) {
        CHECK(mlt_playlist_roll_mix(playlist, 1, extremes[i]) != 0);
        struct window_snapshot after;
        snapshot_windows(playlist, 1, &after);
        CHECK(snapshots_equal(&before, &after));
    }
    CHECK(notice.count == 0);
    CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(playlist)) == 44);

    mlt_playlist_close(playlist);
    mlt_producer_close(a);
    mlt_producer_close(b);
}

static void test_invalid_indices(mlt_profile profile)
{
    mlt_playlist playlist = mlt_playlist_new(profile);
    mlt_producer a = NULL, b = NULL;
    build_basic_mix(profile, playlist, &a, &b, NULL);

    const int bad_indices[] = {-2, -1, 0, 2, 3, 100};
    for (size_t i = 0; i < sizeof(bad_indices) / sizeof(bad_indices[0]); i++)
        CHECK(mlt_playlist_roll_mix(playlist, bad_indices[i], 1) != 0);

    /* A playlist without a mix has no rollable entry. */
    mlt_playlist plain = mlt_playlist_new(profile);
    mlt_producer c = make_source(profile, 3, 1000);
    CHECK(mlt_playlist_append_io(plain, c, 0, 49) == 0);
    CHECK(mlt_playlist_roll_mix(plain, 0, 1) != 0);
    mlt_playlist_close(plain);
    mlt_producer_close(c);

    mlt_playlist_close(playlist);
    mlt_producer_close(a);
    mlt_producer_close(b);
}

static void test_blank_and_repeat_entries_rejected(mlt_profile profile)
{
    mlt_playlist playlist = mlt_playlist_new(profile);
    mlt_producer b = make_source(profile, 2, 1000);
    CHECK(mlt_playlist_blank(playlist, 19) == 0);
    CHECK(mlt_playlist_append_io(playlist, b, 200, 229) == 0);
    CHECK(mlt_playlist_mix(playlist, 0, 6, NULL) == 0);
    CHECK(mlt_playlist_count(playlist) == 3);
    CHECK(mlt_playlist_roll_mix(playlist, 1, 1) != 0);
    mlt_playlist_close(playlist);
    mlt_producer_close(b);

    playlist = mlt_playlist_new(profile);
    mlt_producer a = NULL;
    b = NULL;
    build_basic_mix(profile, playlist, &a, &b, NULL);
    CHECK(mlt_playlist_repeat_clip(playlist, 0, 2) == 0);
    CHECK(mlt_playlist_roll_mix(playlist, 1, 1) != 0);
    mlt_playlist_close(playlist);
    mlt_producer_close(a);
    mlt_producer_close(b);
}

static void test_damaged_links_rejected(mlt_profile profile)
{
    /* Broken mlt_mix marker. */
    {
        mlt_playlist playlist = mlt_playlist_new(profile);
        mlt_producer a = NULL, b = NULL;
        build_basic_mix(profile, playlist, &a, &b, NULL);
        mlt_tractor tractor = (mlt_tractor) mlt_producer_cut_parent(
            mlt_playlist_get_clip(playlist, 1));
        mlt_properties_set_data(MLT_TRACTOR_PROPERTIES(tractor), "mlt_mix", NULL, 0, NULL, NULL);
        CHECK(mlt_playlist_roll_mix(playlist, 1, 1) != 0);
        mlt_playlist_close(playlist);
        mlt_producer_close(a);
        mlt_producer_close(b);
    }

    /* Broken tractor -> body link. */
    {
        mlt_playlist playlist = mlt_playlist_new(profile);
        mlt_producer a = NULL, b = NULL;
        build_basic_mix(profile, playlist, &a, &b, NULL);
        mlt_tractor tractor = (mlt_tractor) mlt_producer_cut_parent(
            mlt_playlist_get_clip(playlist, 1));
        mlt_properties_set_data(MLT_TRACTOR_PROPERTIES(tractor),
                                "mix_in",
                                mlt_playlist_get_clip(playlist, 2),
                                0,
                                NULL,
                                NULL);
        CHECK(mlt_playlist_roll_mix(playlist, 1, 1) != 0);
        mlt_playlist_close(playlist);
        mlt_producer_close(a);
        mlt_producer_close(b);
    }

    /* Broken body -> tractor back link. */
    {
        mlt_playlist playlist = mlt_playlist_new(profile);
        mlt_producer a = NULL, b = NULL;
        build_basic_mix(profile, playlist, &a, &b, NULL);
        mlt_producer body_left = mlt_playlist_get_clip(playlist, 0);
        mlt_properties_set_data(MLT_PRODUCER_PROPERTIES(body_left),
                                "mix_out",
                                NULL,
                                0,
                                NULL,
                                NULL);
        CHECK(mlt_playlist_roll_mix(playlist, 1, 1) != 0);
        mlt_playlist_close(playlist);
        mlt_producer_close(a);
        mlt_producer_close(b);
    }

    /* A track that is no longer an ordinary source cut. */
    {
        mlt_playlist playlist = mlt_playlist_new(profile);
        mlt_producer a = NULL, b = NULL;
        build_basic_mix(profile, playlist, &a, &b, NULL);
        mlt_tractor tractor = (mlt_tractor) mlt_producer_cut_parent(
            mlt_playlist_get_clip(playlist, 1));
        mlt_tractor_set_track(tractor, a, 0);
        CHECK(mlt_playlist_roll_mix(playlist, 1, 1) != 0);
        mlt_playlist_close(playlist);
        mlt_producer_close(a);
        mlt_producer_close(b);
    }

    /* Broken source window continuity. */
    {
        mlt_playlist playlist = mlt_playlist_new(profile);
        mlt_producer a = NULL, b = NULL;
        build_basic_mix(profile, playlist, &a, &b, NULL);
        mlt_tractor tractor = (mlt_tractor) mlt_producer_cut_parent(
            mlt_playlist_get_clip(playlist, 1));
        mlt_producer cut_a = mlt_tractor_get_track(tractor, 0);
        mlt_producer_set_in_and_out(cut_a, 115, 120);
        CHECK(mlt_playlist_roll_mix(playlist, 1, 1) != 0);
        mlt_playlist_close(playlist);
        mlt_producer_close(a);
        mlt_producer_close(b);
    }

    /* Tractor pretending to be a mix but holding only one track. */
    {
        mlt_playlist playlist = mlt_playlist_new(profile);
        mlt_producer a = NULL, b = NULL;
        build_basic_mix(profile, playlist, &a, &b, NULL);
        mlt_tractor tractor = (mlt_tractor) mlt_producer_cut_parent(
            mlt_playlist_get_clip(playlist, 1));
        mlt_multitrack multitrack = mlt_tractor_multitrack(tractor);
        CHECK(mlt_multitrack_disconnect(multitrack, 1) == 0);
        CHECK(mlt_playlist_roll_mix(playlist, 1, 1) != 0);
        mlt_playlist_close(playlist);
        mlt_producer_close(a);
        mlt_producer_close(b);
    }
}

static void test_reused_source_parent_is_isolated(mlt_profile profile)
{
    mlt_playlist playlist = mlt_playlist_new(profile);
    mlt_producer a = NULL, b = NULL;
    build_basic_mix(profile, playlist, &a, &b, NULL);

    /* Reuse source A elsewhere in the same program. */
    CHECK(mlt_playlist_append_io(playlist, a, 50, 60) == 0);
    CHECK(mlt_playlist_count(playlist) == 4);
    mlt_producer reused_cut = mlt_playlist_get_clip(playlist, 3);

    CHECK(mlt_playlist_roll_mix(playlist, 1, 7) == 0);

    CHECK(mlt_producer_get_in(reused_cut) == 50);
    CHECK(mlt_producer_get_out(reused_cut) == 60);
    CHECK(mlt_producer_get_playtime(reused_cut) == 11);
    /* The shared parent keeps its own points and length. */
    CHECK(mlt_producer_get_in(a) == 0);
    CHECK(mlt_producer_get_out(a) == 999);
    CHECK(mlt_producer_get_length(a) == 1000);
    CHECK(mlt_producer_get_in(b) == 0);
    CHECK(mlt_producer_get_out(b) == 999);
    /* The unrelated entry stays where it was on the timeline. */
    CHECK(mlt_playlist_clip_start(playlist, 3) == 44);
    check_clip_info(playlist, 3);

    mlt_position src_a = 0, src_b = 0;
    fetch_program(playlist, 44, &src_a, &src_b);
    CHECK(src_a == 50 && src_b == -1);
    fetch_program(playlist, 54, &src_a, &src_b);
    CHECK(src_a == 60 && src_b == -1);

    mlt_playlist_close(playlist);
    mlt_producer_close(a);
    mlt_producer_close(b);
}

static void test_neighbouring_mixes_undisturbed(mlt_profile profile)
{
    mlt_playlist playlist = mlt_playlist_new(profile);
    mlt_producer a = make_source(profile, 1, 1000);
    mlt_producer b = make_source(profile, 2, 1000);
    mlt_producer c = make_source(profile, 3, 1000);
    CHECK(mlt_playlist_append_io(playlist, a, 100, 119) == 0);
    CHECK(mlt_playlist_append_io(playlist, b, 200, 249) == 0);
    CHECK(mlt_playlist_append_io(playlist, c, 300, 329) == 0);
    CHECK(mlt_playlist_mix(playlist, 0, 6, NULL) == 0);
    CHECK(mlt_playlist_mix(playlist, 2, 6, NULL) == 0);
    CHECK(mlt_playlist_count(playlist) == 5);

    mlt_tractor first_tractor = (mlt_tractor) mlt_producer_cut_parent(
        mlt_playlist_get_clip(playlist, 1));
    mlt_producer first_cut_a = mlt_tractor_get_track(first_tractor, 0);
    mlt_producer first_cut_b = mlt_tractor_get_track(first_tractor, 1);
    mlt_producer first_body_left = mlt_playlist_get_clip(playlist, 0);
    /* The middle B body is shared: mix_out of the first mix and left body of
     * the second. Rolling the second mix changes its window, but the object and
     * its link back to the first mix must survive. */
    mlt_producer shared_body = mlt_playlist_get_clip(playlist, 2);
    struct window_snapshot first_before;
    snapshot_windows(playlist, 1, &first_before);
    mlt_position first_mix_start = mlt_playlist_clip_start(playlist, 1);

    /* Roll the second mix (B/C boundary) two frames later. */
    CHECK(mlt_playlist_roll_mix(playlist, 3, 2) == 0);

    struct window_snapshot first_after;
    snapshot_windows(playlist, 1, &first_after);
    /* The first mix's own cuts and its left body are untouched. */
    CHECK(first_after.body_left_in == first_before.body_left_in);
    CHECK(first_after.body_left_out == first_before.body_left_out);
    CHECK(first_after.cut_a_in == first_before.cut_a_in);
    CHECK(first_after.cut_a_out == first_before.cut_a_out);
    CHECK(first_after.cut_b_in == first_before.cut_b_in);
    CHECK(first_after.cut_b_out == first_before.cut_b_out);
    /* The shared B body is the same cut object; only its window moved. */
    CHECK(mlt_playlist_get_clip(playlist, 2) == shared_body);
    /* The first mix keeps its position, length and bidirectional links. */
    CHECK(mlt_playlist_clip_start(playlist, 1) == first_mix_start);
    CHECK(mlt_tractor_get_track(first_tractor, 0) == first_cut_a);
    CHECK(mlt_tractor_get_track(first_tractor, 1) == first_cut_b);
    CHECK(mlt_properties_get_data(MLT_TRACTOR_PROPERTIES(first_tractor), "mix_in", NULL)
          == first_body_left);
    CHECK(mlt_properties_get_data(MLT_TRACTOR_PROPERTIES(first_tractor), "mix_out", NULL)
          == shared_body);
    /* The shared B body is the right side of the first mix and the left side
     * of the second; both back links must remain intact. */
    CHECK(mlt_properties_get_data(MLT_PRODUCER_PROPERTIES(shared_body), "mix_in", NULL)
          == first_tractor);
    mlt_tractor second_tractor_links = (mlt_tractor) mlt_producer_cut_parent(
        mlt_playlist_get_clip(playlist, 3));
    CHECK(mlt_properties_get_data(MLT_PRODUCER_PROPERTIES(shared_body), "mix_out", NULL)
          == second_tractor_links);
    CHECK(mlt_producer_get_playtime(mlt_playlist_get_clip(playlist, 1)) == 6);
    CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(playlist)) == 88);

    /* The rolled B/C mix windows. */
    struct window_snapshot second;
    snapshot_windows(playlist, 3, &second);
    CHECK(second.body_left_in == 206 && second.body_left_out == 245);
    CHECK(second.cut_a_in == 246 && second.cut_a_out == 251);
    CHECK(second.cut_b_in == 302 && second.cut_b_out == 307);
    CHECK(second.body_right_in == 308 && second.body_right_out == 329);

    int id = 0;
    mlt_position source_frame = 0;
    mlt_tractor second_tractor = (mlt_tractor) mlt_producer_cut_parent(
        mlt_playlist_get_clip(playlist, 3));
    fetch_track(mlt_tractor_get_track(second_tractor, 0), 0, &id, &source_frame);
    CHECK(id == 2 && source_frame == 246);
    fetch_track(mlt_tractor_get_track(second_tractor, 1), 5, &id, &source_frame);
    CHECK(id == 3 && source_frame == 307);

    mlt_playlist_close(playlist);
    mlt_producer_close(a);
    mlt_producer_close(b);
    mlt_producer_close(c);
}

static void test_transition_filters_and_identity_preserved(mlt_profile profile)
{
    mlt_playlist playlist = mlt_playlist_new(profile);
    mlt_producer a = make_source(profile, 1, 1000);
    mlt_producer b = make_source(profile, 2, 1000);

    /* A filter living on the shared source parent must survive untouched. */
    mlt_filter filter = mlt_filter_new();
    CHECK(filter != NULL);
    mlt_properties_set_int(MLT_FILTER_PROPERTIES(filter), "test.filter_marker", 909);
    CHECK(mlt_producer_attach(a, filter) == 0);

    CHECK(mlt_playlist_append_io(playlist, a, 100, 119) == 0);
    CHECK(mlt_playlist_append_io(playlist, b, 200, 229) == 0);

    mlt_transition transition = mlt_transition_new();
    CHECK(transition != NULL);
    mlt_properties_set_int(MLT_TRANSITION_PROPERTIES(transition), "test.curve_marker", 4242);
    CHECK(mlt_playlist_mix(playlist, 0, 6, transition) == 0);

    mlt_tractor tractor = (mlt_tractor) mlt_producer_cut_parent(
        mlt_playlist_get_clip(playlist, 1));
    mlt_field field = mlt_tractor_field(tractor);
    mlt_producer cut_a = mlt_tractor_get_track(tractor, 0);
    mlt_producer cut_b = mlt_tractor_get_track(tractor, 1);
    mlt_producer body_left = mlt_playlist_get_clip(playlist, 0);
    mlt_producer body_right = mlt_playlist_get_clip(playlist, 2);

    CHECK(mlt_playlist_roll_mix(playlist, 1, 4) == 0);
    CHECK(mlt_playlist_roll_mix(playlist, 1, -4) == 0);

    /* All service objects keep their identity. */
    CHECK(mlt_tractor_field(tractor) == field);
    CHECK(mlt_tractor_get_track(tractor, 0) == cut_a);
    CHECK(mlt_tractor_get_track(tractor, 1) == cut_b);
    CHECK(mlt_playlist_get_clip(playlist, 0) == body_left);
    CHECK(mlt_playlist_get_clip(playlist, 2) == body_right);

    /* Transition points, tracks and custom properties are untouched. */
    CHECK(mlt_transition_get_in(transition) == 0);
    CHECK(mlt_transition_get_out(transition) == 5);
    CHECK(mlt_transition_get_length(transition) == 6);
    CHECK(mlt_transition_get_a_track(transition) == 0);
    CHECK(mlt_transition_get_b_track(transition) == 1);
    CHECK(mlt_properties_get_int(MLT_TRANSITION_PROPERTIES(transition), "test.curve_marker")
          == 4242);

    /* The parent filter was neither rebuilt nor reconfigured. */
    CHECK(mlt_producer_filter(a, 0) == filter);
    CHECK(mlt_properties_get_int(MLT_FILTER_PROPERTIES(filter), "test.filter_marker") == 909);

    mlt_playlist_close(playlist);
    mlt_producer_close(a);
    mlt_producer_close(b);
}

static void test_failure_leaves_playhead_and_graph_untouched(mlt_profile profile)
{
    mlt_playlist playlist = mlt_playlist_new(profile);
    mlt_producer a = NULL, b = NULL;
    build_basic_mix(profile, playlist, &a, &b, NULL);

    struct change_notice notice = {0};
    notice.playlist = playlist;
    notice.mix_index = 1;
    mlt_events_listen(MLT_PLAYLIST_PROPERTIES(playlist),
                      &notice,
                      "producer-changed",
                      (mlt_listener) on_producer_changed);

    struct window_snapshot before;
    snapshot_windows(playlist, 1, &before);
    mlt_producer_seek(MLT_PLAYLIST_PRODUCER(playlist), 31);

    CHECK(mlt_playlist_roll_mix(playlist, 1, 100) != 0);

    CHECK(mlt_producer_frame(MLT_PLAYLIST_PRODUCER(playlist)) == 31);
    CHECK(notice.count == 0);
    struct window_snapshot after;
    snapshot_windows(playlist, 1, &after);
    CHECK(snapshots_equal(&before, &after));
    CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(playlist)) == 44);

    mlt_playlist_close(playlist);
    mlt_producer_close(a);
    mlt_producer_close(b);
}

int main(void)
{
    mlt_profile profile = mlt_profile_init(NULL);
    CHECK(profile != NULL);
    test_profile = profile;

    /* No service modules are loaded in this build; install a blank stand-in. */
    mlt_factory_init(NULL);
    mlt_events_listen(mlt_factory_event_object(),
                      NULL,
                      "producer-create-request",
                      (mlt_listener) on_producer_create_request);

    test_positive_and_negative_roll(profile);
    test_alternating_rolls_and_delta_zero(profile);
    test_single_committed_notification(profile);
    test_body_cannot_be_emptied(profile);
    test_insufficient_source_headroom(profile);
    test_extreme_delta_overflow(profile);
    test_invalid_indices(profile);
    test_blank_and_repeat_entries_rejected(profile);
    test_damaged_links_rejected(profile);
    test_reused_source_parent_is_isolated(profile);
    test_neighbouring_mixes_undisturbed(profile);
    test_transition_filters_and_identity_preserved(profile);
    test_failure_leaves_playhead_and_graph_untouched(profile);

    mlt_profile_close(profile);
    mlt_factory_close();
    printf("MLT_ROLL_MIX_OK all roll_mix regressions passed\n");
    return 0;
}
