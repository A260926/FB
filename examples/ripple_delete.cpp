// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

// Example: ripple delete a time range from a multi-track timeline.
//
// This example builds a small program in memory, deletes half a second of
// program time with algo::timeline_ripple_delete, and prints the resulting
// track layout.  It uses only the C++ API: no Python and no media access.

#include <opentimelineio/algo/timelineEditAlgorithm.h>

#include <opentimelineio/clip.h>
#include <opentimelineio/errorStatus.h>
#include <opentimelineio/externalReference.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/timeline.h>
#include <opentimelineio/track.h>

#include <iostream>

namespace otio = opentimelineio::OPENTIMELINEIO_VERSION_NS;
using R       = otio::RationalTime;
using T       = otio::TimeRange;

namespace {

otio::Clip*
make_clip(
    std::string const& name,
    double             source_start,
    double             duration,
    double             rate)
{
    // The media reference covers a large range so trimming never hits the
    // media's available range.
    auto* media =
        new otio::ExternalReference("file:///" + name, T(R(0, rate), R(100000, rate)));
    return new otio::Clip(
        name,
        media,
        T(R(source_start, rate), R(duration, rate)));
}

otio::Gap*
make_gap(double duration, double rate)
{
    return new otio::Gap(T(R(0, rate), R(duration, rate)));
}

void
print_track(otio::Track* track)
{
    std::cout << "track '" << track->name() << "' duration "
              << track->duration().to_seconds() << "s\n";
    for (auto const& child: track->children())
    {
        if (auto clip = dynamic_cast<otio::Clip*>(child.value))
        {
            auto range = clip->trimmed_range();
            std::cout << "  clip '" << clip->name() << "' source ["
                      << range.start_time().to_seconds() << ", "
                      << range.end_time_exclusive().to_seconds() << ")\n";
        }
        else if (auto gap = dynamic_cast<otio::Gap*>(child.value))
        {
            std::cout << "  gap " << gap->duration().to_seconds() << "s\n";
        }
        else
        {
            std::cout << "  " << child->schema_name()
                      << " duration " << child->duration().to_seconds()
                      << "s\n";
        }
    }
}

} // namespace

int
main()
{
    // 24 fps, 60 frames (2.5 seconds).
    auto* timeline = new otio::Timeline("example");

    auto* video = new otio::Track("video");
    video->append_child(make_clip("A", 100, 20, 24));
    video->append_child(make_gap(10, 24));
    video->append_child(make_clip("B", 200, 30, 24));
    timeline->tracks()->append_child(video);

    // A second track with its own source_range [80,120): it exposes a 40 frame
    // window, whose start maps to program zero.
    auto* second =
        new otio::Track("second", T(R(80, 24), R(40, 24)));
    second->append_child(make_clip("C", 500, 200, 24));
    timeline->tracks()->append_child(second);

    otio::SerializableObject::Retainer<otio::Timeline> input(timeline);

    std::cout << "before:\n";
    for (auto const& child: input->tracks()->children())
    {
        print_track(dynamic_cast<otio::Track*>(child.value));
    }

    // Delete program [10, 20) in frames, i.e. frames 10 through 19.
    otio::ErrorStatus status;
    otio::SerializableObject::Retainer<otio::Timeline> output(
        otio::algo::timeline_ripple_delete(
            input,
            T(R(10, 24), R(10, 24)),
            &status));

    if (!output || otio::is_error(status))
    {
        std::cerr << "ripple delete failed: " << status.full_description
                  << std::endl;
        return 1;
    }

    std::cout << "after:\n";
    for (auto const& child: output->tracks()->children())
    {
        print_track(dynamic_cast<otio::Track*>(child.value));
    }

    std::cout << "global_start_time preserved: "
              << (output->global_start_time().has_value() ? "yes" : "no")
              << std::endl;
    return 0;
}
