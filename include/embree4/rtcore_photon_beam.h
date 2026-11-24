// Copyright 2025 Photon beam task.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "rtcore_device.h"
#include <stdbool.h>
#include <stdint.h>

RTC_NAMESPACE_BEGIN

/*! A short photon line segment with a finite-radius density kernel. */
struct RTCPhotonBeam
{
  double from[3];
  double to[3];
  double radius;
  double flux[3];
};

/*! Parameters for one camera/eye beam integration query. */
struct RTCBeamQuery
{
  double origin[3];
  double direction[3];
  double tnear;
  double tfar;
  double sigma_t;
  double sigma_s;
};

/*! Native traversal and intersection diagnostics for one query. */
struct RTCBeamStats
{
  uint64_t nodeVisits;
  uint64_t beamTests;
  uint64_t supportIntervals;
};

typedef struct RTCPhotonBeamMapTy* RTCPhotonBeamMap;

/*! Creates an immutable beam map. The input beams are deeply copied. */
RTC_API RTCPhotonBeamMap rtcNewPhotonBeamMap(RTCDevice device,
                                             const struct RTCPhotonBeam* beams,
                                             size_t beamCount);

/*! Retains a beam map. */
RTC_API void rtcRetainPhotonBeamMap(RTCPhotonBeamMap map);

/*! Releases a beam map. */
RTC_API void rtcReleasePhotonBeamMap(RTCPhotonBeamMap map);

/*! Integrates all beam kernels along the supplied query ray. */
RTC_API bool rtcIntegratePhotonBeams(RTCPhotonBeamMap map,
                                     const struct RTCBeamQuery* query,
                                     double rgb[3],
                                     struct RTCBeamStats* stats);

RTC_NAMESPACE_END
