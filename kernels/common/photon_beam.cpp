// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "device.h"
#include "rtcore.h"
#include "../bvh/bvh_photon_beam.h"

#include <cmath>
#include <limits>

namespace embree
{
  bool PhotonBeamMap::validateBeam(const RTCPhotonBeam& beam)
  {
    if (!std::isfinite(beam.radius) || beam.radius <= 0.0)
      return false;

    for (size_t i = 0; i < 3; ++i)
    {
      if (!std::isfinite(beam.from[i]) || !std::isfinite(beam.to[i]))
        return false;
      if (!std::isfinite(beam.flux[i]) || beam.flux[i] < 0.0)
        return false;
    }

    const Vec3<double> d(beam.to[0]-beam.from[0],
                         beam.to[1]-beam.from[1],
                         beam.to[2]-beam.from[2]);
    if (!std::isfinite(beamDot(d,d)) || beamDot(d,d) <= 0.0)
      return false;

    return true;
  }

  PhotonBeamMap::PhotonBeamMap(Device* device)
    : device(device), refCounter(1), alloc(device, false), root(NodeRef::emptyNode)
  {
    device->refInc();
  }

  PhotonBeamMap::~PhotonBeamMap()
  {
    alloc.clear();
    if (device) device->refDec();
  }

  void PhotonBeamMap::build(const RTCPhotonBeam* input, size_t count)
  {
    if (count != 0 && input == nullptr)
      throw_RTCError(RTC_ERROR_INVALID_ARGUMENT, "photon beam array is null");

    beams.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
      if (!validateBeam(input[i]))
        throw_RTCError(RTC_ERROR_INVALID_ARGUMENT, "invalid photon beam descriptor");

      const Vec3<double> from(input[i].from[0], input[i].from[1], input[i].from[2]);
      const Vec3<double> to(input[i].to[0], input[i].to[1], input[i].to[2]);
      const Vec3<double> axis = to - from;
      const double length = sqrt(beamDot(axis,axis));

      Beam beam;
      beam.from = from;
      beam.to = to;
      beam.length = length;
      beam.w = axis * (1.0 / length);
      beam.radius = input[i].radius;
      beam.flux = Vec3<double>(input[i].flux[0], input[i].flux[1], input[i].flux[2]);
      beams.push_back(beam);
    }

    if (beams.empty())
    {
      root = NodeRef::emptyNode;
      return;
    }

    /* Flat cylinder bounds: the axis-aligned box of the segment grown
     * by the radius. This covers the entire flat-ended support; the
     * float bounds are rounded outwards so the double-precision support
     * is never clipped. */
    leafBounds.resize(beams.size());
    indices.resize(beams.size());
    for (size_t i = 0; i < beams.size(); ++i)
    {
      indices[i] = i;
      const Vec3<double> lo3 = min(beams[i].from, beams[i].to) - Vec3<double>(beams[i].radius);
      const Vec3<double> hi3 = max(beams[i].from, beams[i].to) + Vec3<double>(beams[i].radius);
      BBox3fa box;
      box.lower = Vec3fa(std::nextafter(float(lo3.x), -std::numeric_limits<float>::infinity()),
                         std::nextafter(float(lo3.y), -std::numeric_limits<float>::infinity()),
                         std::nextafter(float(lo3.z), -std::numeric_limits<float>::infinity()));
      box.upper = Vec3fa(std::nextafter(float(hi3.x),  std::numeric_limits<float>::infinity()),
                         std::nextafter(float(hi3.y),  std::numeric_limits<float>::infinity()),
                         std::nextafter(float(hi3.z),  std::numeric_limits<float>::infinity()));
      leafBounds[i] = box;
    }

    root = buildRecursive(0, indices.size(), 0);
  }
}

using namespace embree;

RTC_API RTCPhotonBeamMap rtcNewPhotonBeamMap(RTCDevice hdevice,
                                             const RTCPhotonBeam* beams,
                                             size_t count)
{
  Device* device = (Device*) hdevice;
  RTC_CATCH_BEGIN;
  RTC_TRACE(rtcNewPhotonBeamMap);
  RTC_VERIFY_HANDLE(hdevice);

  PhotonBeamMap* map = new PhotonBeamMap(device);
  try {
    map->build(beams, count);
  } catch (...) {
    delete map;
    throw;
  }
  return (RTCPhotonBeamMap) map;

  RTC_CATCH_END(device);
  return nullptr;
}

RTC_API void rtcRetainPhotonBeamMap(RTCPhotonBeamMap hmap)
{
  PhotonBeamMap* map = (PhotonBeamMap*) hmap;
  Device* device = map ? map->getDevice() : nullptr;
  RTC_CATCH_BEGIN;
  RTC_TRACE(rtcRetainPhotonBeamMap);
  RTC_VERIFY_HANDLE(hmap);
  map->refInc();
  RTC_CATCH_END(device);
}

RTC_API void rtcReleasePhotonBeamMap(RTCPhotonBeamMap hmap)
{
  PhotonBeamMap* map = (PhotonBeamMap*) hmap;
  Device* device = map ? map->getDevice() : nullptr;
  RTC_CATCH_BEGIN;
  RTC_TRACE(rtcReleasePhotonBeamMap);
  RTC_VERIFY_HANDLE(hmap);
  map->refDec();
  RTC_CATCH_END(device);
}

RTC_API bool rtcIntegratePhotonBeams(RTCPhotonBeamMap hmap,
                                     const RTCBeamQuery* query,
                                     double rgb[3],
                                     RTCBeamStats* stats)
{
  PhotonBeamMap* map = (PhotonBeamMap*) hmap;
  Device* device = map ? map->getDevice() : nullptr;
  RTC_CATCH_BEGIN;
  RTC_TRACE(rtcIntegratePhotonBeams);
  RTC_VERIFY_HANDLE(hmap);

  if (query == nullptr || rgb == nullptr)
    throw_RTCError(RTC_ERROR_INVALID_ARGUMENT, "invalid photon beam query argument");

  return map->integrate(*query, rgb, stats);

  RTC_CATCH_END(device);
  return false;
}

