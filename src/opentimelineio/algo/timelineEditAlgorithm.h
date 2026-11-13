// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#pragma once

#include "opentimelineio/timeline.h"
#include "opentimelineio/version.h"

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {
namespace algo {

/// @brief Ripple delete a time range from every track of a timeline.
///
/// The cut is expressed in the timeline's program time, i.e. starting at
/// zero and ignoring Timeline::global_start_time. The half-open interval
/// [cut.start_time(), cut.end_time_exclusive()) is removed. Everything that
/// follows the cut is pulled left by the cut duration, and every output
/// track is padded with a Gap so that it is exactly
/// `timeline.duration() - cut.duration()` long.
///
/// The input timeline must be a Stack of Tracks (the Stack must not have a
/// source_range). Each Track may have a source_range and may contain Clips,
/// Gaps and nested Tracks/Stacks. Nested objects are copied as a unit and
/// only their source_range (their visible trim as an Item) is adjusted.
///
/// Items which straddle the cut are duplicated into two independent
/// fragments. Markers on the tracks Stack are adjusted in program time;
/// markers on top-level Tracks are adjusted in the track's pre-trim local
/// time. Markers inside Clips and nested objects keep their source
/// coordinates. Transitions directly owned by a top-level Track are checked
/// against their `range_of_child`; a cut which has a positive-length
/// intersection with a visible transition, or a track visible range which
/// partially clips a transition, makes the whole operation fail.
///
/// TimeEffects are not supported at any level.
///
/// The input timeline is never modified, whether the operation succeeds or
/// fails. On success the returned Timeline is owned by the caller, following
/// the library's usual Retainer rules. On failure nullptr is returned and
/// `error_status` describes the track or child which caused the failure.
OTIO_API Timeline* timeline_ripple_delete(
    Timeline const*  input,
    TimeRange const& cut,
    ErrorStatus*     error_status = nullptr);

}}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS::algo
