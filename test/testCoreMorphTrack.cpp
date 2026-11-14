#include "TestPrologue.h"

#include <cal3d/coremorphtrack.h>

FIXTURE(CalCoreMorphTrackFixture) {
    float epsilon = 0.00001f;
};

TEST_F(CalCoreMorphTrackFixture, can_keep_animation_with_one_key_if_two_keys_are_the_same) {
    CalCoreMorphTrack track;

    track.keyframes = std::vector<CalCoreMorphKeyframe>({
        CalCoreMorphKeyframe(0.0f, 0.0f),
        CalCoreMorphKeyframe(1.0f, 0.0f)
    });

    CHECK_EQUAL(2U, track.keyframes.size());

    CHECK_CLOSE(0.0f,  track.keyframes[0].weight, epsilon);
    CHECK_CLOSE(0.0f, track.keyframes[1].weight, epsilon);

    CHECK_CLOSE(0.0f, track.keyframes[0].time, epsilon);
    CHECK_CLOSE(1.0f, track.keyframes[1].time, epsilon);


    track.optimize();

    CHECK_EQUAL(1U, track.keyframes.size());

    CHECK_CLOSE(0.0f, track.keyframes[0].weight, epsilon);

    CHECK_CLOSE(0.0f, track.keyframes[0].time, epsilon);
}


TEST_F(CalCoreMorphTrackFixture, can_optimize_linear_keys) {
    CalCoreMorphTrack track;

    track.keyframes = std::vector<CalCoreMorphKeyframe>({
        CalCoreMorphKeyframe(0.0f, 0.0f),
        CalCoreMorphKeyframe(1.0f, 0.25f),
        CalCoreMorphKeyframe(2.0f, 0.5f),
        CalCoreMorphKeyframe(3.0f, 0.75f),
        CalCoreMorphKeyframe(4.0f, 1.0f),
        CalCoreMorphKeyframe(5.0f, 0.9f),
        CalCoreMorphKeyframe(6.0f, 0.8f),
        CalCoreMorphKeyframe(7.0f, 0.7f),
        CalCoreMorphKeyframe(8.0f, 0.6f)
    });

    CHECK_EQUAL(9U, track.keyframes.size());

    CHECK_CLOSE(0.0f,  track.keyframes[0].weight, epsilon);
    CHECK_CLOSE(0.25f, track.keyframes[1].weight, epsilon);
    CHECK_CLOSE(0.5f,  track.keyframes[2].weight, epsilon);
    CHECK_CLOSE(0.75f, track.keyframes[3].weight, epsilon);
    CHECK_CLOSE(1.0f,  track.keyframes[4].weight, epsilon);
    CHECK_CLOSE(0.9f,  track.keyframes[5].weight, epsilon);
    CHECK_CLOSE(0.8f,  track.keyframes[6].weight, epsilon);
    CHECK_CLOSE(0.7f,  track.keyframes[7].weight, epsilon);
    CHECK_CLOSE(0.6f,  track.keyframes[8].weight, epsilon);

    CHECK_CLOSE(0.0f, track.keyframes[0].time, epsilon);
    CHECK_CLOSE(1.0f, track.keyframes[1].time, epsilon);
    CHECK_CLOSE(2.0f, track.keyframes[2].time, epsilon);
    CHECK_CLOSE(3.0f, track.keyframes[3].time, epsilon);
    CHECK_CLOSE(4.0f, track.keyframes[4].time, epsilon);
    CHECK_CLOSE(5.0f, track.keyframes[5].time, epsilon);
    CHECK_CLOSE(6.0f, track.keyframes[6].time, epsilon);
    CHECK_CLOSE(7.0f, track.keyframes[7].time, epsilon);
    CHECK_CLOSE(8.0f, track.keyframes[8].time, epsilon);

    track.optimize();

    CHECK_EQUAL(3U, track.keyframes.size());

    CHECK_CLOSE(0.0f, track.keyframes[0].weight, epsilon);
    CHECK_CLOSE(1.0f, track.keyframes[1].weight, epsilon);
    CHECK_CLOSE(0.6f, track.keyframes[2].weight, epsilon);

    CHECK_CLOSE(0.0f, track.keyframes[0].time, epsilon);
    CHECK_CLOSE(4.0f, track.keyframes[1].time, epsilon);
    CHECK_CLOSE(8.0f, track.keyframes[2].time, epsilon);
}
