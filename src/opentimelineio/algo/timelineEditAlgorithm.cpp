// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#include "opentimelineio/algo/timelineEditAlgorithm.h"

#include "opentimelineio/clip.h"
#include "opentimelineio/gap.h"
#include "opentimelineio/marker.h"
#include "opentimelineio/stack.h"
#include "opentimelineio/timeEffect.h"
#include "opentimelineio/track.h"
#include "opentimelineio/transition.h"

#include <cmath>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace otime = opentime::OPENTIME_VERSION_NS;

using otime::RationalTime;
using otime::TimeRange;

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {
namespace algo {

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

bool
finite_time(RationalTime t) noexcept
{
    return std::isfinite(t.value()) && std::isfinite(t.rate()) && t.rate() > 0;
}

bool
finite_range(TimeRange r) noexcept
{
    return finite_time(r.start_time()) && finite_time(r.duration())
           && r.duration().value() >= 0.0;
}

RationalTime
min_time(RationalTime a, RationalTime b)
{
    return b < a ? b : a;
}

RationalTime
max_time(RationalTime a, RationalTime b)
{
    return a < b ? b : a;
}

RationalTime
clamp_time(RationalTime v, RationalTime lo, RationalTime hi)
{
    if (v < lo)
    {
        return lo;
    }
    if (hi < v)
    {
        return hi;
    }
    return v;
}

std::string
describe(SerializableObjectWithMetadata const* object)
{
    std::string const name = object ? object->name() : std::string();
    return name.empty() ? std::string("<unnamed>") : "'" + name + "'";
}

void
set_error(
    ErrorStatus*              error_status,
    ErrorStatus::Outcome      outcome,
    std::string const&        details,
    SerializableObject const* object = nullptr)
{
    if (error_status)
    {
        *error_status = ErrorStatus(outcome, details, object);
    }
}

// ---------------------------------------------------------------------------
// Validation (read-only, performed against the input timeline)
// ---------------------------------------------------------------------------

bool validate_item(
    Item const*                  item,
    std::string const&           path,
    std::set<Composable const*>& visited,
    ErrorStatus*                 error_status);

bool
validate_children(
    Composition const*           composition,
    std::string const&           path,
    std::set<Composable const*>& visited,
    ErrorStatus*                 error_status)
{
    auto const& children = composition->children();
    for (size_t i = 0; i < children.size(); ++i)
    {
        Composable* child = children[i].value;
        if (!child)
        {
            set_error(
                error_status,
                ErrorStatus::INTERNAL_ERROR,
                path + ": child " + std::to_string(i) + " is null");
            return false;
        }

        if (child->parent() != composition)
        {
            set_error(
                error_status,
                ErrorStatus::INTERNAL_ERROR,
                path + ": child " + std::to_string(i)
                    + " has an invalid parent link",
                child);
            return false;
        }

        std::string const child_path = path + " -> child "
                                       + std::to_string(i) + " "
                                       + describe(child);

        if (auto transition = dynamic_cast<Transition*>(child))
        {
            if (!dynamic_cast<Track const*>(composition))
            {
                set_error(
                    error_status,
                    ErrorStatus::TYPE_MISMATCH,
                    child_path + ": transitions are only supported directly "
                                 "inside Tracks",
                    child);
                return false;
            }

            if (!finite_time(transition->in_offset())
                || !finite_time(transition->out_offset()))
            {
                set_error(
                    error_status,
                    ErrorStatus::INVALID_TIME_RANGE,
                    child_path + ": transition offsets are not finite",
                    child);
                return false;
            }
            continue;
        }

        Item* item = dynamic_cast<Item*>(child);
        if (!item)
        {
            set_error(
                error_status,
                ErrorStatus::TYPE_MISMATCH,
                child_path + ": unsupported child type",
                child);
            return false;
        }

        if (!dynamic_cast<Clip*>(item) && !dynamic_cast<Gap*>(item)
            && !dynamic_cast<Track*>(item) && !dynamic_cast<Stack*>(item))
        {
            set_error(
                error_status,
                ErrorStatus::TYPE_MISMATCH,
                child_path + ": unsupported child subtype",
                child);
            return false;
        }

        if (!validate_item(item, child_path, visited, error_status))
        {
            return false;
        }
    }
    return true;
}

bool
validate_item(
    Item const*                  item,
    std::string const&           path,
    std::set<Composable const*>& visited,
    ErrorStatus*                 error_status)
{
    if (visited.count(item))
    {
        set_error(
            error_status,
            ErrorStatus::OBJECT_CYCLE,
            path + ": object appears more than once in the hierarchy",
            item);
        return false;
    }
    visited.insert(item);

    for (auto const& effect: item->effects())
    {
        if (dynamic_cast<TimeEffect const*>(effect.value))
        {
            set_error(
                error_status,
                ErrorStatus::NOT_IMPLEMENTED,
                path + ": time effects are not supported by ripple delete",
                item);
            return false;
        }
    }

    ErrorStatus local;
    TimeRange   trimmed = item->trimmed_range(&local);
    if (is_error(local))
    {
        set_error(error_status, local.outcome, path + ": " + local.details, item);
        return false;
    }

    if (!finite_range(trimmed))
    {
        set_error(
            error_status,
            ErrorStatus::INVALID_TIME_RANGE,
            path + ": cannot compute a finite, valid range",
            item);
        return false;
    }

    if (auto composition = dynamic_cast<Composition const*>(item))
    {
        return validate_children(composition, path, visited, error_status);
    }
    return true;
}

bool
validate_timeline(Timeline const* input, ErrorStatus* error_status)
{
    Stack* stack = input->tracks();
    if (!stack)
    {
        set_error(
            error_status,
            ErrorStatus::INTERNAL_ERROR,
            "tracks Stack is missing from the timeline");
        return false;
    }

    if (stack->source_range())
    {
        set_error(
            error_status,
            ErrorStatus::TYPE_MISMATCH,
            "the tracks Stack must not have a source_range",
            stack);
        return false;
    }

    std::string const stack_path = "tracks Stack";

    auto const& children = stack->children();
    std::set<Composable const*> visited;
    visited.insert(stack);

    for (size_t i = 0; i < children.size(); ++i)
    {
        Composable* child = children[i].value;
        if (!child)
        {
            set_error(
                error_status,
                ErrorStatus::INTERNAL_ERROR,
                stack_path + ": track " + std::to_string(i) + " is null");
            return false;
        }
        if (child->parent() != stack)
        {
            set_error(
                error_status,
                ErrorStatus::INTERNAL_ERROR,
                stack_path + ": track " + std::to_string(i)
                    + " has an invalid parent link",
                child);
            return false;
        }

        Track* track = dynamic_cast<Track*>(child);
        if (!track)
        {
            set_error(
                error_status,
                ErrorStatus::TYPE_MISMATCH,
                stack_path + " -> child " + std::to_string(i) + " "
                    + describe(child)
                    + ": the tracks Stack may only contain Tracks",
                child);
            return false;
        }

        std::string const track_path = stack_path + " -> track "
                                       + std::to_string(i) + " "
                                       + describe(track);
        if (!validate_item(track, track_path, visited, error_status))
        {
            return false;
        }
    }

    // Check the Stack's own effects and range last, so that any structural
    // problem inside a track is reported against that track first.
    for (auto const& effect: stack->effects())
    {
        if (dynamic_cast<TimeEffect const*>(effect.value))
        {
            set_error(
                error_status,
                ErrorStatus::NOT_IMPLEMENTED,
                stack_path
                    + ": time effects are not supported by ripple delete",
                stack);
            return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Deep-copy fragment helpers
// ---------------------------------------------------------------------------

bool
make_item_fragment(
    Item*                                    source,
    TimeRange const&                         intersection,
    RationalTime const&                      child_start,
    RationalTime const&                      child_end,
    std::string const&                       path,
    SerializableObject::Retainer<Composable>& out,
    ErrorStatus*                             error_status)
{
    ErrorStatus local;
    TimeRange   trimmed = source->trimmed_range(&local);
    if (is_error(local))
    {
        set_error(error_status, local.outcome, path + ": " + local.details, source);
        return false;
    }

    RationalTime const offset_in  = intersection.start_time() - child_start;
    RationalTime const offset_out = child_end - intersection.end_time_exclusive();
    RationalTime const new_start  = trimmed.start_time() + offset_in;
    RationalTime const new_end    = trimmed.end_time_exclusive() - offset_out;

    TimeRange const new_range =
        TimeRange::range_from_start_end_time(new_start, new_end);
    if (!finite_range(new_range))
    {
        set_error(
            error_status,
            ErrorStatus::INVALID_TIME_RANGE,
            path + ": trimming produced an invalid source_range",
            source);
        return false;
    }

    SerializableObject* raw = source->clone(&local);
    if (is_error(local) || !raw)
    {
        if (raw)
        {
            raw->possibly_delete();
        }
        set_error(
            error_status,
            is_error(local) ? local.outcome : ErrorStatus::INTERNAL_ERROR,
            path + ": could not deep-copy the item",
            source);
        return false;
    }

    Item* fragment = dynamic_cast<Item*>(raw);
    if (!fragment)
    {
        raw->possibly_delete();
        set_error(
            error_status,
            ErrorStatus::TYPE_MISMATCH,
            path + ": cloned object is not an Item",
            source);
        return false;
    }

    fragment->set_source_range(new_range);
    out = SerializableObject::Retainer<Composable>(fragment);
    return true;
}

bool
make_marker_fragment(
    Marker const*                            source,
    TimeRange const&                         new_range,
    std::string const&                       path,
    SerializableObject::Retainer<Marker>&    out,
    ErrorStatus*                             error_status)
{
    ErrorStatus         local;
    SerializableObject* raw = source->clone(&local);
    if (is_error(local) || !raw)
    {
        if (raw)
        {
            raw->possibly_delete();
        }
        set_error(
            error_status,
            is_error(local) ? local.outcome : ErrorStatus::INTERNAL_ERROR,
            path + ": could not deep-copy the marker",
            source);
        return false;
    }

    Marker* fragment = dynamic_cast<Marker*>(raw);
    if (!fragment)
    {
        raw->possibly_delete();
        set_error(
            error_status,
            ErrorStatus::TYPE_MISMATCH,
            path + ": cloned object is not a Marker",
            source);
        return false;
    }

    fragment->set_marked_range(new_range);
    out = SerializableObject::Retainer<Marker>(fragment);
    return true;
}

// ---------------------------------------------------------------------------
// Marker adjustment
//
// Markers are expressed relative to the visible window's start; the values
// produced are positions inside the rebuilt object, whose local time starts
// at zero. `vis_dur` is the length of the visible window and `cut` is the
// cut in the same (program) time base.
// ---------------------------------------------------------------------------

bool
adjust_markers(
    std::vector<SerializableObject::Retainer<Marker>> const& in,
    RationalTime const&                                     vis_start,
    RationalTime const&                                     vis_dur,
    TimeRange const&                                        cut,
    std::string const&                                      path,
    std::vector<SerializableObject::Retainer<Marker>>&      out,
    ErrorStatus*                                            error_status)
{
    RationalTime const zero(0, vis_dur.rate());

    auto emit = [&](Marker const*                          original,
                    RationalTime                           start,
                    RationalTime                           duration,
                    bool                                   keep_zero,
                    std::string const&                     marker_path) -> bool {
        if (!keep_zero && !(zero < duration))
        {
            return true;
        }
        SerializableObject::Retainer<Marker> fragment;
        if (!make_marker_fragment(
                original,
                TimeRange(start, duration),
                marker_path,
                fragment,
                error_status))
        {
            return false;
        }
        out.push_back(fragment);
        return true;
    };

    for (auto const& marker_ret: in)
    {
        Marker* marker = marker_ret.value;
        if (!marker)
        {
            continue;
        }

        TimeRange const range = marker->marked_range();
        if (!finite_range(range))
        {
            set_error(
                error_status,
                ErrorStatus::INVALID_TIME_RANGE,
                path + ": marker " + describe(marker)
                    + " has an invalid marked_range",
                marker);
            return false;
        }

        std::string const marker_path = path + ": marker " + describe(marker);

        RationalTime const ps = range.start_time() - vis_start;
        RationalTime const pe = range.end_time_exclusive() - vis_start;

        if (range.duration().value() == 0.0)
        {
            // A point marker exists only inside the half-open visible window.
            if (ps < zero || !(ps < vis_dur))
            {
                continue;
            }

            // Points at the cut start are deleted; points at the cut end
            // survive and slide to the cut start.
            if (!(ps < cut.start_time()) && ps < cut.end_time_exclusive())
            {
                continue;
            }

            RationalTime new_point = ps;
            if (!(ps < cut.end_time_exclusive()))
            {
                new_point = ps - cut.duration();
            }

            if (new_point < zero || !(new_point < vis_dur))
            {
                continue;
            }

            if (!emit(
                    marker,
                    new_point,
                    RationalTime(0, range.start_time().rate()),
                    /*keep_zero=*/true,
                    marker_path))
            {
                return false;
            }
            continue;
        }

        // Positive-length marker: intersect with the visible window first.
        RationalTime const visible_start = max_time(ps, zero);
        RationalTime const visible_end   = min_time(pe, vis_dur);
        if (!(visible_start < visible_end))
        {
            continue;
        }

        RationalTime const remove_start = min_time(cut.start_time(), vis_dur);
        RationalTime const remove_end   = min_time(cut.end_time_exclusive(), vis_dur);
        bool const         removed      = remove_start < remove_end;

        if (!removed)
        {
            if (!emit(
                    marker,
                    visible_start,
                    visible_end - visible_start,
                    /*keep_zero=*/false,
                    marker_path))
            {
                return false;
            }
            continue;
        }

        if (visible_start < remove_start)
        {
            RationalTime const left_end = min_time(visible_end, remove_start);
            if (!emit(
                    marker,
                    visible_start,
                    left_end - visible_start,
                    /*keep_zero=*/false,
                    marker_path))
            {
                return false;
            }
        }

        RationalTime const right_start = max_time(visible_start, remove_end);
        if (right_start < visible_end)
        {
            RationalTime const shifted = right_start - cut.duration();
            if (!emit(
                    marker,
                    shifted,
                    visible_end - right_start,
                    /*keep_zero=*/false,
                    marker_path))
            {
                return false;
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Track processing
// ---------------------------------------------------------------------------

struct OutChild
{
    SerializableObject::Retainer<Composable> object;
    bool                                    generated_gap = false;
};

void
append_gap(std::vector<OutChild>& out, RationalTime const& duration)
{
    if (!(duration.to_seconds() > 0.0))
    {
        return;
    }

    if (!out.empty() && out.back().generated_gap)
    {
        Gap* gap = dynamic_cast<Gap*>(out.back().object.value);
        if (gap && gap->source_range())
        {
            TimeRange const sr = *gap->source_range();
            gap->set_source_range(
                TimeRange(sr.start_time(), sr.duration() + duration));
            return;
        }
    }

    out.push_back(OutChild{
        SerializableObject::Retainer<Composable>(new Gap(duration)),
        true });
}

bool
process_track(
    Track*              track,
    TimeRange const&    cut,
    RationalTime const& output_duration,
    std::string const&  path,
    ErrorStatus*        error_status)
{
    // 1. The interval of the track that was exposed, in program time.
    ErrorStatus local;
    TimeRange   visible = track->trimmed_range(&local);
    if (is_error(local))
    {
        set_error(error_status, local.outcome, path + ": " + local.details, track);
        return false;
    }
    if (!finite_range(visible))
    {
        set_error(
            error_status,
            ErrorStatus::INVALID_TIME_RANGE,
            path + ": the track's visible range is not finite",
            track);
        return false;
    }

    RationalTime const vis_start = visible.start_time();
    RationalTime const vis_dur   = visible.duration();
    RationalTime const vis_end   = visible.end_time_exclusive();

    // 2. Translate the program-time cut into the track's local coordinates.
    RationalTime const cut_start = vis_start + cut.start_time();
    RationalTime const cut_end   = vis_start + cut.end_time_exclusive();
    RationalTime const left_cut  = clamp_time(cut_start, vis_start, vis_end);
    RationalTime const right_cut = clamp_time(cut_end, vis_start, vis_end);
    bool const         split     = left_cut < right_cut;

    std::vector<std::pair<RationalTime, RationalTime>> segments;
    if (split)
    {
        segments.emplace_back(vis_start, left_cut);
        segments.emplace_back(right_cut, vis_end);
    }
    else
    {
        segments.emplace_back(vis_start, vis_end);
    }

    // 3. Snapshot the direct children's ranges before changing anything.
    auto range_map = track->range_of_all_children(&local);
    if (is_error(local))
    {
        set_error(error_status, local.outcome, path + ": " + local.details, track);
        return false;
    }

    // 4. Decide what happens to direct transitions.
    std::unordered_map<Composable const*, size_t> transition_segment;
    for (auto const& child_ret: track->children())
    {
        Transition* transition = dynamic_cast<Transition*>(child_ret.value);
        if (!transition)
        {
            continue;
        }

        auto it = range_map.find(transition);
        if (it == range_map.end())
        {
            set_error(
                error_status,
                ErrorStatus::CANNOT_COMPUTE_AVAILABLE_RANGE,
                path + ": cannot compute the range of transition "
                    + describe(transition),
                transition);
            return false;
        }
        TimeRange const range = it->second;
        if (!finite_range(range))
        {
            set_error(
                error_status,
                ErrorStatus::INVALID_TIME_RANGE,
                path + ": transition " + describe(transition)
                    + " has an invalid range",
                transition);
            return false;
        }

        RationalTime const t_start = range.start_time();
        RationalTime const t_end   = range.end_time_exclusive();

        // Fully outside the visible window: invisible, drop it.
        if (!(vis_start < t_end) || !(t_start < vis_end))
        {
            continue;
        }

        // The track's explicit trim cuts through the transition. A track
        // without a source_range does not crop anything: a leading
        // transition's in_offset is a legitimate OTIO handle.
        if (track->source_range()
            && (t_start < vis_start || vis_end < t_end))
        {
            set_error(
                error_status,
                ErrorStatus::CANNOT_TRIM_TRANSITION,
                path + ": the track's visible range clips transition "
                    + describe(transition) + " in half",
                transition);
            return false;
        }

        // A positive-length intersection with the cut is not allowed;
        // touching at an endpoint is fine.
        RationalTime const overlap_start = max_time(t_start, cut_start);
        RationalTime const overlap_end   = min_time(t_end, cut_end);
        if (overlap_start < overlap_end)
        {
            set_error(
                error_status,
                ErrorStatus::CANNOT_TRIM_TRANSITION,
                path + ": the cut intersects transition "
                    + describe(transition),
                transition);
            return false;
        }

        size_t segment_index = 0;
        if (split)
        {
            if (t_end <= left_cut)
            {
                segment_index = 0;
            }
            else if (right_cut <= t_start)
            {
                segment_index = 1;
            }
            else
            {
                // Only possible for a zero-length transition sitting inside
                // the removed interval; it ceases to exist.
                continue;
            }
        }
        transition_segment[transition] = segment_index;
    }

    // 5. Build the output children from the actual intersections.
    std::vector<OutChild> output;

    for (size_t segment_index = 0; segment_index < segments.size();
         ++segment_index)
    {
        RationalTime const x = segments[segment_index].first;
        RationalTime const y = segments[segment_index].second;

        RationalTime cursor = x;

        for (auto const& child_ret: track->children())
        {
            Composable* child = child_ret.value;

            if (auto transition = dynamic_cast<Transition*>(child))
            {
                auto it = transition_segment.find(transition);
                if (it != transition_segment.end()
                    && it->second == segment_index)
                {
                    output.push_back(OutChild{
                        SerializableObject::Retainer<Composable>(transition),
                        false });
                }
                continue;
            }

            Item* item = dynamic_cast<Item*>(child);
            if (!item)
            {
                set_error(
                    error_status,
                    ErrorStatus::TYPE_MISMATCH,
                    path + ": child " + describe(child) + " is not an Item",
                    child);
                return false;
            }

            auto it = range_map.find(child);
            if (it == range_map.end())
            {
                set_error(
                    error_status,
                    ErrorStatus::CANNOT_COMPUTE_AVAILABLE_RANGE,
                    path + ": cannot compute the range of child "
                        + describe(child),
                    child);
                return false;
            }

            TimeRange const  child_range = it->second;
            RationalTime const child_start = child_range.start_time();
            RationalTime const child_end   = child_range.end_time_exclusive();

            RationalTime const isect_start = max_time(child_start, x);
            RationalTime const isect_end   = min_time(child_end, y);

            if (isect_start < isect_end)
            {
                if (cursor < isect_start)
                {
                    append_gap(output, isect_start - cursor);
                }

                SerializableObject::Retainer<Composable> fragment;
                if (!make_item_fragment(
                        item,
                        TimeRange(isect_start, isect_end - isect_start),
                        child_start,
                        child_end,
                        path + " -> child " + describe(item),
                        fragment,
                        error_status))
                {
                    return false;
                }
                output.push_back(OutChild{ fragment, false });
                cursor = isect_end;
                continue;
            }

            // Zero-length direct items behave like point markers: keep them
            // when they fall inside a surviving half-open segment.
            if (child_range.duration().value() == 0.0 && !(child_start < x)
                && child_start < y)
            {
                RationalTime const program_point = child_start - vis_start;
                if (!(program_point < cut.start_time())
                    && program_point < cut.end_time_exclusive())
                {
                    continue;
                }

                ErrorStatus         clone_status;
                SerializableObject* raw = item->clone(&clone_status);
                if (is_error(clone_status) || !raw)
                {
                    if (raw)
                    {
                        raw->possibly_delete();
                    }
                    set_error(
                        error_status,
                        is_error(clone_status) ? clone_status.outcome
                                               : ErrorStatus::INTERNAL_ERROR,
                        path + ": could not deep-copy child "
                            + describe(item),
                        item);
                    return false;
                }
                output.push_back(OutChild{
                    SerializableObject::Retainer<Composable>(
                        dynamic_cast<Composable*>(raw)),
                    false });
            }
        }

        // Anything in this segment not covered by an Item is blank. If the
        // segment ends with a transition and no Item follows it, the
        // transition's out_offset legitimately covers that space (a trailing
        // transition is measured by its outer offset, not by a Gap). The
        // exact overall length is reconciled in step 6.
        if (cursor < y)
        {
            bool tail_is_transition = !output.empty()
                                      && dynamic_cast<Transition*>(
                                             output.back().object.value);
            if (!tail_is_transition)
            {
                append_gap(output, y - cursor);
            }
        }
    }

    // 6. Pad to the exact output length. The duration of a Track is the sum
    //    of its Item durations, plus the outer offset of a leading/trailing
    //    transition. Transitions between children add no length.
    RationalTime item_total(0, output_duration.rate());
    for (auto const& child: output)
    {
        if (dynamic_cast<Transition*>(child.object.value))
        {
            // Transitions contribute only through their edge offsets, which
            // are handled below.
            continue;
        }
        Item* item = dynamic_cast<Item*>(child.object.value);
        if (!item)
        {
            set_error(
                error_status,
                ErrorStatus::TYPE_MISMATCH,
                path + ": output child is not an Item",
                child.object.value);
            return false;
        }
        ErrorStatus  item_status;
        RationalTime duration = item->duration(&item_status);
        if (is_error(item_status))
        {
            set_error(
                error_status,
                item_status.outcome,
                path + ": " + item_status.details,
                item);
            return false;
        }
        item_total += duration;
    }

    auto leading_offset = [&]() -> RationalTime {
        if (output.empty())
        {
            return RationalTime(0, output_duration.rate());
        }
        if (auto transition =
                dynamic_cast<Transition*>(output.front().object.value))
        {
            return transition->in_offset();
        }
        return RationalTime(0, output_duration.rate());
    };

    RationalTime trailing_offset(0, output_duration.rate());
    if (!output.empty())
    {
        if (auto transition =
                dynamic_cast<Transition*>(output.back().object.value))
        {
            trailing_offset = transition->out_offset();
        }
    }

    RationalTime const base =
        item_total + leading_offset() + trailing_offset;
    RationalTime padding = output_duration - base;

    if (padding.to_seconds() < -otime::DEFAULT_EPSILON_s)
    {
        set_error(
            error_status,
            ErrorStatus::INTERNAL_ERROR,
            path + ": reconstructed track is longer than the output "
                   "duration",
            track);
        return false;
    }

    if (padding.to_seconds() > otime::DEFAULT_EPSILON_s)
    {
        // A trailing transition keeps its outer offset only while it stays
        // last, so any padding has to go before it.
        if (!output.empty()
            && dynamic_cast<Transition*>(output.back().object.value))
        {
            if (!(padding.to_seconds() > 0.0))
            {
                return false;
            }
            output.insert(
                output.end() - 1,
                OutChild{ SerializableObject::Retainer<Composable>(
                              new Gap(padding)),
                          true });
        }
        else
        {
            append_gap(output, padding);
        }
    }

    // 7. Adjust the track's own markers (pre-trim local -> output local).
    std::vector<SerializableObject::Retainer<Marker>> new_markers;
    if (!adjust_markers(
            track->markers(),
            vis_start,
            vis_dur,
            cut,
            path + " markers",
            new_markers,
            error_status))
    {
        return false;
    }

    // 8. Commit. The output track starts at zero with no source_range.
    std::vector<Composable*> raw_children;
    raw_children.reserve(output.size());
    for (auto const& child: output)
    {
        raw_children.push_back(child.object.value);
    }

    track->clear_children();
    track->set_source_range(std::nullopt);
    if (!track->set_children(raw_children, &local))
    {
        set_error(
            error_status,
            is_error(local) ? local.outcome : ErrorStatus::INTERNAL_ERROR,
            path + ": could not install the rebuilt children",
            track);
        return false;
    }
    track->markers() = std::move(new_markers);

    return true;
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
        set_error(
            error_status,
            ErrorStatus::INTERNAL_ERROR,
            "timeline_ripple_delete: input timeline is null");
        return nullptr;
    }

    // --- Validate the cut in program time (global_start_time ignored). ---
    if (!finite_range(cut))
    {
        set_error(
            error_status,
            ErrorStatus::INVALID_TIME_RANGE,
            "timeline_ripple_delete: the cut must have finite values, a "
            "positive rate and a non-negative duration");
        return nullptr;
    }
    if (cut.start_time().value() < 0.0)
    {
        set_error(
            error_status,
            ErrorStatus::INVALID_TIME_RANGE,
            "timeline_ripple_delete: the cut start must be >= 0");
        return nullptr;
    }

    ErrorStatus  duration_status;
    RationalTime program_duration = input->duration(&duration_status);
    if (is_error(duration_status))
    {
        set_error(
            error_status,
            duration_status.outcome,
            "timeline_ripple_delete: cannot compute the timeline duration: "
                + duration_status.details);
        return nullptr;
    }
    if (!finite_time(program_duration) || program_duration.value() < 0.0)
    {
        set_error(
            error_status,
            ErrorStatus::INVALID_TIME_RANGE,
            "timeline_ripple_delete: the timeline duration is invalid");
        return nullptr;
    }
    if (program_duration < cut.end_time_exclusive())
    {
        set_error(
            error_status,
            ErrorStatus::INVALID_TIME_RANGE,
            "timeline_ripple_delete: the cut extends past the end of the "
            "timeline");
        return nullptr;
    }

    // --- Structural validation (also runs for an empty cut). ---
    if (!validate_timeline(input, error_status))
    {
        return nullptr;
    }

    // --- Make an independent copy; all further work happens on it. ---
    ErrorStatus         clone_status;
    SerializableObject* clone_raw = input->clone(&clone_status);
    if (is_error(clone_status) || !clone_raw)
    {
        if (clone_raw)
        {
            clone_raw->possibly_delete();
        }
        set_error(
            error_status,
            is_error(clone_status) ? clone_status.outcome
                                   : ErrorStatus::INTERNAL_ERROR,
            "timeline_ripple_delete: could not copy the timeline");
        return nullptr;
    }

    SerializableObject::Retainer<Timeline> result(
        dynamic_cast<Timeline*>(clone_raw));
    if (!result)
    {
        clone_raw->possibly_delete();
        set_error(
            error_status,
            ErrorStatus::TYPE_MISMATCH,
            "timeline_ripple_delete: cloned object is not a Timeline");
        return nullptr;
    }

    // An empty cut is a no-op: return an independent copy of the project.
    if (cut.start_time() == cut.end_time_exclusive())
    {
        return result.take_value();
    }

    // --- Ripple delete every track. ---
    RationalTime const output_duration = program_duration - cut.duration();

    Stack*      stack  = result->tracks();
    auto const& tracks = stack->children();
    for (size_t i = 0; i < tracks.size(); ++i)
    {
        Track* track = dynamic_cast<Track*>(tracks[i].value);
        if (!track)
        {
            set_error(
                error_status,
                ErrorStatus::TYPE_MISMATCH,
                "timeline_ripple_delete: tracks Stack child "
                    + std::to_string(i) + " is not a Track",
                tracks[i].value);
            return nullptr;
        }

        std::string const path =
            "track " + std::to_string(i) + " " + describe(track);
        if (!process_track(
                track,
                cut,
                output_duration,
                path,
                error_status))
        {
            return nullptr;
        }
    }

    // --- Adjust markers on the tracks Stack (program time). ---
    std::vector<SerializableObject::Retainer<Marker>> stack_markers;
    if (!adjust_markers(
            stack->markers(),
            RationalTime(0, program_duration.rate()),
            program_duration,
            cut,
            "tracks Stack markers",
            stack_markers,
            error_status))
    {
        return nullptr;
    }
    stack->markers() = std::move(stack_markers);

    return result.take_value();
}

}}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS::algo
