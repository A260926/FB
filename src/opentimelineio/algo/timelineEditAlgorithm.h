// SPDX-License-Identifier: Apache-2.0
// Copyright Contributors to the OpenTimelineIO project

#pragma once

#include "opentimelineio/errorStatus.h"
#include "opentimelineio/timeline.h"
#include "opentimelineio/version.h"

namespace opentimelineio { namespace OPENTIMELINEIO_VERSION_NS {
namespace algo {

//! Ripple delete a time range from every track of a timeline.
//
// The cut is expressed in program time, starting at zero and *not* including
// the timeline's global_start_time.  The range is half-open: [start, end).
//
// The input timeline must have a tracks Stack without a source_range whose
// direct children are all Tracks.  A Track may itself have a source_range and
// contain Clips, Gaps and nested Tracks/Stacks; nesting is preserved rather
// than flattened.
//
// Every Track in the returned timeline has its source_range cleared.  Tracks
// are laid out from zero, exactly T - (cut duration) long, where T is the
// program length of the input timeline.  Short tracks are padded with a Gap.
//
// A direct child Item which crosses the cut is split into two independent
// copies (never re-joined).  Items which are wholly inside the cut disappear.
// Nested compositions are trimmed only through their own source_range; their
// internal structure, effects and markers are left untouched.  Markers on the
// tracks Stack are in program time, while markers on a top level Track are in
// that track's local (pre-trim) time; both are clipped to the visible range,
// split around the cut and shifted as appropriate.  Markers inside Clips and
// nested compositions keep their original source coordinates.
//
// Transitions belonging directly to a top level Track are checked against
// their range_of_child coverage.  A cut which positively overlaps a visible
// transition, or a Track source_range which truncates one, makes the whole
// operation fail.  Transitions which are completely outside the visible range
// are dropped, while fully visible ones are preserved with their offsets.
//
// TimeEffects are not supported anywhere in the input and cause an error.  The
// input timeline is never modified, whether the operation succeeds or fails.
//
// An empty cut (start == end) still validates the structure and range, then
// returns a completely independent copy of the original project.
//
// @param input The input timeline.  Must not be null.
// @param cut The half-open program-time interval [start, end) to delete.
// @param error_status Set to a non-OK status on failure (including invalid
//        arguments and structural problems).
// @return A new Timeline on success (owned by the caller according to the
//         usual Retainer rules), or nullptr on failure.
OTIO_API Timeline* timeline_ripple_delete(
    Timeline const*  input,
    TimeRange const& cut,
    ErrorStatus*     error_status = nullptr);

}}} // namespace opentimelineio::OPENTIMELINEIO_VERSION_NS::algo
