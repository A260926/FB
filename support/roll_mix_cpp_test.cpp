/*
 * roll_mix_cpp_test.cpp - regression tests for Mlt::Playlist::roll_mix()
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

#include <mlt++/Mlt.h>

#include <cstdio>
#include <cstdlib>

using namespace Mlt;

#define CHECK(x) \
    do { \
        if (!(x)) { \
            std::fprintf(stderr, "check failed line %d: %s\n", __LINE__, #x); \
            std::exit(1); \
        } \
    } while (0)

/* The C++ wrapper is exercised against the same kind of real producer graph as
 * the C regression: a custom get_frame callback stamps each frame with the
 * source id and the absolute source frame, so rendered output can be verified
 * rather than only computed endpoints. */

static int cpp_source_get_frame(mlt_producer producer, mlt_frame_ptr frame, int index)
{
    (void) index;
    mlt_producer root = mlt_producer_cut_parent(producer);
    *frame = mlt_frame_init(MLT_PRODUCER_SERVICE(producer));
    CHECK(*frame != NULL);
    int source_id = mlt_properties_get_int(MLT_PRODUCER_PROPERTIES(root), "test.source_id");
    mlt_position source_frame = mlt_producer_frame(root);
    mlt_frame_set_position(*frame, source_frame);
    mlt_properties props = MLT_FRAME_PROPERTIES(*frame);
    mlt_properties_set_int(props, "meta.test_source_id", source_id);
    mlt_properties_set_position(props, "meta.test_source_frame", source_frame);
    char key[32];
    std::snprintf(key, sizeof(key), "meta.src.%d", source_id);
    mlt_properties_set_position(props, key, source_frame);
    return 0;
}

static mlt_producer make_source(mlt_profile profile, int id, int length)
{
    mlt_producer producer = mlt_producer_new(profile);
    CHECK(producer != NULL);
    mlt_properties_set_int(MLT_PRODUCER_PROPERTIES(producer), "test.source_id", id);
    mlt_properties_set_position(MLT_PRODUCER_PROPERTIES(producer), "length", length);
    mlt_producer_set_in_and_out(producer, 0, length - 1);
    producer->get_frame = cpp_source_get_frame;
    return producer;
}

struct Notice
{
    int count;
    Playlist *playlist;
};

static void on_changed(mlt_properties owner, void *data, mlt_event_data event_data)
{
    (void) owner;
    (void) event_data;
    Notice *notice = static_cast<Notice *>(data);
    notice->count++;

    /* The committed state must already read back coherently from the callback. */
    ClipInfo *info = notice->playlist->clip_info(1);
    CHECK(info != NULL);
    CHECK(info->frame_count == 6);
    delete info;
}

/* Pull one program frame and read the per-source stamps. Inside the mix both
 * tracks are merged, so per-id keys carry both windows; an absent source reads
 * back as -1. */

static void fetch_program(Playlist &playlist, int frame, int &src_a, int &src_b)
{
    CHECK(playlist.seek(frame) == 0);
    Frame *got = playlist.get_frame();
    CHECK(got != NULL);
    mlt_properties props = got->get_properties();
    src_a = mlt_properties_exists(props, "meta.src.1") ? got->get_int("meta.src.1") : -1;
    src_b = mlt_properties_exists(props, "meta.src.2") ? got->get_int("meta.src.2") : -1;
    delete got;
}

