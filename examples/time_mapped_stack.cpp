// SPDX-License-Identifier: Apache-2.0
// Example for TimeMappedStack: one output curve drives both picture and sound.

#include <opentimelineio/clip.h>
#include <opentimelineio/errorStatus.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/serializableObject.h>
#include <opentimelineio/timeMappedStack.h>
#include <opentimelineio/track.h>

#include <iostream>
#include <string>

namespace o = opentimelineio::OPENTIMELINEIO_VERSION_NS;

namespace
{
o::RationalTime
frame(int n, double rate = 24.0)
{
    return o::RationalTime(n, rate);
}

o::TimeRange
frame_range(int start, int duration, double rate = 24.0)
{
    return o::TimeRange(frame(start, rate), frame(duration, rate));
}

int
fail(char const* what, o::ErrorStatus const& status)
{
    std::cerr << what << ": " << status.full_description << '\n';
    return 1;
}

void
print_samples(o::TimeMappedStack const& mapped, int output_frame)
{
    o::ErrorStatus status;
    auto samples = mapped.sample_at(frame(output_frame), &status);
    if (o::is_error(status))
    {
        std::cout << "output " << output_frame << ": ERROR "
                  << status.full_description << '\n';
        return;
    }

    std::cout << "output " << output_frame << ":";
    for (auto const& sample: samples)
    {
        std::string name = sample.clip ? sample.clip->name() : "<null>";
        std::cout << "  " << name << "@" << sample.source_time.value() << "/"
                  << sample.source_time.rate();
    }
    std::cout << '\n';
}
} // namespace

int
main()
{
    o::ErrorStatus status;

    o::SerializableObject::Retainer<o::Track> video(new o::Track("video"));
    auto* A = new o::Clip("A", nullptr, frame_range(100, 12));
    auto* B = new o::Clip("B", nullptr, frame_range(200, 12));
    if (!video->append_child(A, &status)
        || !video->append_child(B, &status))
    {
        return fail("append video clips", status);
    }

    o::SerializableObject::Retainer<o::Track> audio(new o::Track("audio"));
    auto* C = new o::Clip("C", nullptr, frame_range(500, 24));
    if (!audio->append_child(C, &status))
    {
        return fail("append audio clip", status);
    }

    o::SerializableObject::Retainer<o::TimeMappedStack> mapped(
        new o::TimeMappedStack("shared_curve"));
    if (!mapped->append_child(video.value, &status)
        || !mapped->append_child(audio.value, &status))
    {
        return fail("append tracks", status);
    }

    std::vector<std::pair<o::RationalTime, o::RationalTime>> map = {
        { frame(0), frame(0) },
        { frame(6), frame(12) },
        { frame(10), frame(12) },
        { frame(16), frame(0) },
    };
    if (!mapped->set_time_map(map, &status))
    {
        return fail("set time map", status);
    }

    std::cout << "duration frames=" << mapped->duration(&status).value()
              << '\n';
    for (int f: { 0, 6, 10, 13 })
    {
        print_samples(*mapped, f);
    }

    auto clips_5_11 = mapped->find_children<o::Clip>(
        &status,
        o::TimeRange(frame(5), frame(6)));
    if (o::is_error(status))
    {
        return fail("find [5,11)", status);
    }
    std::cout << "find output [5,11):";
    for (auto const& clip: clips_5_11)
    {
        std::cout << ' ' << clip->name();
    }
    std::cout << '\n';

    auto clips_6_10 = mapped->find_children<o::Clip>(
        &status,
        o::TimeRange(frame(6), frame(4)));
    if (o::is_error(status))
    {
        return fail("find [6,10)", status);
    }
    std::cout << "find output [6,10):";
    for (auto const& clip: clips_6_10)
    {
        std::cout << ' ' << clip->name();
    }
    std::cout << '\n';

    std::string json = mapped->to_json_string(&status);
    if (o::is_error(status) || json.empty())
    {
        return fail("serialize", status);
    }

    o::SerializableObject::Retainer<o::SerializableObject> reread(
        o::SerializableObject::from_json_string(json, &status));
    if (o::is_error(status) || !reread)
    {
        return fail("deserialize", status);
    }

    auto* reread_mapped =
        dynamic_cast<o::TimeMappedStack*>(reread.value);
    if (!reread_mapped)
    {
        std::cerr << "reread object lost TimeMappedStack dynamic type\n";
        return 1;
    }

    std::cout << "reread schema=" << reread_mapped->schema_name() << "."
              << reread_mapped->schema_version()
              << " points=" << reread_mapped->time_map().size() << '\n';
    for (int f: { 0, 6, 10, 13 })
    {
        print_samples(*reread_mapped, f);
    }

    // Put three frames of Gap before the mapped stack in an ordinary Track.
    o::SerializableObject::Retainer<o::Track> parent(new o::Track("parent"));
    if (!parent->append_child(new o::Gap(frame_range(0, 3)), &status)
        || !parent->append_child(mapped.value, &status))
    {
        return fail("build parent track", status);
    }
    std::cout << "parent duration frames="
              << parent->duration(&status).value() << '\n';

    return 0;
}
