// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project
//
// Example: ripple delete a program-time range from a multi-track timeline
// with algo::timeline_ripple_delete().
//
// Build and run (standalone, no Python required):
//
//   bash support/ripple_delete_example.sh
//
// The example mirrors the worked example in the algorithm's documentation:
// a 24fps program 60 frames long, a video track with
// source[100,120) / 10 frame Gap / source[200,230), and an audio track
// exposing [80,120) of a 200 frame clip starting at source 500. Removing
// program [10,20) pulls both tracks in to 50 frames.

#include <opentimelineio/algo/timelineEditAlgorithm.h>
#include <opentimelineio/clip.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/timeline.h>
#include <opentimelineio/track.h>

#include <cstdlib>
#include <iostream>

namespace otio = OTIO_NS;
namespace otime = opentime;
using otime::RationalTime;
using otime::TimeRange;

namespace {

RationalTime
frames(double value)
{
    return RationalTime(value, 24.0);
}

TimeRange
frames_range(double start, double duration)
{
    return TimeRange(frames(start), frames(duration));
}

void
print_track(otio::Track const* track)
{
    std::cout << "  track " << track->name() << " ("
              << track->duration().value() << " frames):\n";
    for (auto const& child: track->children())
    {
        if (auto clip = dynamic_cast<otio::Clip*>(child.value))
        {
            auto range = clip->trimmed_range();
            std::cout << "    Clip  " << clip->name() << "  source ["
                      << range.start_time().value() << ", "
                      << range.end_time_exclusive().value() << ")\n";
        }
        else if (auto gap = dynamic_cast<otio::Gap*>(child.value))
        {
            std::cout << "    Gap   " << gap->duration().value()
                      << " frames\n";
        }
    }
}

} // namespace

int
main()
{
    otio::SerializableObject::Retainer<otio::Timeline> timeline(
        new otio::Timeline("ripple-delete-example", frames(1000)));

    otio::Stack* stack = new otio::Stack("tracks");
    timeline->set_tracks(stack);

    otio::Track* video = new otio::Track("video");
    video->append_child(new otio::Clip(
        "A",
        nullptr,
        frames_range(100, 20)));
    video->append_child(new otio::Gap(frames_range(0, 10)));
    video->append_child(new otio::Clip(
        "B",
        nullptr,
        frames_range(200, 30)));
    stack->append_child(video);

    // The audio track is trimmed to [80, 120) of a long clip.
    otio::Track* audio =
        new otio::Track("audio", frames_range(80, 40));
    audio->append_child(new otio::Clip(
        "C",
        nullptr,
        frames_range(500, 200)));
    stack->append_child(audio);

    std::cout << "Before (" << timeline->duration().value()
              << " frames):\n";
    print_track(video);
    print_track(audio);

    otio::ErrorStatus status;
    otio::SerializableObject::Retainer<otio::Timeline> result(
        otio::algo::timeline_ripple_delete(
            timeline,
            frames_range(10, 10),
            &status));
    if (!result)
    {
        std::cerr << "ripple delete failed: " << status.full_description
                  << "\n";
        return 1;
    }

    std::cout << "\nAfter deleting program [10, 20) ("
              << result->duration().value() << " frames):\n";
    for (auto const& child: result->tracks()->children())
    {
        if (auto track = dynamic_cast<otio::Track*>(child.value))
        {
            print_track(track);
        }
    }

    std::cout << "\nGlobal start time preserved: "
              << (result->global_start_time() ? "yes" : "no") << "\n";
    return 0;
}