int main()
{
    Profile profile;
    CHECK(profile.is_valid());

    mlt_producer raw_a = make_source(profile.get_profile(), 1, 1000);
    mlt_producer raw_b = make_source(profile.get_profile(), 2, 1000);

    {
        Playlist playlist(profile);
        Producer producer_a(raw_a);
        Producer producer_b(raw_b);

        CHECK(playlist.append(producer_a, 100, 119) == 0);
        CHECK(playlist.append(producer_b, 200, 229) == 0);
        CHECK(playlist.mix(0, 6) == 0);
        CHECK(playlist.count() == 3);
        CHECK(playlist.get_playtime() == 44);

        Producer *body_left = playlist.get_clip(0);
        Producer *mix_clip = playlist.get_clip(1);
        Producer *body_right = playlist.get_clip(2);
        CHECK(body_left && mix_clip && body_right);

        mlt_tractor raw_tractor = (mlt_tractor) mlt_producer_cut_parent(
            mix_clip->get_producer());
        Tractor tractor(raw_tractor);
        Producer *cut_a = tractor.track(0);
        Producer *cut_b = tractor.track(1);
        CHECK(cut_a != NULL && cut_b != NULL);

        CHECK(body_left->get_in() == 100 && body_left->get_out() == 113);
        CHECK(cut_a->get_in() == 114 && cut_a->get_out() == 119);
        CHECK(cut_b->get_in() == 200 && cut_b->get_out() == 205);
        CHECK(body_right->get_in() == 206 && body_right->get_out() == 229);

        Notice notice = {0, &playlist};
        Event *event = playlist.listen("producer-changed", &notice, on_changed);
        CHECK(event != NULL);

        /* A positive roll through the C++ wrapper. */
        CHECK(playlist.seek(20) == 0);
        CHECK(playlist.roll_mix(1, 3) == 0);
        CHECK(notice.count == 1);
        CHECK(playlist.frame() == 20);

        CHECK(body_left->get_in() == 100 && body_left->get_out() == 116);
        CHECK(cut_a->get_in() == 117 && cut_a->get_out() == 122);
        CHECK(cut_b->get_in() == 203 && cut_b->get_out() == 208);
        CHECK(body_right->get_in() == 209 && body_right->get_out() == 229);
        CHECK(mix_clip->get_playtime() == 6);
        CHECK(playlist.get_playtime() == 44);
        CHECK(playlist.clip_start(1) == 17);

        /* ClipInfo stays coherent with the live windows. */
        ClipInfo *left_info = playlist.clip_info(0);
        ClipInfo *right_info = playlist.clip_info(2);
        CHECK(left_info && right_info);
        CHECK(left_info->frame_in == 100 && left_info->frame_out == 116);
        CHECK(right_info->frame_in == 209 && right_info->frame_out == 229);
        CHECK(left_info->frame_count == 17 && right_info->frame_count == 21);
        delete left_info;
        delete right_info;

        /* Rendered frames, pulled out of order, follow the rolled windows. */
        const int positions[] = {43, 0, 22, 17, 5, 23, 16, 40};
        for (int position : positions) {
            int src_a = 0;
            int src_b = 0;
            fetch_program(playlist, position, src_a, src_b);
            if (position <= 16) {
                /* Left body: only source A, windows [100,116]. */
                CHECK(src_a == 100 + position);
                CHECK(src_b == -1);
            } else if (position <= 22) {
                /* Mix: both tracks, A=[117,122], B=[203,208]. */
                CHECK(src_a == 117 + (position - 17));
                CHECK(src_b == 203 + (position - 17));
            } else {
                /* Right body: only source B, windows [209,229]. */
                CHECK(src_a == -1);
                CHECK(src_b == 209 + (position - 23));
            }
        }

        /* The wrapper keeps the same service objects. */
        Producer *cut_a_again = tractor.track(0);
        Producer *cut_b_again = tractor.track(1);
        CHECK(cut_a_again && cut_b_again);
        CHECK(cut_a_again->get_producer() == cut_a->get_producer());
        CHECK(cut_b_again->get_producer() == cut_b->get_producer());
        CHECK(playlist.get_clip(1)->get_producer() == mix_clip->get_producer());
        delete cut_a_again;
        delete cut_b_again;

        /* Rolling back restores the original graph. */
        CHECK(playlist.roll_mix(1, -3) == 0);
        CHECK(notice.count == 2);
        CHECK(body_left->get_in() == 100 && body_left->get_out() == 113);
        CHECK(cut_a->get_in() == 114 && cut_a->get_out() == 119);
        CHECK(cut_b->get_in() == 200 && cut_b->get_out() == 205);
        CHECK(body_right->get_in() == 206 && body_right->get_out() == 229);
        CHECK(playlist.clip_start(1) == 14);

        /* delta == 0 succeeds silently. */
        CHECK(playlist.roll_mix(1, 0) == 0);
        CHECK(notice.count == 2);

        /* A roll that would empty the right body is rejected untouched. */
        CHECK(playlist.roll_mix(1, 24) != 0);
        CHECK(notice.count == 2);
        CHECK(body_right->get_in() == 206 && body_right->get_out() == 229);
        CHECK(playlist.get_playtime() == 44);

        /* Invalid indices are rejected by the wrapped C function. */
        CHECK(playlist.roll_mix(0, 1) != 0);
        CHECK(playlist.roll_mix(2, 1) != 0);
        CHECK(playlist.roll_mix(-1, 1) != 0);

        int src_a = 0;
        int src_b = 0;
        fetch_program(playlist, 14, src_a, src_b);
        CHECK(src_a == 114 && src_b == 200);
        fetch_program(playlist, 19, src_a, src_b);
        CHECK(src_a == 119 && src_b == 205);

        delete event;
        delete cut_a;
        delete cut_b;
        delete body_left;
        delete mix_clip;
        delete body_right;
    }

    mlt_producer_close(raw_a);
    mlt_producer_close(raw_b);

    std::printf("MLT_ROLL_MIX_CPP_OK C++ roll_mix wrapper regressions passed\n");
    return 0;
}
