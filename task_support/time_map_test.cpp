#include "task_support/check.h"

#include <opentimelineio/clip.h>
#include <opentimelineio/errorStatus.h>
#include <opentimelineio/gap.h>
#include <opentimelineio/linearTimeWarp.h>
#include <opentimelineio/serializableObject.h>
#include <opentimelineio/timeMappedStack.h>
#include <opentimelineio/track.h>
#include <opentimelineio/trackAlgorithm.h>
#include <opentimelineio/transition.h>

#include <string>
#include <vector>

namespace o = opentimelineio::OPENTIMELINEIO_VERSION_NS;

using o::Clip;
using o::ErrorStatus;
using o::RationalTime;
using o::TimeMappedStack;
using o::TimeRange;
using o::Track;

namespace
{
RationalTime
fr(int value, double rate = 24.0)
{
    return RationalTime(value, rate);
}

TimeRange
rr(int start, int duration, double rate = 24.0)
{
    return TimeRange(fr(start, rate), fr(duration, rate));
}

struct MainProject
{
    o::SerializableObject::Retainer<TimeMappedStack> mapped;
    Clip* A = nullptr;
    Clip* B = nullptr;
    Clip* C = nullptr;
};

MainProject
make_main_project(ErrorStatus* error)
{
    MainProject p;
    p.mapped = new TimeMappedStack("main");

    auto* video = new Track("video");
    p.A         = new Clip("A", nullptr, rr(100, 12));
    p.B         = new Clip("B", nullptr, rr(200, 12));
    video->append_child(p.A, error);
    video->append_child(p.B, error);

    auto* audio = new Track("audio");
    p.C         = new Clip("C", nullptr, rr(500, 24));
    audio->append_child(p.C, error);

    p.mapped->append_child(video, error);
    p.mapped->append_child(audio, error);

    p.mapped->set_time_map(
        {
            { fr(0), fr(0) },
            { fr(6), fr(12) },
            { fr(10), fr(12) },
            { fr(16), fr(0) },
        },
        error);
    return p;
}

bool
sample_matches(
    std::vector<TimeMappedStack::Sample> const& samples,
    size_t                                     index,
    Clip const*                                clip,
    int                                        source_frame)
{
    return index < samples.size() && samples[index].clip == clip
           && samples[index].source_time == fr(source_frame);
}

bool
clip_names_are(
    std::vector<o::SerializableObject::Retainer<Clip>> const& clips,
    std::vector<std::string> const&                           names)
{
    if (clips.size() != names.size())
    {
        return false;
    }
    for (size_t i = 0; i < names.size(); ++i)
    {
        if (clips[i]->name() != names[i])
        {
            return false;
        }
    }
    return true;
}

int
relation_samples_and_find()
{
    ErrorStatus e;
    MainProject p = make_main_project(&e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(p.mapped->duration(&e) == fr(16));
    TASK_CHECK(!o::is_error(e));

    auto s0 = p.mapped->sample_at(fr(0), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(s0.size() == 2);
    TASK_CHECK(sample_matches(s0, 0, p.A, 100));
    TASK_CHECK(sample_matches(s0, 1, p.C, 500));

    auto s6 = p.mapped->sample_at(fr(6), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(s6.size() == 2);
    TASK_CHECK(sample_matches(s6, 0, p.B, 200));
    TASK_CHECK(sample_matches(s6, 1, p.C, 512));

    // The first frame of the reverse outgoing segment is the left clip's
    // exclusive end, reported without an epsilon offset.
    auto s10 = p.mapped->sample_at(fr(10), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(s10.size() == 2);
    TASK_CHECK(sample_matches(s10, 0, p.A, 112));
    TASK_CHECK(sample_matches(s10, 1, p.C, 512));

    auto s13 = p.mapped->sample_at(fr(13), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(s13.size() == 2);
    TASK_CHECK(sample_matches(s13, 0, p.A, 106));
    TASK_CHECK(sample_matches(s13, 1, p.C, 506));

    // Freeze interior uses ordinary half-open ownership at source 12.
    auto s8 = p.mapped->sample_at(fr(8), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(s8.size() == 2);
    TASK_CHECK(sample_matches(s8, 0, p.B, 200));
    TASK_CHECK(sample_matches(s8, 1, p.C, 512));

    // Repeated access is stable.
    auto s13_again = p.mapped->sample_at(fr(13), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(s13_again.size() == s13.size());
    TASK_CHECK(sample_matches(s13_again, 0, p.A, 106));
    TASK_CHECK(sample_matches(s13_again, 1, p.C, 506));

    ErrorStatus out_error;
    auto        s16 = p.mapped->sample_at(fr(16), &out_error);
    TASK_CHECK(o::is_error(out_error));
    TASK_CHECK(s16.empty());

    auto all_5_11 = p.mapped->find_children<Clip>(
        &e,
        TimeRange(fr(5), fr(6)));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(clip_names_are(all_5_11, { "A", "B", "C" }));

    auto freeze_6_10 = p.mapped->find_children<Clip>(
        &e,
        TimeRange(fr(6), fr(4)));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(clip_names_are(freeze_6_10, { "B", "C" }));

    // Endpoint-only contact with B at output 6 is not positive contribution.
    auto before_6 = p.mapped->find_children<Clip>(
        &e,
        TimeRange(fr(5), fr(1)));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(clip_names_are(before_6, { "A", "C" }));

    // The reverse segment alone contributes A, not B.
    auto after_10 = p.mapped->find_children<Clip>(
        &e,
        TimeRange(fr(10), fr(1)));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(clip_names_are(after_10, { "A", "C" }));

    // A zero-duration query has no positive contribution.
    auto point = p.mapped->find_children<Clip>(
        &e,
        TimeRange(fr(8), fr(0)));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(point.empty());

    return 0;
}

int
relation_failures_gaps_and_transforms()
{
    ErrorStatus e;
    MainProject p = make_main_project(&e);
    TASK_CHECK(!o::is_error(e));

    auto old_map = p.mapped->time_map();
    ErrorStatus bad;
    TASK_CHECK(!p.mapped->set_time_map({ { fr(0), fr(0) } }, &bad));
    TASK_CHECK(o::is_error(bad));
    TASK_CHECK(p.mapped->time_map().size() == old_map.size());

    bad = ErrorStatus();
    TASK_CHECK(!p.mapped->set_time_map(
        { { fr(1), fr(0) }, { fr(2), fr(2) } },
        &bad));
    TASK_CHECK(o::is_error(bad));
    TASK_CHECK(p.mapped->time_map().size() == old_map.size());

    bad = ErrorStatus();
    TASK_CHECK(!p.mapped->set_time_map(
        { { fr(0), fr(0) }, { fr(1), fr(1) }, { fr(1), fr(2) } },
        &bad));
    TASK_CHECK(o::is_error(bad));
    TASK_CHECK(p.mapped->time_map().size() == old_map.size());

    bad = ErrorStatus();
    TASK_CHECK(!p.mapped->set_time_map(
        { { fr(0), fr(0) }, { fr(1), fr(25) } },
        &bad));
    TASK_CHECK(o::is_error(bad));
    TASK_CHECK(p.mapped->time_map().size() == old_map.size());

    bad = ErrorStatus();
    TASK_CHECK(!p.mapped->set_time_map(
        { { fr(0), fr(0) },
          { RationalTime(1, -24), RationalTime(1, 24) } },
        &bad));
    TASK_CHECK(o::is_error(bad));
    TASK_CHECK(p.mapped->time_map().size() == old_map.size());

    auto still = p.mapped->sample_at(fr(13), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(sample_matches(still, 0, p.A, 106));

    ErrorStatus transform_error;
    (void)p.A->transformed_time(fr(106), p.mapped.value, &transform_error);
    TASK_CHECK(o::is_error(transform_error));

    // Gap contributes nothing.
    o::SerializableObject::Retainer<Track> gap_track(new Track("gaps"));
    auto* gap_clip = new Clip("GClip", nullptr, rr(10, 4));
    gap_track->append_child(new o::Gap(rr(0, 4)), &e);
    gap_track->append_child(gap_clip, &e);
    o::SerializableObject::Retainer<TimeMappedStack> gap_map(
        new TimeMappedStack("gap_map"));
    gap_map->append_child(gap_track.value, &e);
    TASK_CHECK(gap_map->set_time_map(
        { { fr(0), fr(0) }, { fr(8), fr(8) } },
        &e));
    auto gap_samples = gap_map->sample_at(fr(2), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(gap_samples.empty());
    auto after_gap = gap_map->sample_at(fr(6), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(after_gap.size() == 1);
    TASK_CHECK(sample_matches(after_gap, 0, gap_clip, 12));

    // Disabled branches do not contribute.
    o::SerializableObject::Retainer<Track> hidden(new Track("hidden"));
    auto* hidden_clip = new Clip("Hidden", nullptr, rr(900, 12));
    hidden->append_child(hidden_clip, &e);
    hidden->set_enabled(false);
    p.mapped->append_child(hidden.value, &e);
    auto enabled_only = p.mapped->sample_at(fr(0), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(enabled_only.size() == 2);
    for (auto const& sample: enabled_only)
    {
        TASK_CHECK(sample.clip != hidden_clip);
    }

    // TimeEffect is rejected rather than partially interpreted.
    auto* warped = new Clip("Warped", nullptr, rr(0, 12));
    warped->effects().push_back(
        new o::LinearTimeWarp("warp", "LinearTimeWarp", 2.0));
    o::SerializableObject::Retainer<Track> warp_track(new Track);
    warp_track->append_child(warped, &e);
    o::SerializableObject::Retainer<TimeMappedStack> warp_map(
        new TimeMappedStack);
    warp_map->append_child(warp_track.value, &e);
    TASK_CHECK(warp_map->set_time_map(
        { { fr(0), fr(0) }, { fr(12), fr(12) } },
        &e));
    ErrorStatus warp_error;
    auto warp_samples = warp_map->sample_at(fr(3), &warp_error);
    TASK_CHECK(o::is_error(warp_error));
    TASK_CHECK(warp_samples.empty());

    // A hit transition is rejected.
    auto* transition_clip = new Clip("TClip", nullptr, rr(0, 12));
    o::SerializableObject::Retainer<Track> transition_track(new Track);
    transition_track->append_child(transition_clip, &e);
    transition_track->append_child(
        new o::Transition(
            "dissolve",
            o::Transition::Type::SMPTE_Dissolve,
            fr(6),
            fr(6)),
        &e);
    o::SerializableObject::Retainer<TimeMappedStack> transition_map(
        new TimeMappedStack);
    transition_map->append_child(transition_track.value, &e);
    TASK_CHECK(transition_map->set_time_map(
        { { fr(0), fr(0) }, { fr(12), fr(12) } },
        &e));
    ErrorStatus transition_error;
    auto transition_samples =
        transition_map->sample_at(fr(10), &transition_error);
    TASK_CHECK(o::is_error(transition_error));
    TASK_CHECK(transition_samples.empty());

    return 0;
}

int
relation_freeze_at_boundaries()
{
    ErrorStatus e;
    auto* X = new Clip("X", nullptr, rr(100, 12));
    o::SerializableObject::Retainer<Track> track(new Track);
    track->append_child(X, &e);

    o::SerializableObject::Retainer<TimeMappedStack> mapped(
        new TimeMappedStack);
    mapped->append_child(track.value, &e);
    TASK_CHECK(mapped->set_time_map(
        {
            { fr(0), fr(0) },
            { fr(4), fr(0) },  // freeze on the first frame boundary
            { fr(8), fr(12) },
            { fr(12), fr(12) } // freeze on the exclusive end
        },
        &e));

    auto first = mapped->sample_at(fr(2), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(first.size() == 1);
    TASK_CHECK(sample_matches(first, 0, X, 100));

    auto end = mapped->sample_at(fr(10), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(end.empty());

    auto frozen_start =
        mapped->find_children<Clip>(&e, TimeRange(fr(1), fr(2)));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(frozen_start.size() == 1);
    TASK_CHECK(frozen_start[0].value == X);

    auto frozen_end =
        mapped->find_children<Clip>(&e, TimeRange(fr(9), fr(2)));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(frozen_end.empty());
    return 0;
}

int
nested_time_mapped_stacks()
{
    ErrorStatus e;

    auto* D = new Clip("D", nullptr, rr(300, 12));
    o::SerializableObject::Retainer<Track> inner_track(new Track("inner_v"));
    inner_track->append_child(D, &e);

    o::SerializableObject::Retainer<TimeMappedStack> inner(
        new TimeMappedStack("inner"));
    inner->append_child(inner_track.value, &e);
    TASK_CHECK(inner->set_time_map(
        { { fr(0), fr(0) }, { fr(12), fr(12) } },
        &e));

    o::SerializableObject::Retainer<Track> outer_track(new Track("outer_v"));
    outer_track->append_child(inner.value, &e);

    o::SerializableObject::Retainer<TimeMappedStack> outer(
        new TimeMappedStack("outer"));
    outer->append_child(outer_track.value, &e);
    TASK_CHECK(outer->set_time_map(
        { { fr(0), fr(12) }, { fr(12), fr(0) } },
        &e));

    TASK_CHECK(outer->duration(&e) == fr(12));

    auto s0 = outer->sample_at(fr(0), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(s0.size() == 1);
    TASK_CHECK(sample_matches(s0, 0, D, 312));

    auto s6 = outer->sample_at(fr(6), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(s6.size() == 1);
    TASK_CHECK(sample_matches(s6, 0, D, 306));

    auto clips =
        outer->find_children<Clip>(&e, TimeRange(fr(0), fr(12)));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(clips.size() == 1);
    TASK_CHECK(clips[0].value == D);

    auto partial =
        outer->find_children<Clip>(&e, TimeRange(fr(0), fr(1)));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(partial.size() == 1);
    TASK_CHECK(partial[0].value == D);

    // Returned pointers borrow leaves inside the queried object.
    TASK_CHECK(s0[0].clip == D);
    return 0;
}

int
nested_mixed_time_bases()
{
    ErrorStatus e;

    auto* video25 =
        new Clip("V25", nullptr, TimeRange(fr(250, 25), fr(25, 25)));
    o::SerializableObject::Retainer<Track> video_track(new Track("v25"));
    video_track->append_child(video25, &e);

    auto* audio48 =
        new Clip("A48", nullptr, TimeRange(fr(48000, 48000), fr(48000, 48000)));
    o::SerializableObject::Retainer<Track> audio_track(new Track("a48"));
    audio_track->append_child(audio48, &e);

    o::SerializableObject::Retainer<TimeMappedStack> mixed(
        new TimeMappedStack("mixed"));
    mixed->append_child(video_track.value, &e);
    mixed->append_child(audio_track.value, &e);
    TASK_CHECK(mixed->set_time_map(
        { { fr(0, 24), fr(0, 48000) },
          { fr(24, 24), fr(48000, 48000) } },
        &e));

    auto samples = mixed->sample_at(RationalTime(10, 25), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(samples.size() == 2);
    TASK_CHECK(samples[0].clip == video25);
    TASK_CHECK(samples[0].source_time == RationalTime(260, 25));
    TASK_CHECK(samples[1].clip == audio48);
    TASK_CHECK(samples[1].source_time == RationalTime(67200, 48000));

    auto clips = mixed->find_children<Clip>(
        &e,
        TimeRange(RationalTime(5, 25), RationalTime(5, 25)));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(clips.size() == 2);
    TASK_CHECK(clips[0].value == video25);
    TASK_CHECK(clips[1].value == audio48);

    std::string json = mixed->to_json_string(&e);
    TASK_CHECK(!o::is_error(e));
    o::SerializableObject::Retainer<o::SerializableObject> reread(
        o::SerializableObject::from_json_string(json, &e));
    TASK_CHECK(!o::is_error(e));
    auto* reread_mixed = dynamic_cast<TimeMappedStack*>(reread.value);
    TASK_CHECK(reread_mixed != nullptr);
    auto reread_samples =
        reread_mixed->sample_at(RationalTime(10, 25), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(reread_samples.size() == 2);
    TASK_CHECK(reread_samples[0].source_time == RationalTime(260, 25));
    TASK_CHECK(reread_samples[1].source_time == RationalTime(67200, 48000));
    return 0;
}

int
editing_parent_trim_json_and_clone()
{
    ErrorStatus e;
    MainProject p = make_main_project(&e);
    TASK_CHECK(!o::is_error(e));

    o::SerializableObject::Retainer<Track> parent(new Track("parent"));
    parent->append_child(new o::Gap(rr(0, 3)), &e);
    parent->append_child(p.mapped.value, &e);
    TASK_CHECK(parent->duration(&e) == fr(19));

    o::SerializableObject::Retainer<Track> cut(
        o::track_trimmed_to_range(parent.value, rr(8, 8), &e));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(cut);
    TASK_CHECK(cut->children().size() == 1);

    auto* cut_map = dynamic_cast<TimeMappedStack*>(cut->children()[0].value);
    TASK_CHECK(cut_map != nullptr);
    TASK_CHECK(cut_map->time_map().size() == 4);
    TASK_CHECK(cut_map->duration(&e) == fr(8));
    TASK_CHECK(!o::is_error(e));

    auto first = cut_map->sample_at(fr(0), &e);
    auto fifth = p.mapped->sample_at(fr(5), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(first.size() == fifth.size());
    for (size_t i = 0; i < first.size(); ++i)
    {
        TASK_CHECK(first[i].source_time == fifth[i].source_time);
        TASK_CHECK(first[i].clip->name() == fifth[i].clip->name());
    }
    Clip const* cut_A = nullptr;
    Clip const* cut_C = nullptr;
    for (auto const& clip: cut_map->find_children<Clip>())
    {
        if (clip->name() == "A") cut_A = clip.value;
        if (clip->name() == "C") cut_C = clip.value;
    }
    TASK_CHECK(cut_A != nullptr);
    TASK_CHECK(cut_C != nullptr);
    TASK_CHECK(sample_matches(first, 0, cut_A, 110));
    TASK_CHECK(sample_matches(first, 1, cut_C, 510));

    // JSON write/read preserves dynamic type, curve phase, and edit range.
    std::string json = cut_map->to_json_string(&e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(!json.empty());
    o::SerializableObject::Retainer<o::SerializableObject> reread(
        o::SerializableObject::from_json_string(json, &e));
    TASK_CHECK(!o::is_error(e));
    auto* reread_map = dynamic_cast<TimeMappedStack*>(reread.value);
    TASK_CHECK(reread_map != nullptr);
    TASK_CHECK(reread_map != nullptr);
    TASK_CHECK(
        std::string(o::TimeMappedStack::Schema::name)
        == "TimeMappedStack");
    TASK_CHECK(reread_map->schema_version() == 1);
    TASK_CHECK(reread_map->time_map().size() == 4);
    TASK_CHECK(reread_map->duration(&e) == fr(8));
    auto reread_first = reread_map->sample_at(fr(0), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(reread_first.size() == first.size());
    for (size_t i = 0; i < reread_first.size(); ++i)
    {
        TASK_CHECK(
            reread_first[i].source_time == first[i].source_time);
        TASK_CHECK(reread_first[i].clip->name() == first[i].clip->name());
    }

    // Deep clone has independent curve and child graph.
    o::SerializableObject::Retainer<TimeMappedStack> copy(
        dynamic_cast<TimeMappedStack*>(p.mapped->clone(&e)));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(copy);
    TASK_CHECK(copy.value != p.mapped.value);
    TASK_CHECK(copy->children().size() == p.mapped->children().size());
    TASK_CHECK(copy->time_map().size() == p.mapped->time_map().size());
    TASK_CHECK(copy->set_time_map(
        { { fr(0), fr(0) }, { fr(24), fr(24) } },
        &e));
    TASK_CHECK(p.mapped->time_map().size() == 4);
    TASK_CHECK(copy->time_map().size() == 2);
    auto original_13 = p.mapped->sample_at(fr(13), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(sample_matches(original_13, 0, p.A, 106));

    copy->append_child(new Track("independent_extra"), &e);
    TASK_CHECK(copy->children().size() == p.mapped->children().size() + 1);
    return 0;
}

int
editing_repeated_trims()
{
    ErrorStatus e;
    MainProject p = make_main_project(&e);

    o::SerializableObject::Retainer<Track> parent(new Track);
    parent->append_child(new o::Gap(rr(0, 3)), &e);
    parent->append_child(p.mapped.value, &e);

    o::SerializableObject::Retainer<Track> cut_once(
        o::track_trimmed_to_range(parent.value, rr(8, 8), &e));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(cut_once->children().size() == 1);

    o::SerializableObject::Retainer<Track> cut_twice(
        o::track_trimmed_to_range(cut_once.value, rr(2, 4), &e));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(cut_twice->children().size() == 1);

    auto* mapped =
        dynamic_cast<TimeMappedStack*>(cut_twice->children()[0].value);
    TASK_CHECK(mapped != nullptr);
    TASK_CHECK(mapped->duration(&e) == fr(4));
    TASK_CHECK(!o::is_error(e));

    // Second trim starts two frames into the first eight-frame cut, so visible
    // zero is original output frame 7: still frozen at source 12 -> B/C.
    auto samples = mapped->sample_at(fr(0), &e);
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(samples.size() == 2);
    Clip const* cut_B = nullptr;
    Clip const* cut_C = nullptr;
    for (auto const& clip: mapped->find_children<Clip>())
    {
        if (clip->name() == "B") cut_B = clip.value;
        if (clip->name() == "C") cut_C = clip.value;
    }
    TASK_CHECK(cut_B != nullptr);
    TASK_CHECK(cut_C != nullptr);
    TASK_CHECK(sample_matches(samples, 0, cut_B, 200));
    TASK_CHECK(sample_matches(samples, 1, cut_C, 512));

    auto clips =
        cut_twice->find_children<Clip>(&e, TimeRange(fr(0), fr(4)));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(clip_names_are(clips, { "A", "B", "C" }));

    // The same crop is a source_range in the mapped stack's output domain.
    auto mapped_clips =
        mapped->find_children<Clip>(&e, TimeRange(fr(7), fr(4)));
    TASK_CHECK(!o::is_error(e));
    TASK_CHECK(clip_names_are(mapped_clips, { "A", "B", "C" }));

    // JSON after repeated trimming retains the four-point curve.
    std::string json = mapped->to_json_string(&e);
    TASK_CHECK(!o::is_error(e));
    o::SerializableObject::Retainer<o::SerializableObject> reread(
        o::SerializableObject::from_json_string(json, &e));
    TASK_CHECK(!o::is_error(e));
    auto* reread_mapped = dynamic_cast<TimeMappedStack*>(reread.value);
    TASK_CHECK(reread_mapped != nullptr);
    TASK_CHECK(reread_mapped->time_map().size() == 4);
    TASK_CHECK(reread_mapped->duration(&e) == fr(4));
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    { "relation", "samples_find_boundaries", relation_samples_and_find },
    { "relation",
      "failures_gaps_disabled_effects_transform",
      relation_failures_gaps_and_transforms },
    { "relation", "freeze_boundaries", relation_freeze_at_boundaries },
    { "nested", "nested_curves", nested_time_mapped_stacks },
    { "nested", "mixed_time_bases", nested_mixed_time_bases },
    { "editing",
      "parent_trim_json_clone",
      editing_parent_trim_json_and_clone },
    { "editing", "repeated_trims", editing_repeated_trims },
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
