// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project
//
// Regression tests for algo::timeline_ripple_delete().
//
// Build (see support/check.sh):
//   bash support/check.sh
//
// Exits non-zero on the first failed check.

#include <opentimelineio/algo/timelineEditAlgorithm.h>
#include <opentimelineio/clip.h>
#include <opentimelineio/effect.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/linearTimeWarp.h>
#include <opentimelineio/marker.h>
#include <opentimelineio/missingReference.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/timeline.h>
#include <opentimelineio/track.h>
#include <opentimelineio/transition.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace otio = OTIO_NS;
namespace algo = OTIO_NS::algo;
namespace otime = opentime;
using otime::RationalTime;
using otime::TimeRange;
using otio::AnyDictionary;
using otio::Clip;
using otio::Composable;
using otio::Effect;
using otio::Gap;
using otio::Marker;
using otio::SerializableObject;
using otio::Stack;
using otio::Timeline;
using otio::Track;
using otio::Transition;

// ---------------------------------------------------------------------------
// Tiny test harness
// ---------------------------------------------------------------------------

static int g_checks = 0;

#define CHECK(cond)                                                          \
    do                                                                       \
    {                                                                        \
        ++g_checks;                                                          \
        if (!(cond))                                                         \
        {                                                                    \
            std::cerr << "CHECK FAILED " << __FILE__ << ":" << __LINE__      \
                      << ": " #cond << "\n";                                 \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

#define CHECK_MSG(cond, msg)                                                 \
    do                                                                       \
    {                                                                        \
        ++g_checks;                                                          \
        if (!(cond))                                                         \
        {                                                                    \
            std::cerr << "CHECK FAILED " << __FILE__ << ":" << __LINE__      \
                      << ": " #cond << " (" << msg << ")\n";                 \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

static void
check_time(RationalTime actual, RationalTime expected, char const* what)
{
    ++g_checks;
    if (!actual.almost_equal(expected, 1e-9))
    {
        std::cerr << "TIME MISMATCH " << what << ": got " << actual.value()
                  << "/" << actual.rate() << ", expected " << expected.value()
                  << "/" << expected.rate() << "\n";
        std::exit(1);
    }
}

static void
check_range(TimeRange actual, TimeRange expected, char const* what)
{
    check_time(actual.start_time(), expected.start_time(), what);
    check_time(actual.duration(), expected.duration(), what);
}

static RationalTime
R(double v, double rate = 24.0)
{
    return RationalTime(v, rate);
}

static TimeRange
TR(double start, double duration, double rate = 24.0)
{
    return TimeRange(R(start, rate), R(duration, rate));
}

static Clip*
clip(std::string const& name, double src_start, double dur, double rate)
{
    return new Clip(
        name,
        nullptr,
        TR(src_start, dur, rate));
}

static Gap*
gap(double dur, double rate)
{
    return new Gap(TR(0, dur, rate));
}

static void
add_marker(
    otio::Item*     item,
    std::string const& name,
    TimeRange const& range,
    otio::Color const& color = otio::Color::green)
{
    item->markers().push_back(new Marker(name, range, color));
}

static std::string
json_of(SerializableObject const* object)
{
    otio::ErrorStatus status;
    std::string       result = object->to_json_string(&status);
    CHECK(!otio::is_error(status));
    return result;
}

// ---------------------------------------------------------------------------
// 1. The worked example from the specification
// ---------------------------------------------------------------------------

static SerializableObject::Retainer<Timeline>
build_example()
{
    SerializableObject::Retainer<Timeline> timeline(
        new Timeline("example", R(1000, 24)));

    Stack* stack = new Stack("tracks");
    timeline->set_tracks(stack);

    Track* video = new Track("video");
    video->append_child(clip("A", 100, 20, 24));
    video->append_child(gap(10, 24));
    video->append_child(clip("B", 200, 30, 24));
    stack->append_child(video);

    // One long clip, exposed through the track's own [80, 120) trim.
    Track* audio = new Track("audio", TR(80, 40, 24));
    audio->append_child(clip("C", 500, 200, 24));
    stack->append_child(audio);

    return timeline;
}

static void
test_worked_example()
{
    auto input = build_example();
    CHECK(input->duration() == R(60, 24));

    std::string const before = json_of(input);

    otio::ErrorStatus status;
    SerializableObject::Retainer<Timeline> output(
        algo::timeline_ripple_delete(input, TR(10, 10, 24), &status));
    CHECK(output.value != nullptr);
    CHECK(!otio::is_error(status));

    // Input must not be touched.
    CHECK(json_of(input) == before);
    CHECK(input->duration() == R(60, 24));

    // Global start time and other timeline info survive.
    CHECK(output->name() == "example");
    CHECK(output->global_start_time().has_value());
    check_time(
        *output->global_start_time(),
        R(1000, 24),
        "global_start_time preserved");

    Stack* out_stack = output->tracks();
    CHECK(out_stack != nullptr);
    CHECK(out_stack->children().size() == 2);

    Track* out_video = dynamic_cast<Track*>(out_stack->children()[0].value);
    Track* out_audio = dynamic_cast<Track*>(out_stack->children()[1].value);
    CHECK(out_video && out_audio);
    CHECK(out_video->name() == "video");
    CHECK(out_audio->name() == "audio");

    // Output tracks always start at zero with no source_range.
    CHECK(!out_video->source_range());
    CHECK(!out_audio->source_range());
    check_range(
        out_video->trimmed_range(),
        TR(0, 50, 24),
        "video duration");
    check_range(
        out_audio->trimmed_range(),
        TR(0, 50, 24),
        "audio duration");

    // Video: source[100,110), 10 frame gap, source[200,230).
    CHECK(out_video->children().size() == 3);
    Clip* va = dynamic_cast<Clip*>(out_video->children()[0].value);
    Gap*  vg = dynamic_cast<Gap*>(out_video->children()[1].value);
    Clip* vb = dynamic_cast<Clip*>(out_video->children()[2].value);
    CHECK(va && vg && vb);
    check_range(va->trimmed_range(), TR(100, 10, 24), "video A");
    check_range(vg->trimmed_range(), TR(0, 10, 24), "video gap");
    check_range(vb->trimmed_range(), TR(200, 30, 24), "video B");
    CHECK(va->name() == "A");
    CHECK(vb->name() == "B");

    // Audio: source[580,590), source[600,620), 20 frame gap.
    CHECK(out_audio->children().size() == 3);
    Clip* aa = dynamic_cast<Clip*>(out_audio->children()[0].value);
    Clip* ab = dynamic_cast<Clip*>(out_audio->children()[1].value);
    Gap*  ag = dynamic_cast<Gap*>(out_audio->children()[2].value);
    CHECK(aa && ab && ag);
    check_range(aa->trimmed_range(), TR(580, 10, 24), "audio left");
    check_range(ab->trimmed_range(), TR(600, 20, 24), "audio right");
    check_range(ag->trimmed_range(), TR(0, 20, 24), "audio gap");
    CHECK(aa->name() == "C");
    CHECK(ab->name() == "C");
    // The two fragments are independent objects.
    CHECK(aa != ab);

    // JSON round-trip preserves the edit.
    otio::ErrorStatus read_status;
    SerializableObject::Retainer<SerializableObject> reread(
        SerializableObject::from_json_string(
            json_of(output),
            &read_status));
    CHECK(reread.value != nullptr);
    CHECK(!otio::is_error(read_status));
    CHECK(reread->is_equivalent_to(*output));
}

// ---------------------------------------------------------------------------
// 2. Mixed rates and a fractional cut
// ---------------------------------------------------------------------------

static void
test_mixed_rates_fractional_cut()
{
    SerializableObject::Retainer<Timeline> timeline;
    Stack*                                 stack = new Stack("tracks");
    timeline = new Timeline("rates");
    timeline->set_tracks(stack);

    // Three tracks of exactly 2.5 seconds, expressed at 24, 25 and 48kHz.
    Track* t24 = new Track("t24");
    t24->append_child(clip("v24", 100, 60, 24));
    stack->append_child(t24);

    Track* t25 = new Track("t25");
    t25->append_child(clip("v25", 500, 62.5, 25));
    stack->append_child(t25);

    Track* t48k = new Track("t48k");
    t48k->append_child(clip("a48k", 24000, 120000, 48000));
    stack->append_child(t48k);

    // Cut [10, 20) at 24fps == [0.41666.., 0.8333..) seconds. Never an
    // integer number of frames at 25fps.
    TimeRange const cut(R(10, 24), R(10, 24));
    RationalTime const output_duration = R(50, 24);

    otio::ErrorStatus status;
    SerializableObject::Retainer<Timeline> output(
        algo::timeline_ripple_delete(timeline, cut, &status));
    CHECK(output.value != nullptr);
    CHECK(!otio::is_error(status));

    Stack* out_stack = output->tracks();
    CHECK(out_stack->children().size() == 3);

    Track* o24 = dynamic_cast<Track*>(out_stack->children()[0].value);
    Track* o25 = dynamic_cast<Track*>(out_stack->children()[1].value);
    Track* o48 = dynamic_cast<Track*>(out_stack->children()[2].value);
    CHECK(o24 && o25 && o48);
    check_time(o24->duration(), output_duration, "t24 duration");
    check_time(o25->duration(), output_duration, "t25 duration");
    check_time(o48->duration(), output_duration, "t48k duration");

    // 24fps: exact frame split.
    CHECK(o24->children().size() == 2);
    Clip* a24 = dynamic_cast<Clip*>(o24->children()[0].value);
    Clip* b24 = dynamic_cast<Clip*>(o24->children()[1].value);
    CHECK(a24 && b24);
    check_range(a24->trimmed_range(), TR(100, 10, 24), "24 left");
    check_range(b24->trimmed_range(), TR(120, 40, 24), "24 right");
    check_time(a24->duration(), R(10, 24), "24 left dur");
    check_time(b24->duration(), R(40, 24), "24 right dur");

    // 25fps: fractional source values, still expressed as fractions.
    CHECK(o25->children().size() == 2);
    Clip* a25 = dynamic_cast<Clip*>(o25->children()[0].value);
    Clip* b25 = dynamic_cast<Clip*>(o25->children()[1].value);
    CHECK(a25 && b25);
    check_time(a25->trimmed_range().start_time(), R(500, 25), "25 left start");
    check_time(a25->trimmed_range().duration(), R(250.0 / 24.0, 25), "25 left dur");
    check_time(b25->trimmed_range().start_time(), R(500 + 500.0 / 24.0, 25), "25 right start");
    check_time(b25->trimmed_range().duration(), R(1000.0 / 24.0, 25), "25 right dur");
    // Durations are not rounded to whole 25fps frames.
    CHECK(std::fabs(a25->duration().value() - std::round(a25->duration().value()))
          > 1e-9);

    // 48kHz: exact integer sample split.
    CHECK(o48->children().size() == 2);
    Clip* a48 = dynamic_cast<Clip*>(o48->children()[0].value);
    Clip* b48 = dynamic_cast<Clip*>(o48->children()[1].value);
    CHECK(a48 && b48);
    check_range(a48->trimmed_range(), TR(24000, 20000, 48000), "48k left");
    check_range(b48->trimmed_range(), TR(64000, 80000, 48000), "48k right");
}

// ---------------------------------------------------------------------------
// 3. Nested objects crossing the cut
// ---------------------------------------------------------------------------

static void
test_nested_object_split()
{
    SerializableObject::Retainer<Timeline> timeline = new Timeline("nested");
    Stack*                                 stack    = new Stack("tracks");
    timeline->set_tracks(stack);

    Track* track = new Track("outer");
    stack->append_child(track);

    // A nested Stack holding two clips; the whole stack is exposed from
    // source zero, and straddles the cut [10, 20).
    Stack* nested = new Stack("nestedStack", TR(0, 40, 24));
    nested->append_child(clip("n1", 1000, 15, 24));
    nested->append_child(clip("n2", 2000, 25, 24));
    nested->markers().push_back(new Marker("inner", TR(3, 0, 24)));
    nested->effects().push_back(new Effect("blur", "Blur"));
    track->append_child(nested);

    // Also append a clip so the track reaches program length 60.
    track->append_child(clip("tail", 300, 20, 24));

    CHECK(timeline->duration() == R(60, 24));

    otio::ErrorStatus status;
    SerializableObject::Retainer<Timeline> output(
        algo::timeline_ripple_delete(timeline, TR(10, 10, 24), &status));
    CHECK(output.value != nullptr);
    CHECK(!otio::is_error(status));

    Track* out_track =
        dynamic_cast<Track*>(output->tracks()->children()[0].value);
    CHECK(out_track && !out_track->source_range());
    check_time(out_track->duration(), R(50, 24), "nested track duration");

    // left nested fragment, right nested fragment, tail clip
    CHECK(out_track->children().size() == 3);
    Stack* left  = dynamic_cast<Stack*>(out_track->children()[0].value);
    Stack* right = dynamic_cast<Stack*>(out_track->children()[1].value);
    Clip*  tail  = dynamic_cast<Clip*>(out_track->children()[2].value);
    CHECK(left && right && tail);

    // Nested structure is preserved on both sides, not flattened.
    CHECK(left != right);
    CHECK(left->children().size() == 2);
    CHECK(right->children().size() == 2);
    CHECK(dynamic_cast<Clip*>(left->children()[0].value));
    CHECK(dynamic_cast<Clip*>(left->children()[1].value));

    // Only the visible trim (as an Item) of each fragment changed.
    check_range(left->trimmed_range(), TR(0, 10, 24), "nested left trim");
    check_range(right->trimmed_range(), TR(20, 20, 24), "nested right trim");

    // The nested object's own markers stay in source coordinates and both
    // fragments carry them independently.
    CHECK(left->markers().size() == 1);
    CHECK(right->markers().size() == 1);
    CHECK(left->markers()[0].value != right->markers()[0].value);
    check_range(
        left->markers()[0]->marked_range(),
        TR(3, 0, 24),
        "nested marker source coords");

    // Effects survive on both fragments and are independent objects.
    CHECK(left->effects().size() == 1);
    CHECK(right->effects().size() == 1);
    CHECK(left->effects()[0].value != right->effects()[0].value);
    CHECK(left->effects()[0]->effect_name() == "Blur");

    // Internal clip trims are untouched by program-time arithmetic.
    Clip* lc = dynamic_cast<Clip*>(left->children()[0].value);
    check_range(lc->trimmed_range(), TR(1000, 15, 24), "nested clip unchanged");

    // Tail clip is pulled left: it sat at [40,60), now [30,50) -> src [300,320).
    check_range(tail->trimmed_range(), TR(300, 20, 24), "tail unchanged src");
    check_range(tail->range_in_parent(), TR(30, 20, 24), "tail shifted");
}

// ---------------------------------------------------------------------------
// 4. Short and empty tracks
// ---------------------------------------------------------------------------

static void
test_short_and_empty_tracks()
{
    SerializableObject::Retainer<Timeline> timeline = new Timeline("short");
    Stack*                                 stack    = new Stack("tracks");
    timeline->set_tracks(stack);

    Track* full = new Track("full");
    full->append_child(clip("F", 500, 60, 24));
    stack->append_child(full);

    // Shorter track: 20 frames of program out of 60.
    Track* short_track = new Track("short");
    short_track->append_child(clip("S", 900, 20, 24));
    stack->append_child(short_track);

    // Completely empty track.
    Track* empty = new Track("empty");
    stack->append_child(empty);

    otio::ErrorStatus status;
    SerializableObject::Retainer<Timeline> output(
        algo::timeline_ripple_delete(timeline, TR(10, 10, 24), &status));
    CHECK(output.value != nullptr);
    CHECK(!otio::is_error(status));

    auto const& kids = output->tracks()->children();
    CHECK(kids.size() == 3);

    Track* ofull  = dynamic_cast<Track*>(kids[0].value);
    Track* oshort = dynamic_cast<Track*>(kids[1].value);
    Track* oempty = dynamic_cast<Track*>(kids[2].value);
    CHECK(ofull && oshort && oempty);

    check_time(ofull->duration(), R(50, 24), "full duration");
    check_time(oshort->duration(), R(50, 24), "short padded to 50");
    check_time(oempty->duration(), R(50, 24), "empty padded to 50");

    // The short track had no content after frame 10, so the second half is
    // blank: its clip is trimmed to [900,910) and a 30 frame gap follows.
    CHECK(oshort->children().size() == 2);
    Clip* sc = dynamic_cast<Clip*>(oshort->children()[0].value);
    Gap*  sg = dynamic_cast<Gap*>(oshort->children()[1].value);
    CHECK(sc && sg);
    check_range(sc->trimmed_range(), TR(900, 10, 24), "short clip trimmed");
    check_time(sg->duration(), R(40, 24), "short trailing gap");

    // The empty track becomes one full-length gap (not a zero-length gap).
    CHECK(oempty->children().size() == 1);
    Gap* eg = dynamic_cast<Gap*>(oempty->children()[0].value);
    CHECK(eg != nullptr);
    check_time(eg->duration(), R(50, 24), "empty track gap");
}

static void
test_delete_entire_timeline_keeps_empty_tracks()
{
    SerializableObject::Retainer<Timeline> timeline = new Timeline("whole");
    Stack*                                 stack    = new Stack("tracks");
    timeline->set_tracks(stack);

    Track* a = new Track("a");
    a->append_child(clip("A", 0, 30, 24));
    stack->append_child(a);

    Track* b = new Track("b");
    b->append_child(clip("B", 0, 20, 24));
    stack->append_child(b);
    add_marker(b, "keep", TR(1, 2, 24));

    otio::ErrorStatus status;
    SerializableObject::Retainer<Timeline> output(
        algo::timeline_ripple_delete(timeline, TR(0, 30, 24), &status));
    CHECK(output.value != nullptr);
    CHECK(!otio::is_error(status));

    auto const& kids = output->tracks()->children();
    CHECK(kids.size() == 2);
    for (auto const& kid: kids)
    {
        Track* track = dynamic_cast<Track*>(kid.value);
        CHECK(track != nullptr);
        CHECK(track->children().empty());
        check_time(track->duration(), R(0, 24), "empty track duration");
        CHECK(!track->source_range());
    }

    // Track names and the stack itself are kept.
    CHECK(dynamic_cast<Track*>(kids[0].value)->name() == "a");
    CHECK(output->tracks()->name() == "tracks");
}

// ---------------------------------------------------------------------------
// 5. Markers
// ---------------------------------------------------------------------------

static void
test_markers()
{
    SerializableObject::Retainer<Timeline> timeline = new Timeline("markers");
    Stack*                                 stack    = new Stack("tracks");
    timeline->set_tracks(stack);

    Track* track = new Track("video");
    track->append_child(clip("A", 0, 60, 24));
    stack->append_child(track);

    // Stack markers are in program time.
    //   - an interval crossing the cut -> two pieces
    //   - an interval entirely after the cut -> shifted
    //   - a point at a (deleted), a point at b (kept at a), a point at the
    //     visible end (dropped)
    auto* cross = new Marker("cross", TR(5, 25, 24), otio::Color::red);
    cross->metadata()["kind"] = std::string("cross");
    stack->markers().push_back(cross);
    add_marker(stack, "after", TR(25, 10, 24));
    add_marker(stack, "point_a", TR(10, 0, 24));
    add_marker(stack, "point_b", TR(20, 0, 24));
    add_marker(stack, "point_end", TR(60, 0, 24));
    add_marker(stack, "point_inside_cut", TR(15, 0, 24));

    // Top-level track markers are in the track's pre-trim local time, which
    // for a track without source_range equals program time.
    add_marker(track, "track_cross", TR(5, 25, 24));

    otio::ErrorStatus status;
    SerializableObject::Retainer<Timeline> output(
        algo::timeline_ripple_delete(timeline, TR(10, 10, 24), &status));
    CHECK(output.value != nullptr);
    CHECK(!otio::is_error(status));

    Stack* out_stack = output->tracks();
    auto const& markers = out_stack->markers();

    // Order: cross-left, cross-right, after, point_b.
    CHECK(markers.size() == 4);
    CHECK(markers[0]->name() == "cross");
    CHECK(markers[1]->name() == "cross");
    CHECK(markers[2]->name() == "after");
    CHECK(markers[3]->name() == "point_b");

    // Interval [5,30) minus [10,20) -> [5,10) and [20,30)-10 = [10,20).
    check_range(markers[0]->marked_range(), TR(5, 5, 24), "marker left");
    check_range(markers[1]->marked_range(), TR(10, 10, 24), "marker right");
    // The two pieces touch but are not merged.
    CHECK(markers[0].value != markers[1].value);
    // Colour, name and metadata are preserved on both halves.
    CHECK(markers[0]->color() == otio::Color::red);
    CHECK(markers[1]->color() == otio::Color::red);
    CHECK(markers[0]->metadata().at("kind").type() == typeid(std::string));
    CHECK(markers[1]->metadata().at("kind").type() == typeid(std::string));

    // [25,35) shifted by 10 -> [15,25).
    check_range(markers[2]->marked_range(), TR(15, 10, 24), "marker after");
    // Point at b moves to a.
    check_range(markers[3]->marked_range(), TR(10, 0, 24), "point b -> a");

    // Track markers are adjusted the same way in pre-trim local time.
    Track* out_track =
        dynamic_cast<Track*>(output->tracks()->children()[0].value);
    CHECK(out_track->markers().size() == 2);
    check_range(
        out_track->markers()[0]->marked_range(),
        TR(5, 5, 24),
        "track marker left");
    check_range(
        out_track->markers()[1]->marked_range(),
        TR(10, 10, 24),
        "track marker right");

    // Clip markers keep their source coordinates.
    Clip* out_clip = dynamic_cast<Clip*>(out_track->children()[0].value);
    CHECK(out_clip != nullptr);
    auto* clip_marker = new Marker("clip_marker", TR(3, 0, 24));
    // (Built on the output solely to check independence below.)
    out_clip->markers().push_back(clip_marker);
    check_range(
        out_clip->markers()[0]->marked_range(),
        TR(3, 0, 24),
        "clip marker local");

    // The input timeline markers are untouched.
    CHECK(timeline->tracks()->markers().size() == 6);
    CHECK(timeline->tracks()->markers()[0]->marked_range() == TR(5, 25, 24));
    CHECK(track->markers().size() == 1);
}

// ---------------------------------------------------------------------------
// 6. Transitions
// ---------------------------------------------------------------------------

static SerializableObject::Retainer<Timeline>
build_transition_timeline(
    double transition_in,
    double transition_out,
    bool   with_track_trim,
    TimeRange const& track_trim = TimeRange())
{
    SerializableObject::Retainer<Timeline> timeline = new Timeline("trans");
    Stack*                                 stack    = new Stack("tracks");
    timeline->set_tracks(stack);

    Track* track = new Track(
        "video",
        with_track_trim ? std::optional<TimeRange>(track_trim) : std::nullopt);
    track->append_child(clip("A", 100, 20, 24));
    track->append_child(new Transition(
        "dissolve",
        Transition::Type::SMPTE_Dissolve,
        R(transition_in, 24),
        R(transition_out, 24)));
    track->append_child(clip("B", 200, 20, 24));
    stack->append_child(track);
    return timeline;
}

static void
test_transition_touching_cut_endpoint()
{
    // A is [0,20), the transition covers [19,20) (in=1, out=0), B is [20,40).
    // A cut starting at 20 only touches the transition at its end.
    auto timeline = build_transition_timeline(1, 0, false);

    otio::ErrorStatus status;
    SerializableObject::Retainer<Timeline> output(
        algo::timeline_ripple_delete(timeline, TR(20, 10, 24), &status));
    CHECK(output.value != nullptr);
    CHECK(!otio::is_error(status));

    Track* track =
        dynamic_cast<Track*>(output->tracks()->children()[0].value);
    CHECK(track != nullptr);
    CHECK(track->children().size() == 3);

    Clip*       a = dynamic_cast<Clip*>(track->children()[0].value);
    Transition* t = dynamic_cast<Transition*>(track->children()[1].value);
    Clip*       b = dynamic_cast<Clip*>(track->children()[2].value);
    CHECK(a && t && b);
    check_time(t->in_offset(), R(1, 24), "transition in offset unchanged");
    check_time(t->out_offset(), R(0, 24), "transition out offset unchanged");
    CHECK(t->name() == "dissolve");
    // B occupied [20,40); removing [20,30) leaves [30,40) -> src [210,220).
    check_range(b->trimmed_range(), TR(210, 10, 24), "B after cut");
    check_range(a->trimmed_range(), TR(100, 20, 24), "A untouched");
    check_range(a->range_in_parent(), TR(0, 20, 24), "A still at zero");
    check_time(track->duration(), R(30, 24), "transition track duration");
}

static void
test_transition_touching_cut_start_endpoint()
{
    // Transition spans [19,21) (in=1, out=1); a cut ending at 19 touches
    // its start without overlapping.
    auto timeline = build_transition_timeline(1, 1, false);

    otio::ErrorStatus status;
    SerializableObject::Retainer<Timeline> output(
        algo::timeline_ripple_delete(timeline, TR(5, 14, 24), &status));
    CHECK(output.value != nullptr);
    CHECK(!otio::is_error(status));

    Track* track =
        dynamic_cast<Track*>(output->tracks()->children()[0].value);
    CHECK(track != nullptr);
    // A spans the cut, so it splits; the transition survives in the right
    // half because the cut only touches its start.
    CHECK(track->children().size() == 4);
    Clip*       a_right = dynamic_cast<Clip*>(track->children()[1].value);
    Transition* t       = dynamic_cast<Transition*>(track->children()[2].value);
    Clip*       b       = dynamic_cast<Clip*>(track->children()[3].value);
    CHECK(a_right && t && b);
    check_range(a_right->trimmed_range(), TR(119, 1, 24), "A right sliver");
    check_time(t->in_offset(), R(1, 24), "transition in preserved");
    check_time(t->out_offset(), R(1, 24), "transition out preserved");
    check_time(track->duration(), R(26, 24), "transition track duration");
}

static void
test_transition_intersecting_cut_fails()
{
    // Transition spans [19,21); cut [20,30) has a positive overlap.
    auto timeline = build_transition_timeline(1, 1, false);
    std::string const before = json_of(timeline);

    otio::ErrorStatus status;
    SerializableObject::Retainer<Timeline> output(
        algo::timeline_ripple_delete(timeline, TR(20, 10, 24), &status));
    CHECK(output.value == nullptr);
    CHECK(otio::is_error(status));
    CHECK_MSG(!status.details.empty(), "failure should describe the cause");
    // Input unchanged after the failure.
    CHECK(json_of(timeline) == before);
}

static void
test_transition_clipped_by_track_trim_fails()
{
    // Track exposes only [1, 40), so the transition [19,21) is bisected by
    // the visible window's start? No: use [0, 20) track trim ending inside
    // the transition.
    auto timeline = build_transition_timeline(1, 1, true, TR(0, 20, 24));
    std::string const before = json_of(timeline);

    otio::ErrorStatus status;
    SerializableObject::Retainer<Timeline> output(
        algo::timeline_ripple_delete(timeline, TR(5, 5, 24), &status));
    CHECK(output.value == nullptr);
    CHECK(otio::is_error(status));
    CHECK_MSG(!status.details.empty(), "failure should describe the cause");
    CHECK(json_of(timeline) == before);
}

static void
test_transition_fully_invisible_dropped()
{
    auto timeline = build_transition_timeline(1, 1, false);
    // Track trim [30, 40) hides the transition entirely.
    Track* track = dynamic_cast<Track*>(timeline->tracks()->children()[0].value);
    CHECK(track != nullptr);
    track->set_source_range(TR(30, 10, 24));

    otio::ErrorStatus status;
    SerializableObject::Retainer<Timeline> output(
        algo::timeline_ripple_delete(timeline, TR(2, 3, 24), &status));
    CHECK(output.value != nullptr);
    CHECK(!otio::is_error(status));

    Track* out_track =
        dynamic_cast<Track*>(output->tracks()->children()[0].value);
    CHECK(out_track != nullptr);
    for (auto const& child: out_track->children())
    {
        CHECK(dynamic_cast<Transition*>(child.value) == nullptr);
    }
}

// ---------------------------------------------------------------------------
// 6b. Track source_range, markers and partial cuts
// ---------------------------------------------------------------------------

static void
test_track_source_range_and_markers()
{
    // Track [80,120) of a 200 frame clip: program [0,40) == source [80,120).
    SerializableObject::Retainer<Timeline> timeline = new Timeline("trim");
    Stack*                                 stack    = new Stack("tracks");
    timeline->set_tracks(stack);

    Track* track = new Track("audio", TR(80, 40, 24));
    track->append_child(clip("long", 0, 200, 24));
    // Track markers live in pre-trim local time, i.e. the same coordinate
    // system as the track's source_range. Its visible window is [80,120).
    add_marker(track, "tm", TR(85, 20, 24));
    stack->append_child(track);

    otio::ErrorStatus status;
    SerializableObject::Retainer<Timeline> output(
        algo::timeline_ripple_delete(timeline, TR(10, 10, 24), &status));
    CHECK(output.value != nullptr);
    CHECK(!otio::is_error(status));

    Track* out_track =
        dynamic_cast<Track*>(output->tracks()->children()[0].value);
    CHECK(out_track != nullptr);
    CHECK(!out_track->source_range());
    check_time(out_track->duration(), R(30, 24), "trimmed track duration");

    // The clip is trimmed to the program-visible window [80,120) then cut.
    CHECK(out_track->children().size() == 2);
    Clip* a = dynamic_cast<Clip*>(out_track->children()[0].value);
    Clip* b = dynamic_cast<Clip*>(out_track->children()[1].value);
    CHECK(a && b);
    check_range(a->trimmed_range(), TR(80, 10, 24), "trim left");
    check_range(b->trimmed_range(), TR(100, 20, 24), "trim right");

    // Track marker [85,105) is program [5,25); the cut removes its middle.
    CHECK(out_track->markers().size() == 2);
    check_range(
        out_track->markers()[0]->marked_range(),
        TR(5, 5, 24),
        "track marker left");
    check_range(
        out_track->markers()[1]->marked_range(),
        TR(10, 5, 24),
        "track marker right");
}

static void
test_cut_at_program_start()
{
    // A cut that starts at program zero. A track with a source_range still
    // maps program zero to the start of that range, not to its start value.
    SerializableObject::Retainer<Timeline> timeline = new Timeline("start");
    Stack*                                 stack    = new Stack("tracks");
    timeline->set_tracks(stack);

    Track* full = new Track("full");
    full->append_child(clip("F", 0, 60, 24));
    stack->append_child(full);

    Track* trimmed = new Track("trimmed", TR(20, 40, 24));
    trimmed->append_child(clip("L", 1000, 60, 24));
    stack->append_child(trimmed);

    otio::ErrorStatus status;
    SerializableObject::Retainer<Timeline> output(
        algo::timeline_ripple_delete(timeline, TR(0, 10, 24), &status));
    CHECK(output.value != nullptr);
    CHECK(!otio::is_error(status));

    auto const& kids = output->tracks()->children();
    Track*      otrim = dynamic_cast<Track*>(kids[1].value);
    CHECK(otrim != nullptr);
    check_time(otrim->duration(), R(50, 24), "trimmed track duration");
    CHECK(!otrim->source_range());

    // The track's source_range start (20) is program zero. Program [0,40)
    // maps to local [20,60), so the clip shows source [1020,1060). Removing
    // program [0,10) leaves local [30,60) == source [1030,1060) plus blank.
    CHECK(otrim->children().size() == 2);
    Clip* lc = dynamic_cast<Clip*>(otrim->children()[0].value);
    Gap*  g1 = dynamic_cast<Gap*>(otrim->children()[1].value);
    CHECK(lc && g1);
    check_range(lc->trimmed_range(), TR(1030, 30, 24), "trimmed clip");
    check_time(g1->duration(), R(20, 24), "trailing blank");
}

// ---------------------------------------------------------------------------
// 7. Deep-copy independence and metadata / effects
// ---------------------------------------------------------------------------

static void
test_deep_copy_independence()
{
    auto timeline = build_example();

    // Add effects and metadata to the input clips.
    Track* video = dynamic_cast<Track*>(timeline->tracks()->children()[0].value);
    CHECK(video != nullptr);
    Clip* input_a = dynamic_cast<Clip*>(video->children()[0].value);
    CHECK(input_a != nullptr);
    input_a->metadata()["tag"] = std::string("original");
    input_a->effects().push_back(new Effect("grain", "Grain"));
    input_a->markers().push_back(new Marker("m", TR(2, 0, 24)));

    std::string const before = json_of(timeline);

    otio::ErrorStatus status;
    SerializableObject::Retainer<Timeline> output(
        algo::timeline_ripple_delete(timeline, TR(5, 10, 24), &status));
    CHECK(output.value != nullptr);
    CHECK(!otio::is_error(status));

    Track* out_video =
        dynamic_cast<Track*>(output->tracks()->children()[0].value);
    CHECK(out_video != nullptr);
    // A is split at [0,5) / [15,20).
    CHECK(out_video->children().size() == 4); // Aleft, Aright, gap, B
    Clip* left  = dynamic_cast<Clip*>(out_video->children()[0].value);
    Clip* right = dynamic_cast<Clip*>(out_video->children()[1].value);
    CHECK(left && right);
    CHECK(left != right);
    CHECK(std::any_cast<std::string>(left->metadata().at("tag"))
          == std::string("original"));
    CHECK(left->effects().size() == 1);
    CHECK(right->effects().size() == 1);
    CHECK(left->effects()[0].value != right->effects()[0].value);
    CHECK(left->markers()[0].value != right->markers()[0].value);

    // Mutating the output must not affect the input.
    left->metadata()["tag"] = std::string("mutated");
    left->effects()[0]->set_effect_name("Changed");
    left->markers()[0]->set_name("changed");
    left->set_enabled(false);
    right->set_source_range(TR(1, 1, 24));

    CHECK(json_of(timeline) == before);
    CHECK(input_a->metadata().at("tag").type() == typeid(std::string));
    CHECK(std::any_cast<std::string>(input_a->metadata().at("tag"))
          == std::string("original"));
    CHECK(input_a->effects()[0]->effect_name() == "Grain");
    CHECK(input_a->markers()[0]->name() == "m");
    CHECK(input_a->enabled());
}

// ---------------------------------------------------------------------------
// 8. Invalid inputs and validation
// ---------------------------------------------------------------------------

static void
expect_failure(
    Timeline const* input,
    TimeRange const& cut,
    char const*      what)
{
    otio::ErrorStatus status;
    SerializableObject::Retainer<Timeline> output(
        algo::timeline_ripple_delete(input, cut, &status));
    CHECK_MSG(output.value == nullptr, what);
    CHECK_MSG(otio::is_error(status), what);
    CHECK_MSG(!status.details.empty(), what);
}

static void
test_invalid_inputs()
{
    auto timeline = build_example();
    std::string const before = json_of(timeline);

    // Null input.
    expect_failure(nullptr, TR(0, 1, 24), "null input");

    // Negative start.
    expect_failure(timeline, TR(-1, 2, 24), "negative cut start");

    // End past the program length.
    expect_failure(timeline, TR(50, 20, 24), "cut past the end");

    // Zero / negative rate.
    expect_failure(
        timeline,
        TimeRange(R(0, 0), R(5, 24)),
        "zero cut rate");
    expect_failure(
        timeline,
        TimeRange(R(0, 24), R(5, -24)),
        "negative duration rate");

    // Non-finite values.
    expect_failure(
        timeline,
        TimeRange(
            R(std::numeric_limits<double>::infinity(), 24),
            R(5, 24)),
        "infinite cut start");
    expect_failure(
        timeline,
        TimeRange(R(0, 24), R(std::numeric_limits<double>::quiet_NaN(), 24)),
        "NaN cut duration");

    // The input survives every failure.
    CHECK(json_of(timeline) == before);
}

static void
test_empty_cut_returns_independent_copy()
{
    auto timeline = build_example();
    std::string const before = json_of(timeline);

    otio::ErrorStatus status;
    SerializableObject::Retainer<Timeline> output(
        algo::timeline_ripple_delete(timeline, TR(10, 0, 24), &status));
    CHECK(output.value != nullptr);
    CHECK(!otio::is_error(status));
    CHECK(output.value != timeline.value);
    CHECK(output->is_equivalent_to(*timeline));
    CHECK(json_of(timeline) == before);

    // Deep copy: mutating the copy leaves the original alone.
    Track* out_track =
        dynamic_cast<Track*>(output->tracks()->children()[0].value);
    CHECK(out_track != nullptr);
    out_track->children()[0]->metadata()["x"] = std::string("y");
    CHECK(json_of(timeline) == before);

    // An empty cut is still structurally validated.
    SerializableObject::Retainer<Timeline> bad = new Timeline("bad");
    Stack*                                 stack = new Stack("tracks");
    bad->set_tracks(stack);
    // A non-Track child is not allowed.
    stack->append_child(new Clip("not_a_track", nullptr, TR(0, 10, 24)));
    expect_failure(bad, TR(1, 0, 24), "empty cut still validates structure");
}

static void
test_time_effects_rejected()
{
    auto timeline = build_example();
    Stack* stack   = timeline->tracks();
    Track* track   = dynamic_cast<Track*>(stack->children()[0].value);
    CHECK(track != nullptr);
    Clip* c = dynamic_cast<Clip*>(track->children()[0].value);
    CHECK(c != nullptr);
    c->effects().push_back(new otio::LinearTimeWarp("warp", "Warp", 2.0));

    std::string const before = json_of(timeline);
    expect_failure(timeline, TR(50, 5, 24), "time effect far from the cut");
    CHECK(json_of(timeline) == before);

    // A nested time effect is rejected too.
    SerializableObject::Retainer<Timeline> nested = new Timeline("nested");
    Stack*                                 nstack = new Stack("tracks");
    nested->set_tracks(nstack);
    Track* outer = new Track("outer");
    nstack->append_child(outer);
    Stack* inner = new Stack("inner", TR(0, 10, 24));
    inner->effects().push_back(new otio::LinearTimeWarp("freeze", "Warp", 0.0));
    inner->append_child(clip("c", 0, 10, 24));
    outer->append_child(inner);
    expect_failure(nested, TR(0, 1, 24), "nested time effect");
}

static void
test_tracks_stack_source_range_rejected()
{
    SerializableObject::Retainer<Timeline> timeline = new Timeline("bad");
    Stack*                                 stack =
        new Stack("tracks", TR(0, 10, 24));
    timeline->set_tracks(stack);
    Track* track = new Track("t");
    track->append_child(clip("c", 0, 10, 24));
    stack->append_child(track);

    expect_failure(timeline, TR(1, 1, 24), "stack source_range");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int
main()
{
    test_worked_example();
    test_mixed_rates_fractional_cut();
    test_nested_object_split();
    test_short_and_empty_tracks();
    test_delete_entire_timeline_keeps_empty_tracks();
    test_markers();
    test_transition_touching_cut_endpoint();
    test_transition_touching_cut_start_endpoint();
    test_transition_intersecting_cut_fails();
    test_transition_clipped_by_track_trim_fails();
    test_transition_fully_invisible_dropped();
    test_track_source_range_and_markers();
    test_cut_at_program_start();
    test_deep_copy_independence();
    test_invalid_inputs();
    test_empty_cut_returns_independent_copy();
    test_time_effects_rejected();
    test_tracks_stack_source_range_rejected();

    std::cout << "RIPPLE_DELETE_TESTS_OK checks=" << g_checks << "\n";
    return 0;
}
