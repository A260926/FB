// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#include "opentimelineio/algo/timelineEditAlgorithm.h"

#include "opentimelineio/clip.h"
#include "opentimelineio/composition.h"
#include "opentimelineio/effect.h"
#include "opentimelineio/gap.h"
#include "opentimelineio/item.h"
#include "opentimelineio/marker.h"
#include "opentimelineio/stack.h"
#include "opentimelineio/timeEffect.h"
#include "opentimelineio/track.h"
#include "opentimelineio/transition.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace otime = opentime::OPENTIME_VERSION_NS;

using otime::RationalTime;
using otime::TimeRange;

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {
namespace algo {

namespace {

// Tolerance, in seconds, used when comparing arbitrary-rate fractional times.
// It is well below one sample at 192kHz, so genuine fractional edits survive
// while floating point noise cannot create zero-length fragments.
constexpr double time_epsilon = 1e-9;

inline bool
is_finite_time(RationalTime const& time)
{
    return std::isfinite(time.value()) && std::isfinite(time.rate())
           && time.rate() > 0.0;
}

inline bool
is_finite_range(TimeRange const& range)
{
    // A valid range has a finite start and a finite, non-negative duration.
    return is_finite_time(range.start_time())
           && is_finite_time(range.duration())
           && range.duration().value() >= 0.0;
}

inline double
seconds(RationalTime const& time)
{
    return time.to_seconds();
}

inline bool
time_equal(RationalTime const& lhs, RationalTime const& rhs)
{
    return std::abs(seconds(lhs) - seconds(rhs)) <= time_epsilon;
}

// Strict ordering which also treats nearly-equal times as equal.
inline bool
time_less(RationalTime const& lhs, RationalTime const& rhs)
{
    return seconds(lhs) < seconds(rhs) - time_epsilon;
}

inline bool
positive_duration(RationalTime const& duration)
{
    return seconds(duration) > time_epsilon;
}

inline RationalTime
min_time(RationalTime const& lhs, RationalTime const& rhs)
{
    return time_less(rhs, lhs) ? rhs : lhs;
}

inline RationalTime
max_time(RationalTime const& lhs, RationalTime const& rhs)
{
    return time_less(lhs, rhs) ? rhs : lhs;
}

// Intersect two ranges, requiring a strictly positive overlap.  Touching at a
// single endpoint does not count, which matches half-open range semantics and
// lets transitions that merely touch the cut survive.
bool
intersect_positive(TimeRange const& lhs, TimeRange const& rhs, TimeRange* out)
{
    const RationalTime start = max_time(lhs.start_time(), rhs.start_time());
    const RationalTime end =
        min_time(lhs.end_time_exclusive(), rhs.end_time_exclusive());
    if (!positive_duration(end - start))
    {
        return false;
    }
    if (out)
    {
        *out = TimeRange(start, end - start);
    }
    return true;
}

bool
range_contains(TimeRange const& outer, TimeRange const& inner)
{
    return seconds(inner.start_time()) >= seconds(outer.start_time())
                                                       - time_epsilon
           && seconds(inner.end_time_exclusive())
                  <= seconds(outer.end_time_exclusive()) + time_epsilon;
}

bool
fail(
    ErrorStatus*              error_status,
    ErrorStatus::Outcome      outcome,
    std::string const&        details,
    SerializableObject const* object = nullptr)
{
    if (error_status)
    {
        *error_status = ErrorStatus(outcome, details, object);
    }
    return false;
}

bool
propagate(
    ErrorStatus*              error_status,
    ErrorStatus const&        incoming,
    std::string const&        context,
    SerializableObject const* object = nullptr)
{
    if (error_status)
    {
        *error_status = ErrorStatus(
            incoming.outcome,
            context + ": " + incoming.details,
            object ? object : incoming.object_details);
    }
    return false;
}

std::string
track_label(Track const* track, size_t index)
{
    std::string label = "track " + std::to_string(index);
    if (track && !track->name().empty())
    {
        label += " ('" + track->name() + "')";
    }
    return label;
}

// Recursively validate an item and all descendants without mutating anything.
// Rejects TimeEffects, uncomputable or non-finite ranges, illegal
// parent/child structure, cycles and unsupported child types.
bool
validate_item(
    Item const*                  item,
    std::string const&           where,
    std::set<Composable const*>& visited,
    ErrorStatus*                 error_status)
{
    if (!item)
    {
        return fail(
            error_status,
            ErrorStatus::TYPE_MISMATCH,
            where + ": null item");
    }
    if (!visited.insert(item).second)
    {
        return fail(
            error_status,
            ErrorStatus::OBJECT_CYCLE,
            where + ": object appears more than once",
            item);
    }

    for (auto const& effect: item->effects())
    {
        if (dynamic_cast<TimeEffect const*>(effect.value))
        {
            return fail(
                error_status,
                ErrorStatus::NOT_IMPLEMENTED,
                where + ": TimeEffect is not supported ('"
                    + effect->name() + "')",
                item);
        }
    }

    ErrorStatus     local_status;
    const TimeRange trimmed = item->trimmed_range(&local_status);
    if (is_error(&local_status))
    {
        return propagate(error_status, local_status, where, item);
    }
    if (!is_finite_range(trimmed))
    {
        return fail(
            error_status,
            ErrorStatus::INVALID_TIME_RANGE,
            where + ": item has a non-finite or invalid trimmed range",
            item);
    }

    auto const composition = dynamic_cast<Composition const*>(item);
    if (!composition)
    {
        return true;
    }

    auto const& children = composition->children();
    for (size_t index = 0; index < children.size(); ++index)
    {
        Composable const* child = children[index].value;
        const std::string child_where =
            where + " child " + std::to_string(index);
        if (!child)
        {
            return fail(
                error_status,
                ErrorStatus::TYPE_MISMATCH,
                child_where + " is null",
                composition);
        }
        if (child->parent() != composition)
        {
            return fail(
                error_status,
                ErrorStatus::NOT_A_CHILD_OF,
                child_where + " has an illegal parent",
                child);
        }

        if (auto const transition = dynamic_cast<Transition const*>(child))
        {
            if (!is_finite_time(transition->in_offset())
                || !is_finite_time(transition->out_offset()))
            {
                return fail(
                    error_status,
                    ErrorStatus::INVALID_TIME_RANGE,
                    child_where + " (transition) has an invalid offset",
                    transition);
            }
            continue;
        }

        if (auto const child_item = dynamic_cast<Item const*>(child))
        {
            if (!validate_item(
                    child_item,
                    child_where,
                    visited,
                    error_status))
            {
                return false;
            }
            continue;
        }

        return fail(
            error_status,
            ErrorStatus::TYPE_MISMATCH,
            child_where + " is an unsupported composable type",
            child);
    }

    return true;
}

// Map one marker through the cut.
//
// `marked` is already in program time, `visible` is the program range the
// owning object exposes, and `cut` is the half-open program interval being
// removed.
std::vector<TimeRange>
remap_marker(
    TimeRange const& marked,
    TimeRange const& visible,
    TimeRange const& cut,
    bool             empty_cut)
{
    std::vector<TimeRange> result;

    const bool point = std::abs(seconds(marked.duration())) <= time_epsilon;

    if (point)
    {
        const RationalTime position = marked.start_time();

        // Outside the visible range, or on its exclusive end: drop it.
        if (seconds(position) < seconds(visible.start_time()) - time_epsilon
            || seconds(position)
                   >= seconds(visible.end_time_exclusive()) - time_epsilon)
        {
            return result;
        }

        if (empty_cut)
        {
            result.push_back(marked);
            return result;
        }

        const RationalTime start = cut.start_time();
        const RationalTime end   = cut.end_time_exclusive();

        if (time_equal(position, start))
        {
            // Points exactly at the cut start are deleted.
            return result;
        }
        if (time_equal(position, end))
        {
            // Points exactly at the cut end survive, moved to the new
            // position of that instant.
            result.push_back(TimeRange(start, marked.duration()));
            return result;
        }
        if (time_less(position, start))
        {
            result.push_back(marked);
            return result;
        }
        if (time_less(end, position))
        {
            result.push_back(
                TimeRange(position - cut.duration(), marked.duration()));
            return result;
        }

        // Strictly inside the deleted interval.
        return result;
    }

    TimeRange clipped;
    if (!intersect_positive(marked, visible, &clipped))
    {
        return result;
    }

    if (empty_cut)
    {
        result.push_back(clipped);
        return result;
    }

    const RationalTime start = cut.start_time();
    const RationalTime end   = cut.end_time_exclusive();
    const RationalTime shift = cut.duration();

    if (time_less(clipped.start_time(), start))
    {
        const RationalTime left_end =
            min_time(clipped.end_time_exclusive(), start);
        if (positive_duration(left_end - clipped.start_time()))
        {
            result.push_back(
                TimeRange(clipped.start_time(), left_end - clipped.start_time()));
        }
    }

    if (time_less(end, clipped.end_time_exclusive()))
    {
        const RationalTime right_start = max_time(clipped.start_time(), end);
        if (positive_duration(clipped.end_time_exclusive() - right_start))
        {
            result.push_back(TimeRange(
                right_start - shift,
                clipped.end_time_exclusive() - right_start));
        }
    }

    return result;
}

// Replace markers of `target` with remapped copies of `originals`.
//
// `visible` is the program range the owner exposes, `local_offset` maps the
// owner's local time into program time (the track source_range start, or zero
// for the tracks stack), and the output coordinate is program time because
// output tracks no longer carry a source_range.
bool
replace_markers(
    Item*                                                      target,
    std::vector<SerializableObject::Retainer<Marker>> const&    originals,
    TimeRange const&                                           visible,
    TimeRange const&                                           cut,
    RationalTime const&                                        local_offset,
    bool                                                       empty_cut,
    std::string const&                                         where,
    ErrorStatus*                                               error_status)
{
    target->markers().clear();

    const bool shifted = seconds(local_offset) != 0.0;

    for (auto const& original: originals)
    {
        TimeRange marked = original->marked_range();
        if (!is_finite_range(marked))
        {
            return fail(
                error_status,
                ErrorStatus::INVALID_TIME_RANGE,
                where + ": marker '" + original->name()
                    + "' has a non-finite range",
                original.value);
        }
        if (shifted)
        {
            marked = TimeRange(
                marked.start_time() - local_offset,
                marked.duration());
        }

        for (auto const& piece:
             remap_marker(marked, visible, cut, empty_cut))
        {
            ErrorStatus local_status;
            auto        cloned = original->clone(&local_status);
            if (!cloned || is_error(&local_status))
            {
                return propagate(
                    error_status,
                    local_status,
                    where + ": failed to clone marker '"
                        + original->name() + "'",
                    original.value);
            }
            SerializableObject::Retainer<> guard(cloned);
            auto marker = dynamic_cast<Marker*>(cloned);
            if (!marker)
            {
                return fail(
                    error_status,
                    ErrorStatus::TYPE_MISMATCH,
                    where + ": cloned marker is not a Marker");
            }
            marker->set_marked_range(piece);
            target->markers().push_back(marker);
        }
    }
    return true;
}

// Clone one direct child item and trim only its visible window (source_range)
// to the given track-local interval.  Internal structure, effects, markers and
// media references are preserved by the deep clone and are never rewritten.
Item*
clone_item_trimmed(
    Item const*       item,
    TimeRange const&  local_range,
    TimeRange const&  trimmed_range,
    TimeRange const&  intersection,
    ErrorStatus*      error_status)
{
    ErrorStatus local_status;
    auto        cloned = item->clone(&local_status);
    if (!cloned || is_error(&local_status))
    {
        propagate(error_status, local_status, "failed to clone item", item);
        return nullptr;
    }

    auto piece = dynamic_cast<Item*>(cloned);
    if (!piece)
    {
        fail(
            error_status,
            ErrorStatus::TYPE_MISMATCH,
            "cloned object is not an Item");
        return nullptr;
    }
    SerializableObject::Retainer<> guard(piece);

    const bool full_item =
        time_equal(intersection.start_time(), local_range.start_time())
        && time_equal(intersection.duration(), local_range.duration());
    if (!full_item)
    {
        const RationalTime source_start =
            trimmed_range.start_time()
            + (intersection.start_time() - local_range.start_time());
        piece->set_source_range(
            TimeRange(source_start, intersection.duration()));
    }

    return dynamic_cast<Item*>(guard.take_value());
}

} // namespace

Timeline*
timeline_ripple_delete(
    Timeline const*  input,
    TimeRange const& cut,
    ErrorStatus*     error_status)
{
    if (error_status)
    {
        *error_status = ErrorStatus();
    }

    if (!input)
    {
        fail(
            error_status,
            ErrorStatus::INTERNAL_ERROR,
            "input timeline is null");
        return nullptr;
    }

    // Basic cut validation: finite values and rates, non-negative start and
    // duration so that 0 <= a <= b.
    if (!is_finite_time(cut.start_time()) || !is_finite_time(cut.duration()))
    {
        fail(
            error_status,
            ErrorStatus::INVALID_TIME_RANGE,
            "cut has a non-finite value or rate");
        return nullptr;
    }
    if (cut.duration().value() < 0.0)
    {
        fail(
            error_status,
            ErrorStatus::INVALID_TIME_RANGE,
            "cut has a negative duration");
        return nullptr;
    }
    if (seconds(cut.start_time()) < -time_epsilon)
    {
        fail(
            error_status,
            ErrorStatus::INVALID_TIME_RANGE,
            "cut starts before program time zero");
        return nullptr;
    }

    Stack* tracks = input->tracks();
    if (!tracks)
    {
        fail(
            error_status,
            ErrorStatus::MALFORMED_SCHEMA,
            "timeline has no tracks stack",
            input);
        return nullptr;
    }
    if (tracks->source_range())
    {
        fail(
            error_status,
            ErrorStatus::INVALID_TIME_RANGE,
            "tracks stack must not have a source_range",
            tracks);
        return nullptr;
    }

    // Structure: the tracks stack directly contains Tracks only.
    std::vector<Track const*> track_list;
    track_list.reserve(tracks->children().size());
    for (size_t index = 0; index < tracks->children().size(); ++index)
    {
        Composable const* child = tracks->children()[index].value;
        auto const        track = dynamic_cast<Track const*>(child);
        if (!track)
        {
            fail(
                error_status,
                ErrorStatus::TYPE_MISMATCH,
                "tracks stack child " + std::to_string(index)
                    + " is not a Track",
                child);
            return nullptr;
        }
        if (child->parent() != tracks)
        {
            fail(
                error_status,
                ErrorStatus::NOT_A_CHILD_OF,
                "tracks stack child " + std::to_string(index)
                    + " has an illegal parent",
                child);
            return nullptr;
        }
        track_list.push_back(track);
    }

    // Full recursive validation; the empty cut still has to pass this.
    std::set<Composable const*> visited;
    if (!validate_item(tracks, "tracks stack", visited, error_status))
    {
        return nullptr;
    }

    // Program length is the longest exposed track.  A track without a
    // source_range exposes its available range from zero; with a source_range
    // it exposes exactly that range, whose start maps to program zero.
    struct TrackInfo
    {
        Track const* track  = nullptr;
        RationalTime duration;
        RationalTime offset;
    };
    std::vector<TrackInfo> track_info;
    track_info.reserve(track_list.size());
    RationalTime program_length(0, 1);
    bool         have_length = false;

    for (size_t index = 0; index < track_list.size(); ++index)
    {
        Track const* track = track_list[index];
        ErrorStatus  local_status;
        const RationalTime duration = track->duration(&local_status);
        if (is_error(&local_status))
        {
            propagate(
                error_status,
                local_status,
                track_label(track, index),
                track);
            return nullptr;
        }
        if (!is_finite_time(duration))
        {
            fail(
                error_status,
                ErrorStatus::INVALID_TIME_RANGE,
                track_label(track, index) + ": non-finite duration",
                track);
            return nullptr;
        }

        RationalTime offset(0, duration.rate());
        if (auto const sr = track->source_range())
        {
            if (!is_finite_range(*sr))
            {
                fail(
                    error_status,
                    ErrorStatus::INVALID_TIME_RANGE,
                    track_label(track, index) + ": non-finite source_range",
                    track);
                return nullptr;
            }
            offset = sr->start_time();
        }

        if (!have_length || time_less(program_length, duration))
        {
            program_length = duration;
            have_length    = true;
        }
        track_info.push_back(TrackInfo{ track, duration, offset });
    }

    const RationalTime cut_end = cut.end_time_exclusive();
    if (!is_finite_time(cut_end)
        || seconds(cut_end) > seconds(program_length) + time_epsilon)
    {
        fail(
            error_status,
            ErrorStatus::INVALID_TIME_RANGE,
            "cut end is beyond the original program length");
        return nullptr;
    }

    const bool         empty_cut     = !positive_duration(cut.duration());
    const RationalTime result_length = program_length - cut.duration();

    // An empty cut changes nothing.  After the structure and range checks
    // above, return a completely independent copy of the original project.
    // Everything below handles a real deletion.
    if (empty_cut)
    {
        ErrorStatus      copy_status;
        SerializableObject* copy = input->clone(&copy_status);
        if (!copy || is_error(&copy_status))
        {
            propagate(
                error_status,
                copy_status,
                "failed to clone timeline",
                input);
            return nullptr;
        }
        auto copied_timeline = dynamic_cast<Timeline*>(copy);
        if (!copied_timeline)
        {
            SerializableObject::Retainer<> guard(copy);
            fail(
                error_status,
                ErrorStatus::TYPE_MISMATCH,
                "cloned object is not a Timeline");
            return nullptr;
        }
        return copied_timeline;
    }

    // Independent deep copy.  The input is never mutated below.
    ErrorStatus      clone_status;
    SerializableObject* cloned = input->clone(&clone_status);
    if (!cloned || is_error(&clone_status))
    {
        propagate(error_status, clone_status, "failed to clone timeline", input);
        return nullptr;
    }
    auto timeline = dynamic_cast<Timeline*>(cloned);
    if (!timeline)
    {
        SerializableObject::Retainer<> guard(cloned);
        fail(
            error_status,
            ErrorStatus::TYPE_MISMATCH,
            "cloned object is not a Timeline");
        return nullptr;
    }
    SerializableObject::Retainer<Timeline> output(timeline);

    Stack* output_tracks = output->tracks();
    output_tracks->clear_children();

    // Markers on the tracks stack are already in program time.
    {
        const double program_rate = program_length.rate();
        if (!replace_markers(
                output_tracks,
                input->tracks()->markers(),
                TimeRange(RationalTime(0, program_rate), program_length),
                cut,
                RationalTime(0, program_rate),
                empty_cut,
                "tracks stack",
                error_status))
        {
            return nullptr;
        }
    }

    for (size_t track_index = 0; track_index < track_info.size();
         ++track_index)
    {
        TrackInfo const&  info   = track_info[track_index];
        Track const*      source = info.track;
        const std::string label  = track_label(source, track_index);

        // The visible part of the track, in its own local (pre-trim) time.
        const RationalTime exposed_start  = info.offset;
        const RationalTime exposed_length = info.duration;
        const TimeRange    visible(exposed_start, exposed_length);

        // The cut expressed in the same local time.
        const TimeRange local_cut(
            exposed_start + cut.start_time(),
            cut.duration());

        // Retained intervals of the exposed range: before a and after b.
        // An empty cut keeps the range whole so items stay single.  Each
        // interval records which logical side it belongs to so transitions
        // are not misclassified when only one side survives.
        struct RetainedInterval
        {
            TimeRange range;
            bool      on_left;
        };
        std::vector<RetainedInterval> retained;
        if (empty_cut)
        {
            if (positive_duration(exposed_length))
            {
                retained.push_back(RetainedInterval{ visible, true });
            }
        }
        else
        {
            const RationalTime left_length =
                min_time(cut.start_time(), exposed_length);
            if (positive_duration(left_length))
            {
                retained.push_back(RetainedInterval{
                    TimeRange(exposed_start, left_length),
                    true});
            }
            if (time_less(cut_end, exposed_length))
            {
                retained.push_back(RetainedInterval{
                    TimeRange(
                        exposed_start + cut_end,
                        exposed_length - cut_end),
                    false});
            }
        }

        auto const& children = source->children();

        // First check every direct transition against the visible range and
        // the cut, before any children are rebuilt.  A transition is retained
        // on the left side when it ends at or before the cut start and on the
        // right side otherwise; since it cannot positively overlap the cut,
        // these two cases are exhaustive and touching endpoints are allowed.
        struct TransitionPlan
        {
            Transition const* transition  = nullptr;
            TimeRange         range;
            bool              on_left_side = true;
        };
        std::vector<TransitionPlan> transition_plans;
        for (size_t index = 0; index < children.size(); ++index)
        {
            auto const transition =
                dynamic_cast<Transition const*>(children[index].value);
            if (!transition)
            {
                continue;
            }
            const std::string child_label =
                label + " child " + std::to_string(index);

            ErrorStatus     local_status;
            const TimeRange transition_range =
                source->range_of_child_at_index(int(index), &local_status);
            if (is_error(&local_status))
            {
                propagate(
                    error_status,
                    local_status,
                    child_label + " (transition)",
                    transition);
                return nullptr;
            }

            TimeRange visible_part;
            if (!intersect_positive(
                    transition_range,
                    visible,
                    &visible_part))
            {
                // Completely outside the visible range: drop.
                continue;
            }

            // A track source_range which cuts a transition is fatal.
            if (!range_contains(visible, transition_range))
            {
                fail(
                    error_status,
                    ErrorStatus::CANNOT_TRIM_TRANSITION,
                    child_label
                        + " (transition): the track's visible range truncates the transition",
                    transition);
                return nullptr;
            }

            // A positive overlap with the deleted range is fatal.
            TimeRange overlap;
            if (!empty_cut
                && intersect_positive(
                    transition_range,
                    local_cut,
                    &overlap))
            {
                fail(
                    error_status,
                    ErrorStatus::CANNOT_TRIM_TRANSITION,
                    child_label
                        + " (transition): the deleted range overlaps the transition",
                    transition);
                return nullptr;
            }

            // The transition is fully visible and does not positively overlap
            // the cut.  For a non-empty cut it either ends at or before the
            // cut start (left side) or begins at or after the cut end (right
            // side).  An empty cut keeps the whole range in one pass, so all
            // fully visible transitions survive there.
            const bool on_left =
                empty_cut
                || !time_less(
                    local_cut.start_time(),
                    transition_range.end_time_exclusive());

            transition_plans.push_back(TransitionPlan{
                transition,
                transition_range,
                on_left});
        }

        // Clone the track, drop its source_range and rebuild children.
        ErrorStatus track_status;
        auto        cloned_track = source->clone(&track_status);
        if (!cloned_track || is_error(&track_status))
        {
            propagate(error_status, track_status, label, source);
            return nullptr;
        }
        auto track = dynamic_cast<Track*>(cloned_track);
        if (!track)
        {
            SerializableObject::Retainer<> guard(cloned_track);
            fail(
                error_status,
                ErrorStatus::TYPE_MISMATCH,
                label + ": cloned object is not a Track");
            return nullptr;
        }
        SerializableObject::Retainer<Track> track_guard(track);
        track->clear_children();
        track->set_source_range(std::nullopt);

        // Build the retained sides; the left side is emitted before the
        // right side, and within a side the original child order is kept.
        for (size_t interval_index = 0; interval_index < retained.size();
             ++interval_index)
        {
            const bool in_right_side = !retained[interval_index].on_left;

            for (size_t index = 0; index < children.size(); ++index)
            {
                Composable const*  child       = children[index].value;
                const std::string  child_label =
                    label + " child " + std::to_string(index);

                if (auto const transition =
                        dynamic_cast<Transition const*>(child))
                {
                    const auto plan = std::find_if(
                        transition_plans.begin(),
                        transition_plans.end(),
                        [transition](TransitionPlan const& candidate) {
                            return candidate.transition == transition;
                        });
                    if (plan == transition_plans.end())
                    {
                        continue;
                    }

                    // Each fully retained transition belongs to exactly one
                    // side; emit it only while walking that side.
                    const bool plan_on_left = plan->on_left_side;
                    const bool side_is_left = !in_right_side;
                    if (plan_on_left != side_is_left)
                    {
                        continue;
                    }

                    ErrorStatus clone_local;
                    auto cloned_transition = transition->clone(&clone_local);
                    if (!cloned_transition || is_error(&clone_local))
                    {
                        propagate(
                            error_status,
                            clone_local,
                            child_label + " (transition)",
                            transition);
                        return nullptr;
                    }
                    SerializableObject::Retainer<> transition_guard(
                        cloned_transition);
                    auto retained_transition =
                        dynamic_cast<Transition*>(cloned_transition);
                    if (!retained_transition)
                    {
                        fail(
                            error_status,
                            ErrorStatus::TYPE_MISMATCH,
                            child_label
                                + ": cloned object is not a Transition");
                        return nullptr;
                    }
                    if (!track->append_child(
                            retained_transition,
                            &clone_local))
                    {
                        propagate(
                            error_status,
                            clone_local,
                            child_label + " (transition)",
                            transition);
                        return nullptr;
                    }
                    continue;
                }

                auto const item = dynamic_cast<Item const*>(child);
                if (!item)
                {
                    fail(
                        error_status,
                        ErrorStatus::TYPE_MISMATCH,
                        child_label + " is not an Item",
                        child);
                    return nullptr;
                }

                ErrorStatus     local_status;
                const TimeRange trimmed =
                    item->trimmed_range(&local_status);
                if (is_error(&local_status))
                {
                    propagate(error_status, local_status, child_label, item);
                    return nullptr;
                }
                const TimeRange local_range =
                    source->range_of_child_at_index(int(index), &local_status);
                if (is_error(&local_status))
                {
                    propagate(error_status, local_status, child_label, item);
                    return nullptr;
                }

                TimeRange intersection;
                if (!intersect_positive(
                        local_range,
                        retained[interval_index].range,
                        &intersection))
                {
                    continue;
                }

                Item* piece = clone_item_trimmed(
                    item,
                    local_range,
                    trimmed,
                    intersection,
                    error_status);
                if (!piece)
                {
                    return nullptr;
                }
                SerializableObject::Retainer<Item> piece_guard(piece);
                ErrorStatus                        append_status;
                if (!track->append_child(piece, &append_status))
                {
                    propagate(
                        error_status,
                        append_status,
                        child_label,
                        item);
                    return nullptr;
                }
            }
        }

        // Track markers are in pre-trim local time.  After the source_range
        // is cleared the output track's local time is program time, and the
        // exposed range becomes [0, exposed_length) in program time.
        const double track_rate = exposed_length.rate();
        if (!replace_markers(
                track,
                source->markers(),
                TimeRange(
                    RationalTime(0, track_rate),
                    exposed_length),
                cut,
                exposed_start,
                empty_cut,
                label,
                error_status))
        {
            return nullptr;
        }

        // Pad to exactly the result length, never adding a zero-length gap.
        bool length_ok = false;
        for (int attempt = 0; attempt < 8 && !length_ok; ++attempt)
        {
            ErrorStatus  local_status;
            RationalTime current = track->duration(&local_status);
            if (is_error(&local_status))
            {
                propagate(error_status, local_status, label, track);
                return nullptr;
            }

            const RationalTime difference = result_length - current;
            const double       difference_seconds = seconds(difference);
            if (difference_seconds <= time_epsilon)
            {
                if (difference_seconds < -time_epsilon)
                {
                    fail(
                        error_status,
                        ErrorStatus::INTERNAL_ERROR,
                        label + ": content is longer than the result length",
                        track);
                    return nullptr;
                }
                length_ok = true;
                break;
            }

            SerializableObject::Retainer<Gap> gap(new Gap(difference));
            ErrorStatus                       gap_status;
            if (!track->append_child(gap.value, &gap_status))
            {
                propagate(error_status, gap_status, label, track);
                return nullptr;
            }
        }
        if (!length_ok)
        {
            fail(
                error_status,
                ErrorStatus::INTERNAL_ERROR,
                label + ": could not pad to the result length",
                track);
            return nullptr;
        }

        ErrorStatus append_status;
        if (!output_tracks->append_child(track, &append_status))
        {
            propagate(error_status, append_status, label, track);
            return nullptr;
        }
    }

    return output.take_value();
}

}}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS::algo
