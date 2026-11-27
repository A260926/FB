// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

// Regression tests for algo::timeline_ripple_delete.
//
// These tests build timelines directly through the C++ API and verify the
// ripple delete behaviour without any Python or network dependence.

#include <opentimelineio/algo/timelineEditAlgorithm.h>

#include <opentimelineio/clip.h>
#include <opentimelineio/effect.h>
#include <opentimelineio/errorStatus.h>
#include <opentimelineio/externalReference.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/linearTimeWarp.h>
#include <opentimelineio/marker.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/timeline.h>
#include <opentimelineio/track.h>
#include <opentimelineio/transition.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

using namespace OTIO_NS;
using OTIOErrorStatus = OTIO_NS::ErrorStatus;
using R = RationalTime;
using T = TimeRange;

#define CHECK(expr)                                                          \
    do                                                                       \
    {                                                                        \
        if (!(expr))                                                         \
        {                                                                    \
            std::cerr << "FAILED " << __FILE__ << ":" << __LINE__ << ": "    \
                      << #expr << std::endl;                                 \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(actual, expected, tolerance)                              \
    do                                                                       \
    {                                                                        \
        double _actual   = (actual);                                         \
        double _expected = (expected);                                       \
        if (std::abs(_actual - _expected) > (tolerance))                     \
        {                                                                    \
            std::cerr << "FAILED " << __FILE__ << ":" << __LINE__ << ": "    \
                      << "expected " << _expected << " got " << _actual      \
                      << std::endl;                                          \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

namespace {

const double eps = 1e-9;

using RetTimeline = SerializableObject::Retainer<Timeline>;
using RetTrack    = SerializableObject::Retainer<Track>;
using RetStack    = SerializableObject::Retainer<Stack>;
using RetMarker   = SerializableObject::Retainer<Marker>;
using RetEffect   = SerializableObject::Retainer<Effect>;

double
sec(RationalTime const& t)
{
    return t.to_seconds();
}

void
check_span(TimeRange const& range, double start_s, double duration_s)
{
    CHECK_NEAR(sec(range.start_time()), start_s, eps);
    CHECK_NEAR(sec(range.duration()), duration_s, eps);
}

void
check_time(RationalTime const& actual, RationalTime const& expected)
{
    CHECK_NEAR(sec(actual), sec(expected), eps);
}

//! Clip with a large media available range so media never limits the trim.
Clip*
clip(
    std::string const& name,
    double             source_start,
    double             duration,
    double             rate)
{
    auto* media =
        new ExternalReference("file:///" + name, T(R(0, rate), R(100000, rate)));
    return new Clip(name, media, T(R(source_start, rate), R(duration, rate)));
}

Gap*
gap(double duration, double rate)
{
    return new Gap(T(R(0, rate), R(duration, rate)));
}

RetTimeline
build_timeline(std::string const& name, std::optional<R> global_start)
{
    return RetTimeline(new Timeline(name, global_start));
}

void
append_track(Timeline* timeline, Track* track)
{
    CHECK(timeline->tracks()->append_child(track));
}

std::string
json_of(Timeline const* timeline)
{
    OTIOErrorStatus status;
    auto                 json = timeline->to_json_string(&status);
    CHECK(!is_error(status));
    return json;
}

Track*
track_at(Timeline* timeline, size_t index)
{
    CHECK(index < timeline->tracks()->children().size());
    auto* track =
        dynamic_cast<Track*>(timeline->tracks()->children()[index].value);
    CHECK(track != nullptr);
    return track;
}

Clip*
clip_child(Track* track, size_t index)
{
    CHECK(index < track->children().size());
    auto* c = dynamic_cast<Clip*>(track->children()[index].value);
    CHECK(c != nullptr);
    return c;
}

Gap*
gap_child(Track* track, size_t index)
{
    CHECK(index < track->children().size());
    auto* g = dynamic_cast<Gap*>(track->children()[index].value);
    CHECK(g != nullptr);
    return g;
}

void
check_cleared_source_ranges(Timeline* timeline)
{
    for (auto const& child: timeline->tracks()->children())
    {
        auto* track = dynamic_cast<Track*>(child.value);
        CHECK(track != nullptr);
        CHECK(!track->source_range().has_value());
    }
}

//! The canonical multi-track example from the task description.
void
test_example_multitrack()
{
    RetTimeline input = build_timeline("program", std::nullopt);

    RetTrack video = new Track("video");
    CHECK(video->append_child(clip("A", 100, 20, 24)));
    CHECK(video->append_child(gap(10, 24)));
    CHECK(video->append_child(clip("B", 200, 30, 24)));
    append_track(input, video);

    RetTrack second =
        new Track("second", T(R(80, 24), R(40, 24)));
    CHECK(second->append_child(clip("C", 500, 200, 24)));
    append_track(input, second);

    const std::string before = json_of(input);

    OTIOErrorStatus status;
    RetTimeline output(
        algo::timeline_ripple_delete(input, T(R(10, 24), R(10, 24)), &status));
    CHECK(output != nullptr);
    CHECK(!is_error(status));

    CHECK(output->tracks()->children().size() == 2);
    check_cleared_source_ranges(output);

    Track* out_video = track_at(output, 0);
    CHECK(out_video->name() == "video");
    CHECK(out_video->children().size() == 3);
    check_span(
        clip_child(out_video, 0)->trimmed_range(),
        100.0 / 24.0,
        10.0 / 24.0);
    check_span(gap_child(out_video, 1)->trimmed_range(), 0.0, 10.0 / 24.0);
    check_span(
        clip_child(out_video, 2)->trimmed_range(),
        200.0 / 24.0,
        30.0 / 24.0);
    CHECK_NEAR(sec(out_video->duration()), 50.0 / 24.0, eps);

    Track* out_second = track_at(output, 1);
    CHECK(out_second->name() == "second");
    CHECK(out_second->children().size() == 3);
    check_span(
        clip_child(out_second, 0)->trimmed_range(),
        580.0 / 24.0,
        10.0 / 24.0);
    check_span(
        clip_child(out_second, 1)->trimmed_range(),
        600.0 / 24.0,
        20.0 / 24.0);
    check_span(gap_child(out_second, 2)->trimmed_range(), 0.0, 20.0 / 24.0);
    CHECK_NEAR(sec(out_second->duration()), 50.0 / 24.0, eps);

    CHECK(json_of(input) == before);
}

//! Fractional cut across 24 fps, 25 fps and 48 kHz material.  No integer-frame
//! rounding is allowed anywhere.
void
test_fractional_mixed_rates()
{
    RetTimeline input = build_timeline("mixed", std::nullopt);

    // Program length: exactly 5 seconds on every track.
    RetTrack video = new Track("video24");
    CHECK(video->append_child(clip("v1", 0, 120, 24)));
    append_track(input, video);

    RetTrack audio = new Track("audio48k", std::nullopt, Track::Kind::audio);
    CHECK(audio->append_child(clip("a1", 0, 240000, 48000)));
    append_track(input, audio);

    RetTrack film = new Track("film25");
    CHECK(film->append_child(clip("f1", 0, 125, 25)));
    append_track(input, film);

    const std::string before = json_of(input);

    // 10.5 frames at 24 fps through 13.25 frames: a genuinely fractional cut.
    const T cut(R(10.5, 24), R(2.75, 24));

    OTIOErrorStatus status;
    RetTimeline output(algo::timeline_ripple_delete(input, cut, &status));
    CHECK(output != nullptr);
    CHECK(!is_error(status));

    const double a             = 10.5 / 24.0;
    const double b             = 13.25 / 24.0;
    const double result_length = 5.0 - (b - a);

    Track* out_v = track_at(output, 0);
    CHECK(out_v->children().size() == 2);
    check_span(clip_child(out_v, 0)->trimmed_range(), 0.0, a);
    check_span(clip_child(out_v, 1)->trimmed_range(), b, 5.0 - b);
    CHECK_NEAR(sec(out_v->duration()), result_length, eps);

    // 48 kHz: the cut lands exactly on 21000 and 26500 samples.
    Track* out_a = track_at(output, 1);
    CHECK(out_a->children().size() == 2);
    check_span(clip_child(out_a, 0)->trimmed_range(), 0.0, a);
    check_span(clip_child(out_a, 1)->trimmed_range(), b, 5.0 - b);
    CHECK_NEAR(
        sec(clip_child(out_a, 0)->trimmed_range().duration()) * 48000.0,
        21000.0,
        eps);
    CHECK_NEAR(
        sec(clip_child(out_a, 1)->trimmed_range().start_time()) * 48000.0,
        26500.0,
        eps);
    CHECK_NEAR(sec(out_a->duration()), result_length, eps);

    // 25 fps: fractional frame values survive unscaled.
    Track* out_f = track_at(output, 2);
    CHECK(out_f->children().size() == 2);
    check_span(clip_child(out_f, 0)->trimmed_range(), 0.0, a);
    check_span(clip_child(out_f, 1)->trimmed_range(), b, 5.0 - b);
    CHECK_NEAR(sec(out_f->duration()), result_length, eps);

    CHECK(json_of(input) == before);
}

//! A nested Stack crossing the cut stays hierarchical; only its visible item
//! window is retrimmed, and internal markers keep source coordinates.
void
test_nested_object_crosses_cut()
{
    RetTimeline input = build_timeline("nested", std::nullopt);

    RetTrack track = new Track("video");
    CHECK(track->append_child(gap(10, 24)));

    RetTrack nested = new Track("nested");
    CHECK(nested->append_child(clip("inner_clip", 50, 10, 24)));
    CHECK(nested->append_child(gap(20, 24)));
    RetMarker inner_marker =
        new Marker("inner", T(R(3, 24), R(2, 24)), Color::red);
    nested->markers().push_back(inner_marker);
    CHECK(track->append_child(nested));

    CHECK(track->append_child(clip("tail", 300, 20, 24)));
    append_track(input, track);

    const std::string before = json_of(input);

    // Program: gap [0,10), nested [10,40), tail [40,60).  Delete [15,25).
    OTIOErrorStatus status;
    RetTimeline output(
        algo::timeline_ripple_delete(input, T(R(15, 24), R(10, 24)), &status));
    CHECK(output != nullptr);
    CHECK(!is_error(status));

    Track* out = track_at(output, 0);
    CHECK_NEAR(sec(out->duration()), 50.0 / 24.0, eps);
    CHECK(out->children().size() == 4);
    check_span(gap_child(out, 0)->trimmed_range(), 0.0, 10.0 / 24.0);

    // Left side: nested trimmed to [10,15) local -> source [0,5).
    auto* left_nested = dynamic_cast<Track*>(out->children()[1].value);
    CHECK(left_nested != nullptr);
    CHECK(left_nested->name() == "nested");
    CHECK(left_nested->children().size() == 2);
    CHECK(left_nested->source_range().has_value());
    check_span(left_nested->source_range().value(), 0.0, 5.0 / 24.0);

    // Right side: nested trimmed to [25,40) local -> source [15,30).
    auto* right_nested = dynamic_cast<Track*>(out->children()[2].value);
    CHECK(right_nested != nullptr);
    CHECK(right_nested->name() == "nested");
    CHECK(right_nested->children().size() == 2);
    CHECK(right_nested->source_range().has_value());
    check_span(right_nested->source_range().value(), 15.0 / 24.0, 15.0 / 24.0);

    check_span(
        clip_child(out, 3)->trimmed_range(),
        300.0 / 24.0,
        20.0 / 24.0);

    // The two nested copies and their children are independent objects.
    CHECK(left_nested != right_nested);
    CHECK(
        dynamic_cast<Clip*>(left_nested->children()[0].value)
        != dynamic_cast<Clip*>(right_nested->children()[0].value));

    // Internal markers keep their original source coordinates on both sides.
    CHECK(left_nested->markers().size() == 1);
    CHECK(right_nested->markers().size() == 1);
    check_span(
        left_nested->markers()[0]->marked_range(),
        3.0 / 24.0,
        2.0 / 24.0);
    check_span(
        right_nested->markers()[0]->marked_range(),
        3.0 / 24.0,
        2.0 / 24.0);
    CHECK(left_nested->markers()[0].value != right_nested->markers()[0].value);
    CHECK(left_nested->markers()[0]->name() == "inner");
    CHECK(left_nested->markers()[0]->color().has_value());

    CHECK(json_of(input) == before);
}

//! Short and empty tracks are padded; zero-length gaps are never emitted.
void
test_short_and_empty_tracks()
{
    RetTimeline input = build_timeline("short", std::nullopt);

    RetTrack long_track = new Track("long");
    CHECK(long_track->append_child(clip("A", 0, 60, 24)));
    append_track(input, long_track);

    RetTrack short_track = new Track("short");
    CHECK(short_track->append_child(clip("S", 0, 10, 24)));
    append_track(input, short_track);

    RetTrack empty_track = new Track("empty");
    append_track(input, empty_track);

    const std::string before = json_of(input);

    OTIOErrorStatus status;
    RetTimeline output(
        algo::timeline_ripple_delete(input, T(R(10, 24), R(10, 24)), &status));
    CHECK(output != nullptr);
    CHECK(!is_error(status));

    // Long track: one clip split into [0,10) and [20,60).
    Track* out_long = track_at(output, 0);
    CHECK(out_long->children().size() == 2);
    check_span(clip_child(out_long, 0)->trimmed_range(), 0.0, 10.0 / 24.0);
    check_span(clip_child(out_long, 1)->trimmed_range(), 20.0 / 24.0, 40.0 / 24.0);
    CHECK_NEAR(sec(out_long->duration()), 50.0 / 24.0, eps);

    // Short track: its 10-frame clip is untouched, then a 40-frame gap.
    Track* out_short = track_at(output, 1);
    CHECK(out_short->children().size() == 2);
    check_span(clip_child(out_short, 0)->trimmed_range(), 0.0, 10.0 / 24.0);
    check_span(gap_child(out_short, 1)->trimmed_range(), 0.0, 40.0 / 24.0);

    // Empty track: a single 50-frame gap, never a zero-length one.
    Track* out_empty = track_at(output, 2);
    CHECK(out_empty->children().size() == 1);
    check_span(gap_child(out_empty, 0)->trimmed_range(), 0.0, 50.0 / 24.0);

    for (auto* t: { out_long, out_short, out_empty })
    {
        CHECK_NEAR(sec(t->duration()), 50.0 / 24.0, eps);
        CHECK(!t->source_range().has_value());
    }

    CHECK(json_of(input) == before);
}

//! Deleting the entire program keeps (now empty) tracks and project info.
void
test_delete_entire_program()
{
    RetTimeline input = build_timeline("whole", R(0, 24));
    input->metadata()["studio"] = std::string("studio-x");

    RetTrack track = new Track("video");
    CHECK(track->append_child(clip("A", 0, 60, 24)));
    append_track(input, track);

    RetTrack audio = new Track("audio", std::nullopt, Track::Kind::audio);
    CHECK(audio->append_child(clip("B", 0, 60, 24)));
    append_track(input, audio);

    const std::string before = json_of(input);

    OTIOErrorStatus status;
    RetTimeline output(
        algo::timeline_ripple_delete(input, T(R(0, 24), R(60, 24)), &status));
    CHECK(output != nullptr);
    CHECK(!is_error(status));

    CHECK(output->tracks()->children().size() == 2);
    check_cleared_source_ranges(output);
    for (auto const& child: output->tracks()->children())
    {
        auto* track = dynamic_cast<Track*>(child.value);
        CHECK(track != nullptr);
        CHECK(track->children().empty());
        CHECK_NEAR(sec(track->duration()), 0.0, eps);
    }
    CHECK(output->name() == "whole");
    CHECK(output->global_start_time().has_value());
    CHECK_NEAR(sec(output->global_start_time().value()), 0.0, eps);
    std::string studio;
    CHECK(output->metadata().get_if_set("studio", &studio));
    CHECK(studio == "studio-x");

    CHECK(json_of(input) == before);
}

//! Stack markers (program time) and Track markers (local time) both follow
//! clip-then-delete then shift, with interval markers split in two.
void
test_markers_intervals_and_points()
{
    RetTimeline input = build_timeline("markers", std::nullopt);

    // Stack markers are in program time.  Program length is 60 frames.
    RetMarker stack_cross =
        new Marker("stack_cross", T(R(5, 24), R(20, 24)), Color::green);
    stack_cross->metadata()["tag"] = std::string("cross-tag");
    RetMarker stack_point_a =
        new Marker("stack_point_a", T(R(10, 24), R(0, 24)), Color::cyan);
    RetMarker stack_point_b =
        new Marker("stack_point_b", T(R(20, 24), R(0, 24)), Color::orange);
    RetMarker stack_point_in =
        new Marker("stack_point_in", T(R(15, 24), R(0, 24)), Color::purple);
    RetMarker stack_point_end =
        new Marker("stack_point_end", T(R(60, 24), R(0, 24)), Color::blue);
    RetMarker stack_after =
        new Marker("stack_after", T(R(30, 24), R(5, 24)), Color::red);
    input->tracks()->markers().push_back(stack_cross);
    input->tracks()->markers().push_back(stack_point_a);
    input->tracks()->markers().push_back(stack_point_b);
    input->tracks()->markers().push_back(stack_point_in);
    input->tracks()->markers().push_back(stack_point_end);
    input->tracks()->markers().push_back(stack_after);

    // Track source_range [4,50) local; exposed program [0,46).
    RetTrack track = new Track("video", T(R(4, 24), R(46, 24)));
    CHECK(track->append_child(clip("A", 0, 60, 24)));
    // Track markers are local (pre-trim): [4,14) local -> program [0,10).
    RetMarker track_cross =
        new Marker("track_cross", T(R(4, 24), R(10, 24)), Color::yellow);
    // Local point 23 -> program 19, inside the cut.
    RetMarker track_point =
        new Marker("track_point", T(R(23, 24), R(0, 24)), Color::magenta);
    // Local point 44 -> program 40, after the cut; it should move back to
    // program 30, i.e. local 30 in the output track.
    RetMarker track_after =
        new Marker("track_after", T(R(44, 24), R(0, 24)), Color::orange);
    track->markers().push_back(track_cross);
    track->markers().push_back(track_point);
    track->markers().push_back(track_after);
    append_track(input, track);

    RetTrack long_track = new Track("long");
    CHECK(long_track->append_child(clip("L", 0, 60, 24)));
    append_track(input, long_track);

    const std::string before = json_of(input);

    OTIOErrorStatus status;
    RetTimeline output(
        algo::timeline_ripple_delete(input, T(R(10, 24), R(10, 24)), &status));
    CHECK(output != nullptr);
    CHECK(!is_error(status));

    // Stack markers: crossing interval -> left [5,10) and right [10,15);
    // point at a gone; point at b moved to a; interior point gone; end point
    // dropped; later interval shifted back by 10.
    auto const& stack_markers = output->tracks()->markers();
    CHECK(stack_markers.size() == 4);
    CHECK(stack_markers[0]->name() == "stack_cross");
    check_span(stack_markers[0]->marked_range(), 5.0 / 24.0, 5.0 / 24.0);
    CHECK(stack_markers[1]->name() == "stack_cross");
    check_span(stack_markers[1]->marked_range(), 10.0 / 24.0, 5.0 / 24.0);
    CHECK(stack_markers[2]->name() == "stack_point_b");
    check_span(stack_markers[2]->marked_range(), 10.0 / 24.0, 0.0);
    CHECK(stack_markers[3]->name() == "stack_after");
    check_span(stack_markers[3]->marked_range(), 20.0 / 24.0, 5.0 / 24.0);
    CHECK(stack_markers[0].value != stack_markers[1].value);
    CHECK(stack_markers[0]->color().has_value());
    CHECK(stack_markers[0]->color().value() == Color::green);
    for (auto const& marker: stack_markers)
    {
        std::string tag;
        if (marker->name() == "stack_cross")
        {
            CHECK(marker->metadata().get_if_set("tag", &tag));
            CHECK(tag == "cross-tag");
        }
    }

    // Track markers: [0,10) survives unchanged (touching the cut end), the
    // interior point is removed, and the post-cut point moves back by 10
    // frames in program time.  Output coordinates are program time.
    auto const& track_markers = track_at(output, 0)->markers();
    CHECK(track_markers.size() == 2);
    CHECK(track_markers[0]->name() == "track_cross");
    check_span(track_markers[0]->marked_range(), 0.0, 10.0 / 24.0);
    CHECK(track_markers[1]->name() == "track_after");
    check_span(track_markers[1]->marked_range(), 30.0 / 24.0, 0.0);

    CHECK(json_of(input) == before);
}

//! Points exactly on a are deleted, on b move to a, and on the exclusive
//! visible end are dropped.
void
test_marker_boundaries()
{
    RetTimeline input = build_timeline("bounds", std::nullopt);

    RetTrack track = new Track("video");
    CHECK(track->append_child(clip("A", 0, 40, 24)));
    track->markers().push_back(
        new Marker("at_a", T(R(5, 24), R(0, 24))));
    track->markers().push_back(
        new Marker("at_b", T(R(15, 24), R(0, 24))));
    track->markers().push_back(
        new Marker("at_end", T(R(40, 24), R(0, 24))));
    track->markers().push_back(
        new Marker("just_before_end", T(R(39, 24), R(1, 24))));
    append_track(input, track);

    OTIOErrorStatus status;
    RetTimeline output(
        algo::timeline_ripple_delete(input, T(R(5, 24), R(10, 24)), &status));
    CHECK(output != nullptr);
    CHECK(!is_error(status));

    auto const& markers = track_at(output, 0)->markers();
    CHECK(markers.size() == 2);
    CHECK(markers[0]->name() == "at_b");
    check_span(markers[0]->marked_range(), 5.0 / 24.0, 0.0);
    CHECK(markers[1]->name() == "just_before_end");
    check_span(markers[1]->marked_range(), 29.0 / 24.0, 1.0 / 24.0);
}

//! Transitions: kept when fully retained, touching endpoints allowed,
//! positive overlap fails, a truncating track range fails, invisible dropped.
void
test_transitions()
{
    // Case 1: transition away from the cut keeps offsets.
    RetTimeline input = build_timeline("tr_ok", std::nullopt);
    RetTrack track = new Track("video");
    CHECK(track->append_child(clip("A", 0, 20, 24)));
    auto* tr = new Transition(
        "dissolve",
        Transition::Type::SMPTE_Dissolve,
        R(2, 24),
        R(2, 24));
    CHECK(track->append_child(tr));
    CHECK(track->append_child(clip("B", 20, 40, 24)));
    append_track(input, track);

    const std::string before = json_of(input);

    OTIOErrorStatus status;
    // Transition spans [18,22); cut [30,40) never touches it.
    RetTimeline output(
        algo::timeline_ripple_delete(input, T(R(30, 24), R(10, 24)), &status));
    CHECK(output != nullptr);
    CHECK(!is_error(status));
    Track* out = track_at(output, 0);
    CHECK(out->children().size() == 4);
    auto* out_tr = dynamic_cast<Transition*>(out->children()[1].value);
    CHECK(out_tr != nullptr);
    check_time(out_tr->in_offset(), R(2, 24));
    check_time(out_tr->out_offset(), R(2, 24));
    CHECK(out_tr->transition_type() == Transition::Type::SMPTE_Dissolve);
    CHECK_NEAR(sec(out->duration()), 50.0 / 24.0, eps);
    CHECK(json_of(input) == before);

    // Case 2: a transition whose end touches the cut start is allowed.
    RetTimeline touch = build_timeline("tr_touch", std::nullopt);
    RetTrack touch_track = new Track("video");
    CHECK(touch_track->append_child(clip("A", 0, 20, 24)));
    CHECK(touch_track->append_child(new Transition(
        "t",
        Transition::Type::SMPTE_Dissolve,
        R(0, 24),
        R(4, 24))));
    CHECK(touch_track->append_child(clip("B", 20, 40, 24)));
    append_track(touch, touch_track);

    // Transition spans [20,24); cut starts at 24, touching only its end.
    OTIOErrorStatus touch_status;
    RetTimeline touch_out(algo::timeline_ripple_delete(
        touch,
        T(R(24, 24), R(10, 24)),
        &touch_status));
    CHECK(touch_out != nullptr);
    CHECK(!is_error(touch_status));
    CHECK(track_at(touch_out, 0)->children().size() == 4);
    CHECK_NEAR(sec(track_at(touch_out, 0)->duration()), 50.0 / 24.0, eps);

    // Case 2b: a transition whose start touches the cut end survives on the
    // right-hand side.
    RetTimeline touch2 = build_timeline("tr_touch2", std::nullopt);
    RetTrack touch2_track = new Track("video");
    CHECK(touch2_track->append_child(clip("A", 0, 20, 24)));
    CHECK(touch2_track->append_child(new Transition(
        "t",
        Transition::Type::SMPTE_Dissolve,
        R(4, 24),
        R(0, 24)))); // spans [16,20)
    CHECK(touch2_track->append_child(clip("B", 20, 40, 24)));
    append_track(touch2, touch2_track);

    // Cut [10,16) ends exactly where the transition starts.
    OTIOErrorStatus touch2_status;
    RetTimeline touch2_out(algo::timeline_ripple_delete(
        touch2,
        T(R(10, 24), R(6, 24)),
        &touch2_status));
    CHECK(touch2_out != nullptr);
    CHECK(!is_error(touch2_status));
    CHECK(track_at(touch2_out, 0)->children().size() == 4);
    bool touch2_kept = false;
    for (auto const& child: track_at(touch2_out, 0)->children())
    {
        if (dynamic_cast<Transition*>(child.value))
        {
            touch2_kept = true;
        }
    }
    CHECK(touch2_kept);
    CHECK_NEAR(sec(track_at(touch2_out, 0)->duration()), 54.0 / 24.0, eps);

    // Case 2c: a cut at the very head keeps a transition that lies later on
    // the single surviving right side.
    RetTimeline head = build_timeline("tr_head", std::nullopt);
    RetTrack head_track = new Track("video");
    CHECK(head_track->append_child(clip("A", 0, 20, 24)));
    CHECK(head_track->append_child(new Transition(
        "t",
        Transition::Type::SMPTE_Dissolve,
        R(2, 24),
        R(2, 24)))); // spans [18,22)
    CHECK(head_track->append_child(clip("B", 20, 40, 24)));
    append_track(head, head_track);

    OTIOErrorStatus head_status;
    RetTimeline head_out(algo::timeline_ripple_delete(
        head,
        T(R(0, 24), R(10, 24)),
        &head_status));
    CHECK(head_out != nullptr);
    CHECK(!is_error(head_status));
    CHECK(track_at(head_out, 0)->children().size() == 3);
    CHECK(
        dynamic_cast<Transition*>(track_at(head_out, 0)->children()[1].value)
        != nullptr);
    CHECK_NEAR(sec(track_at(head_out, 0)->duration()), 50.0 / 24.0, eps);

    // Case 2d: a cut at the very tail keeps an earlier transition on the
    // single surviving left side.
    RetTimeline tail = build_timeline("tr_tail", std::nullopt);
    RetTrack tail_track = new Track("video");
    CHECK(tail_track->append_child(clip("A", 0, 20, 24)));
    CHECK(tail_track->append_child(new Transition(
        "t",
        Transition::Type::SMPTE_Dissolve,
        R(2, 24),
        R(2, 24))));
    CHECK(tail_track->append_child(clip("B", 20, 40, 24)));
    append_track(tail, tail_track);

    OTIOErrorStatus tail_status;
    RetTimeline tail_out(algo::timeline_ripple_delete(
        tail,
        T(R(50, 24), R(10, 24)),
        &tail_status));
    CHECK(tail_out != nullptr);
    CHECK(!is_error(tail_status));
    CHECK(track_at(tail_out, 0)->children().size() == 3);
    CHECK(
        dynamic_cast<Transition*>(track_at(tail_out, 0)->children()[1].value)
        != nullptr);
    CHECK_NEAR(sec(track_at(tail_out, 0)->duration()), 50.0 / 24.0, eps);

    // Case 3: a positive overlap with the cut fails.
    RetTimeline bad = build_timeline("tr_bad", std::nullopt);
    RetTrack bad_track = new Track("video");
    CHECK(bad_track->append_child(clip("A", 0, 20, 24)));
    CHECK(bad_track->append_child(new Transition(
        "t",
        Transition::Type::SMPTE_Dissolve,
        R(2, 24),
        R(2, 24))));
    CHECK(bad_track->append_child(clip("B", 20, 40, 24)));
    append_track(bad, bad_track);

    const std::string bad_before = json_of(bad);
    OTIOErrorStatus bad_status;
    RetTimeline bad_out(algo::timeline_ripple_delete(
        bad,
        T(R(19, 24), R(4, 24)),
        &bad_status));
    CHECK(bad_out == nullptr);
    CHECK(is_error(bad_status));
    CHECK(bad_status.outcome == OTIOErrorStatus::CANNOT_TRIM_TRANSITION);
    CHECK(json_of(bad) == bad_before);

    // Case 4: a track source_range that truncates a transition fails.
    RetTimeline trunc = build_timeline("tr_trunc", std::nullopt);
    RetTrack trunc_track =
        new Track("video", T(R(0, 24), R(21, 24)));
    CHECK(trunc_track->append_child(clip("A", 0, 20, 24)));
    CHECK(trunc_track->append_child(new Transition(
        "t",
        Transition::Type::SMPTE_Dissolve,
        R(2, 24),
        R(2, 24))));
    CHECK(trunc_track->append_child(clip("B", 20, 40, 24)));
    append_track(trunc, trunc_track);

    OTIOErrorStatus trunc_status;
    RetTimeline trunc_out(algo::timeline_ripple_delete(
        trunc,
        T(R(1, 24), R(4, 24)),
        &trunc_status));
    CHECK(trunc_out == nullptr);
    CHECK(is_error(trunc_status));
    CHECK(trunc_status.outcome == OTIOErrorStatus::CANNOT_TRIM_TRANSITION);

    // Case 5: a transition fully outside the visible range is dropped.
    RetTimeline hidden = build_timeline("tr_hidden", std::nullopt);
    RetTrack hidden_track =
        new Track("video", T(R(30, 24), R(20, 24))); // program [0,20)
    CHECK(hidden_track->append_child(clip("A", 0, 20, 24)));
    CHECK(hidden_track->append_child(new Transition(
        "t",
        Transition::Type::SMPTE_Dissolve,
        R(2, 24),
        R(2, 24)))); // range [18,22), outside [30,50)
    CHECK(hidden_track->append_child(clip("B", 20, 20, 24)));
    append_track(hidden, hidden_track);

    OTIOErrorStatus hidden_status;
    RetTimeline hidden_out(algo::timeline_ripple_delete(
        hidden,
        T(R(5, 24), R(5, 24)),
        &hidden_status));
    CHECK(hidden_out != nullptr);
    CHECK(!is_error(hidden_status));
    bool has_transition = false;
    for (auto const& child: track_at(hidden_out, 0)->children())
    {
        if (dynamic_cast<Transition*>(child.value))
        {
            has_transition = true;
        }
    }
    CHECK(!has_transition);

    // Case 6: a transition nested inside a nested composition is preserved
    // with its object; only the outer item's visible window changes.
    RetTimeline nested_tr = build_timeline("tr_nested", std::nullopt);
    RetTrack    outer     = new Track("video");
    CHECK(outer->append_child(clip("head", 0, 20, 24)));
    RetTrack inner = new Track("inner_track");
    CHECK(inner->append_child(clip("A", 0, 10, 24)));
    CHECK(inner->append_child(new Transition(
        "t",
        Transition::Type::SMPTE_Dissolve,
        R(1, 24),
        R(1, 24)))); // spans local [9,11)
    CHECK(inner->append_child(clip("B", 10, 10, 24)));
    CHECK(outer->append_child(inner)); // program [20,40)
    append_track(nested_tr, outer);

    const std::string nested_before = json_of(nested_tr);
    OTIOErrorStatus   nested_status;
    RetTimeline       nested_out(algo::timeline_ripple_delete(
        nested_tr,
        T(R(5, 24), R(10, 24)),
        &nested_status));
    CHECK(nested_out != nullptr);
    CHECK(!is_error(nested_status));
    Track* out_outer = track_at(nested_out, 0);
    // Left side keeps head [0,5); right side keeps head [10,20) then inner.
    CHECK(out_outer->children().size() == 3);
    auto* out_inner = dynamic_cast<Track*>(out_outer->children()[2].value);
    CHECK(out_inner != nullptr);
    CHECK(out_inner->children().size() == 3);
    CHECK(out_inner->children()[1].value
          != inner->children()[1].value);
    CHECK(
        dynamic_cast<Transition*>(out_inner->children()[1].value)
        != nullptr);
    CHECK(json_of(nested_tr) == nested_before);
}

//! The result is a deep, independent copy: mutating it cannot touch the input.
void
test_deep_copy_independence()
{
    RetTimeline input = build_timeline("deep", std::nullopt);

    RetTrack track  = new Track("video");
    auto*    source = clip("A", 0, 60, 24);
    source->metadata()["tag"] = std::string("original");
    source->effects().push_back(new Effect("blur", "Blur"));
    source->markers().push_back(
        new Marker("clip_marker", T(R(1, 24), R(2, 24)), Color::red));
    CHECK(track->append_child(source));
    append_track(input, track);

    const std::string before = json_of(input);

    OTIOErrorStatus status;
    RetTimeline output(
        algo::timeline_ripple_delete(input, T(R(10, 24), R(10, 24)), &status));
    CHECK(output != nullptr);
    CHECK(!is_error(status));

    Track* out   = track_at(output, 0);
    auto*  out_a = clip_child(out, 0);
    Track* in_track = track_at(input, 0);
    auto*  in_a  = clip_child(in_track, 0);

    // Distinct objects at every level.
    CHECK(out_a != in_a);
    CHECK(out->children()[0].value != in_track->children()[0].value);
    CHECK(out_a->effects().size() == 1);
    CHECK(out_a->effects()[0].value != in_a->effects()[0].value);
    CHECK(out_a->markers().size() == 1);
    CHECK(out_a->markers()[0].value != in_a->markers()[0].value);

    // Mutating the output leaves the input untouched.
    out_a->set_name("changed");
    out_a->metadata()["tag"]       = std::string("changed");
    out_a->effects()[0]->set_name("changed-effect");
    out_a->markers()[0]->set_name("changed-marker");
    out_a->markers()[0]->set_marked_range(T(R(9, 24), R(1, 24)));
    CHECK(in_a->name() == "A");
    check_span(in_a->markers()[0]->marked_range(), 1.0 / 24.0, 2.0 / 24.0);
    CHECK(json_of(input) == before);
}

//! A JSON write/read round trip reproduces the edited timeline.
void
test_json_roundtrip()
{
    RetTimeline input = build_timeline("roundtrip", R(1, 24));

    RetTrack video = new Track("video");
    CHECK(video->append_child(clip("A", 0, 20, 24)));
    CHECK(video->append_child(gap(10, 24)));
    RetStack nested = new Stack("nested");
    CHECK(nested->append_child(clip("N", 100, 10, 24)));
    CHECK(nested->append_child(gap(10, 24)));
    CHECK(video->append_child(nested));
    CHECK(video->append_child(clip("B", 200, 20, 24)));
    append_track(input, video);

    RetTrack audio = new Track("audio", std::nullopt, Track::Kind::audio);
    CHECK(audio->append_child(clip("C", 0, 1000, 48000)));
    append_track(input, audio);

    OTIOErrorStatus status;
    RetTimeline output(
        algo::timeline_ripple_delete(input, T(R(15, 24), R(5, 24)), &status));
    CHECK(output != nullptr);
    CHECK(!is_error(status));

    const std::string   json = json_of(output);
    OTIOErrorStatus reread_status;
    SerializableObject* reread_raw =
        SerializableObject::from_json_string(json, &reread_status);
    CHECK(reread_raw != nullptr);
    CHECK(!is_error(reread_status));
    SerializableObject::Retainer<> reread_guard(reread_raw);
    auto* reread = dynamic_cast<Timeline*>(reread_raw);
    CHECK(reread != nullptr);
    CHECK(reread->is_equivalent_to(*output));
    CHECK(reread->global_start_time().has_value());
    check_time(reread->global_start_time().value(), R(1, 24));
    CHECK(reread->tracks()->children().size() == 2);

    // The input was not disturbed by the round trip.
    CHECK(json_of(input).find("roundtrip") != std::string::npos);
}

//! An empty cut validates structure and range, then returns a fully
//! independent copy of the original project.
void
test_empty_cut_copy()
{
    RetTimeline input = build_timeline("empty_cut", R(2, 25));

    RetTrack video = new Track("video");
    CHECK(video->append_child(clip("A", 0, 30, 24)));
    CHECK(video->append_child(gap(10, 24)));
    CHECK(video->append_child(clip("B", 200, 20, 24)));
    append_track(input, video);

    RetTrack with_range =
        new Track("with_range", T(R(5, 24), R(20, 24)));
    CHECK(with_range->append_child(clip("C", 100, 40, 24)));
    append_track(input, with_range);

    const std::string before = json_of(input);

    OTIOErrorStatus status;
    RetTimeline output(
        algo::timeline_ripple_delete(input, T(R(12, 24), R(0, 24)), &status));
    CHECK(output != nullptr);
    CHECK(!is_error(status));

    CHECK(output->tracks()->children().size() == 2);

    // An empty cut returns a complete copy of the original project, so the
    // tracks, their source_ranges and their children all stay as they were.
    Track* out_video = track_at(output, 0);
    CHECK(out_video->children().size() == 3);
    CHECK(!out_video->source_range().has_value());
    check_span(clip_child(out_video, 0)->trimmed_range(), 0.0, 30.0 / 24.0);
    check_span(gap_child(out_video, 1)->trimmed_range(), 0.0, 10.0 / 24.0);
    check_span(
        clip_child(out_video, 2)->trimmed_range(),
        200.0 / 24.0,
        20.0 / 24.0);

    Track* out_r = track_at(output, 1);
    CHECK(out_r->children().size() == 1);
    CHECK(out_r->source_range().has_value());
    check_span(out_r->source_range().value(), 5.0 / 24.0, 20.0 / 24.0);

    // It is an independent copy, not aliased to the input.
    CHECK(out_video != track_at(input, 0));
    CHECK(clip_child(out_video, 0) != clip_child(track_at(input, 0), 0));

    clip_child(out_video, 0)->set_name("changed");
    CHECK(json_of(input) == before);

    // An empty cut preserves transitions regardless of where the (empty) cut
    // sits, including transitions after its position.
    RetTimeline with_transition = build_timeline("empty_tr", std::nullopt);
    RetTrack    t               = new Track("video");
    CHECK(t->append_child(clip("A", 0, 20, 24)));
    CHECK(t->append_child(new Transition(
        "t",
        Transition::Type::SMPTE_Dissolve,
        R(2, 24),
        R(2, 24)))); // spans [18,22)
    CHECK(t->append_child(clip("B", 20, 40, 24)));
    append_track(with_transition, t);

    OTIOErrorStatus empty_tr_status;
    RetTimeline empty_tr_out(algo::timeline_ripple_delete(
        with_transition,
        T(R(0, 24), R(0, 24)),
        &empty_tr_status));
    CHECK(empty_tr_out != nullptr);
    CHECK(!is_error(empty_tr_status));
    Track* out_t = track_at(empty_tr_out, 0);
    CHECK(out_t->children().size() == 3);
    auto* kept = dynamic_cast<Transition*>(out_t->children()[1].value);
    CHECK(kept != nullptr);
    check_time(kept->in_offset(), R(2, 24));
    check_time(kept->out_offset(), R(2, 24));
}

//! Invalid arguments and unsupported structure fail and leave input alone.
void
test_invalid_inputs()
{
    RetTimeline input = build_timeline("valid", std::nullopt);
    RetTrack track = new Track("video");
    CHECK(track->append_child(clip("A", 0, 60, 24)));
    append_track(input, track);
    const std::string before = json_of(input);

    auto expect_failure = [&](T const&             cut,
                              OTIOErrorStatus::Outcome expected) {
        OTIOErrorStatus status;
        RetTimeline out(algo::timeline_ripple_delete(input, cut, &status));
        CHECK(out == nullptr);
        CHECK(is_error(status));
        if (expected != OTIOErrorStatus::INTERNAL_ERROR)
        {
            CHECK(status.outcome == expected);
        }
        CHECK(json_of(input) == before);
    };

    // Null input.
    {
        OTIOErrorStatus status;
        RetTimeline out(algo::timeline_ripple_delete(
            nullptr,
            T(R(1, 24), R(1, 24)),
            &status));
        CHECK(out == nullptr);
        CHECK(is_error(status));
    }

    // Negative start, negative duration, and beyond the program length.
    expect_failure(T(R(-1, 24), R(2, 24)), OTIOErrorStatus::INVALID_TIME_RANGE);
    expect_failure(T(R(5, 24), R(-2, 24)), OTIOErrorStatus::INVALID_TIME_RANGE);
    expect_failure(T(R(55, 24), R(10, 24)), OTIOErrorStatus::INVALID_TIME_RANGE);

    // Non-finite values and rates.
    expect_failure(
        T(R(1, 24), R(1, std::numeric_limits<double>::infinity())),
        OTIOErrorStatus::INVALID_TIME_RANGE);
    expect_failure(
        T(R(std::nan(""), 24), R(1, 24)),
        OTIOErrorStatus::INVALID_TIME_RANGE);
    expect_failure(T(R(1, 0), R(1, 1)), OTIOErrorStatus::INVALID_TIME_RANGE);

    // Tracks Stack carrying a source_range.
    {
        RetTimeline bad = build_timeline("bad_stack", std::nullopt);
        RetTrack    t   = new Track("video");
        CHECK(t->append_child(clip("A", 0, 60, 24)));
        append_track(bad, t);
        bad->tracks()->set_source_range(T(R(5, 24), R(10, 24)));
        const std::string bad_before = json_of(bad);
        OTIOErrorStatus status;
        RetTimeline out(algo::timeline_ripple_delete(
            bad,
            T(R(5, 24), R(5, 24)),
            &status));
        CHECK(out == nullptr);
        CHECK(is_error(status));
        CHECK(json_of(bad) == bad_before);
    }

    // A non-Track child directly under the tracks Stack.
    {
        RetTimeline bad = build_timeline("bad_child", std::nullopt);
        CHECK(bad->tracks()->append_child(clip("A", 0, 60, 24)));
        const std::string bad_before = json_of(bad);
        OTIOErrorStatus status;
        RetTimeline out(algo::timeline_ripple_delete(
            bad,
            T(R(5, 24), R(5, 24)),
            &status));
        CHECK(out == nullptr);
        CHECK(is_error(status));
        CHECK(status.outcome == OTIOErrorStatus::TYPE_MISMATCH);
        CHECK(json_of(bad) == bad_before);
    }

    // A TimeEffect anywhere is unsupported.
    {
        RetTimeline bad = build_timeline("time_effect", std::nullopt);
        RetTrack    t   = new Track("video");
        auto*       a   = clip("A", 0, 60, 24);
        a->effects().push_back(new LinearTimeWarp("warp", "speed", 2.0));
        CHECK(t->append_child(a));
        append_track(bad, t);
        const std::string bad_before = json_of(bad);
        OTIOErrorStatus status;
        RetTimeline out(algo::timeline_ripple_delete(
            bad,
            T(R(5, 24), R(5, 24)),
            &status));
        CHECK(out == nullptr);
        CHECK(is_error(status));
        CHECK(json_of(bad) == bad_before);
    }

    // A TimeEffect nested inside a nested composition is also unsupported.
    {
        RetTimeline bad = build_timeline("nested_effect", std::nullopt);
        RetTrack    t   = new Track("video");
        RetStack    s   = new Stack("nested");
        auto*       inner = clip("inner", 0, 20, 24);
        inner->effects().push_back(new LinearTimeWarp("warp", "speed", 2.0));
        CHECK(s->append_child(inner));
        CHECK(t->append_child(s));
        append_track(bad, t);
        const std::string bad_before = json_of(bad);
        OTIOErrorStatus status;
        RetTimeline out(algo::timeline_ripple_delete(
            bad,
            T(R(5, 24), R(5, 24)),
            &status));
        CHECK(out == nullptr);
        CHECK(is_error(status));
        CHECK(json_of(bad) == bad_before);
    }

    // An empty cut beyond T still fails.
    expect_failure(T(R(70, 24), R(0, 24)), OTIOErrorStatus::INVALID_TIME_RANGE);
}

//! Ownership: the result is independently retained and freed normally.
void
test_ownership()
{
    RetTimeline input = build_timeline("own", std::nullopt);
    RetTrack track = new Track("video");
    CHECK(track->append_child(clip("A", 0, 40, 24)));
    append_track(input, track);

    OTIOErrorStatus status;
    Timeline* raw = algo::timeline_ripple_delete(
        input,
        T(R(10, 24), R(10, 24)),
        &status);
    CHECK(raw != nullptr);
    CHECK(!is_error(status));
    CHECK(raw->current_ref_count() == 0);
    {
        RetTimeline output(raw);
        CHECK(output->tracks()->children().size() == 1);
        CHECK_NEAR(sec(output->duration()), 30.0 / 24.0, eps);
    }
    CHECK_NEAR(sec(input->duration()), 40.0 / 24.0, eps);
}

} // namespace

int
main()
{
    test_example_multitrack();
    test_fractional_mixed_rates();
    test_nested_object_crosses_cut();
    test_short_and_empty_tracks();
    test_delete_entire_program();
    test_markers_intervals_and_points();
    test_marker_boundaries();
    test_transitions();
    test_deep_copy_independence();
    test_json_roundtrip();
    test_empty_cut_copy();
    test_invalid_inputs();
    test_ownership();

    std::cout << "OTIO_RIPPLE_DELETE_OK" << std::endl;
    return 0;
}
