/*
 * Regression test for Mlt::Playlist::roll_mix - the C++ wrapper of
 * mlt_playlist_roll_mix.
 */

#include <mlt++/Mlt.h>
#include <cstdio>

#define CHECK(x) \
    do { \
        if (!(x)) { \
            fprintf(stderr, "check failed line %d: %s\n", __LINE__, #x); \
            return 1; \
        } \
    } while (0)

using namespace Mlt;

static mlt_producer new_source(Profile &profile, mlt_position length)
{
    mlt_producer producer = mlt_producer_new(profile.get_profile());
    mlt_properties_set_position(MLT_PRODUCER_PROPERTIES(producer), "length", length);
    mlt_producer_set_in_and_out(producer, 0, length - 1);
    return producer;
}

int main(void)
{
    Profile profile;
    mlt_producer a_raw = new_source(profile, 1000);
    mlt_producer b_raw = new_source(profile, 1000);
    CHECK(a_raw != nullptr && b_raw != nullptr);
    Producer a(a_raw);
    Producer b(b_raw);
    mlt_producer_close(a_raw);
    mlt_producer_close(b_raw);

    Playlist playlist(profile);
    CHECK(playlist.append(a, 100, 119) == 0);
    CHECK(playlist.append(b, 200, 229) == 0);
    CHECK(playlist.mix(0, 6) == 0);
    CHECK(playlist.count() == 3);
    CHECK(playlist.is_mix(1));
    CHECK(playlist.clip_length(0) == 14);
    CHECK(playlist.clip_length(1) == 6);
    CHECK(playlist.clip_length(2) == 24);

    /* Roll the mix forward by 3 frames. */
    CHECK(playlist.roll_mix(1, 3) == 0);
    CHECK(playlist.clip_length(0) == 17);
    CHECK(playlist.clip_length(1) == 6);
    CHECK(playlist.clip_length(2) == 21);
    CHECK(playlist.clip_start(1) == 17);
    CHECK(playlist.get_playtime() == 44);

    mlt_producer mix_clip = mlt_playlist_get_clip(playlist.get_playlist(), 1);
    mlt_tractor tractor = (mlt_tractor) mlt_producer_cut_parent(mix_clip);
    CHECK(mlt_producer_get_in(mlt_tractor_get_track(tractor, 0)) == 117);
    CHECK(mlt_producer_get_out(mlt_tractor_get_track(tractor, 0)) == 122);
    CHECK(mlt_producer_get_in(mlt_tractor_get_track(tractor, 1)) == 203);
    CHECK(mlt_producer_get_out(mlt_tractor_get_track(tractor, 1)) == 208);

    /* Roll it back. */
    CHECK(playlist.roll_mix(1, -3) == 0);
    CHECK(playlist.clip_length(0) == 14);
    CHECK(playlist.clip_length(2) == 24);
    CHECK(playlist.clip_start(1) == 14);
    CHECK(mlt_producer_get_in(mlt_tractor_get_track(tractor, 0)) == 114);
    CHECK(mlt_producer_get_out(mlt_tractor_get_track(tractor, 0)) == 119);

    /* Invalid rolls fail. */
    CHECK(playlist.roll_mix(0, 1) != 0);
    CHECK(playlist.roll_mix(2, 1) != 0);
    CHECK(playlist.roll_mix(-1, 1) != 0);
    CHECK(playlist.roll_mix(1, 100) != 0);
    CHECK(playlist.roll_mix(1, -100) != 0);
    CHECK(playlist.clip_length(0) == 14);
    CHECK(playlist.clip_length(1) == 6);
    CHECK(playlist.clip_length(2) == 24);
    CHECK(playlist.get_playtime() == 44);

    printf("MLT_CPP_ROLL_MIX_OK\n");
    return 0;
}
