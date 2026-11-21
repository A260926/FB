// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "rtcore_common.h"
#include "rtcore_device.h"

#include <stdint.h>

RTC_NAMESPACE_BEGIN

/* An immutable photon beam: a flat-ended finite cylinder of the given
 * radius, spanning from "from" to "to". The endpoints carry a spectrally
 * constant flux. All coordinates and values use double precision. */
struct RTCPhotonBeam
{
  double from[3];    // start point of the beam axis
  double to[3];      // end point of the beam axis
  double radius;     // cylinder radius, strictly positive
  double flux[3];    // spectral power, all components non-negative
};

/* A single eye ray query against a photon beam map. "direction" does not
 * have to be normalized; all geometry works in ray parameter space. */
struct RTCBeamQuery
{
  double origin[3];    // ray origin
  double direction[3]; // ray direction, must be non-zero
  double tnear;        // start of the integration interval, tnear >= 0
  double tfar;         // end of the integration interval, tfar >= tnear
  double sigma_t;      // extinction coefficient along the eye ray
  double sigma_s;      // scattering coefficient, 0 <= sigma_s <= sigma_t
};

/* Per-query traversal counters. All fields are reset to zero at the start
 * of every rtcIntegratePhotonBeams call and are only incremented at the
 * place the corresponding work actually happens. */
struct RTCBeamStats
{
  uint64_t nodeVisits;       // BVH nodes opened during traversal
  uint64_t beamTests;        // individual beams tested in leaves
  uint64_t supportIntervals; // beams whose support intersects the
                             // ray interval with positive length
};

/* Opaque handle of an immutable photon beam map. */
typedef struct RTCPhotonBeamMapTy* RTCPhotonBeamMap;

/* Creates a photon beam map from a deep copy of the given beams. The
 * device is retained for the lifetime of the map. An empty map (beams ==
 * nullptr and count == 0) is legal. Invalid input reports
 * RTC_ERROR_INVALID_ARGUMENT on the device and returns nullptr. */
RTC_API RTCPhotonBeamMap rtcNewPhotonBeamMap(RTCDevice device,
                                             const struct RTCPhotonBeam* beams,
                                             size_t count);

/* Increments the reference count of the photon beam map. */
RTC_API void rtcRetainPhotonBeamMap(RTCPhotonBeamMap map);

/* Decrements the reference count of the photon beam map, destroying it
 * when the count reaches zero. */
RTC_API void rtcReleasePhotonBeamMap(RTCPhotonBeamMap map);

/* Analytically integrates the contributions of all overlapping beams
 * along the query ray and adds the result to rgb[0..2]. Returns true on
 * success, false (without touching rgb or reading an invalid stats
 * pointer) when the query arguments are invalid. The stats object, when
 * non-null, always describes exactly this one query. The map itself is
 * immutable and may be queried concurrently from multiple threads. */
RTC_API bool rtcIntegratePhotonBeams(RTCPhotonBeamMap map,
                                     const struct RTCBeamQuery* query,
                                     double rgb[3],
                                     struct RTCBeamStats* stats);

RTC_NAMESPACE_END
