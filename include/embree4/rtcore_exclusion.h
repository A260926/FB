// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "rtcore_common.h"

#include <stdint.h>

RTC_NAMESPACE_BEGIN

/* Opaque handle for an immutable set of instance path exclusion rules. */
typedef struct RTCInstanceExclusionTy* RTCInstanceExclusion;

/* One edge of an instance path. A path is an ordered sequence of edges
 * from the root scene towards an inner instance.
 *
 * geomID:     geometry ID of the instance geometry inside its parent scene.
 *             For a regular instance (RTC_GEOMETRY_TYPE_INSTANCE) this is the
 *             instance's geometry ID. For an instance array
 *             (RTC_GEOMETRY_TYPE_INSTANCE_ARRAY) this is the array geometry
 *             ID as well.
 * instPrimID: element number inside an instance array. For a regular
 *             instance this must be RTC_INVALID_GEOMETRY_ID.
 */
struct RTCInstanceExclusionPath
{
  unsigned int geomID;
  unsigned int instPrimID;
};

/* Per-query statistics of the sparse path trie. Both counters are reset to
 * zero at the beginning of each ray query when exclusionStats is provided.
 *
 * transitions: number of trie edge transition attempts performed while
 *              descending through instances (one attempt per active lane and
 *              traversed instance edge, including attempts that do not find
 *              a matching edge).
 * comparisons: number of key comparisons performed while looking up edges
 *              inside the sparse trie.
 * Rays without an exclusion rule never touch the trie and add no counts.
 */
struct RTCInstanceExclusionStats
{
  uint64_t transitions;
  uint64_t comparisons;
};

/* Creates an immutable instance exclusion handle. The rules are a table of
 * pathCount paths. paths[i] points to an ordered array of
 * RTCInstanceExclusionPath edges from the root scene towards an inner
 * instance, and pathLengths[i] gives its number of edges. Prefix matching
 * excludes the entire subtree below a matched path. Duplicate paths are
 * merged, and descendant paths already covered by an ancestor path do not
 * add query work.
 *
 * An empty rule table (pathCount == 0) is legal and produces a handle that
 * never excludes anything. Both edge arrays and the two pointer tables are
 * deep copied, so the caller may release or reuse all input arrays
 * immediately after the call.
 *
 * RTC_ERROR_INVALID_ARGUMENT is reported on the device and a null handle
 * returned when: device is invalid, pathCount is non-zero with a null
 * paths or null pathLengths table, paths[i] is null while
 * pathLengths[i] is non-zero, a path length is zero or larger than
 * RTC_MAX_INSTANCE_LEVEL_COUNT, or an edge carries an invalid geomID
 * (RTC_INVALID_GEOMETRY_ID).
 */
RTC_API RTCInstanceExclusion rtcNewInstanceExclusion(
  RTCDevice device,
  const struct RTCInstanceExclusionPath* const* paths,
  const size_t* pathLengths,
  size_t pathCount);

/* Increments the reference count of the exclusion handle. */
RTC_API void rtcRetainInstanceExclusion(RTCInstanceExclusion exclusion);

/* Decrements the reference count of the exclusion handle and destroys it
 * when the count reaches zero. */
RTC_API void rtcReleaseInstanceExclusion(RTCInstanceExclusion exclusion);

RTC_NAMESPACE_END
