// Copyright 2025 Photon beam task.
// SPDX-License-Identifier: Apache-2.0

#define RTC_EXPORT_API

#include "photon_beam_internal.h"

#include "default.h"
#include "device.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

namespace embree
{
  namespace
  {
    using LD = long double;

    LD dot3(const LD a[3], const LD b[3])
    {
      return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    void cross3(const LD a[3], const LD b[3], LD out[3])
    {
      out[0] = a[1] * b[2] - a[2] * b[1];
      out[1] = a[2] * b[0] - a[0] * b[2];
      out[2] = a[0] * b[1] - a[1] * b[0];
    }

    bool finiteDouble3(const double v[3])
    {
      return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
    }
  }

  void reportPhotonBeamError(Device* device, const char* message)
  {
    if (device)
      device->setDeviceErrorCode(RTC_ERROR_INVALID_ARGUMENT, message);
    else
      Device::setThreadErrorCode(RTC_ERROR_INVALID_ARGUMENT, message);
  }

  bool photonBeamDescriptorValid(const RTCPhotonBeam& beam)
  {
    if (!finiteDouble3(beam.from) || !finiteDouble3(beam.to) ||
        !std::isfinite(beam.radius) || !finiteDouble3(beam.flux))
      return false;

    const double dx = beam.to[0] - beam.from[0];
    const double dy = beam.to[1] - beam.from[1];
    const double dz = beam.to[2] - beam.from[2];
    const double length = std::hypot(dx, dy, dz);

    if (!std::isfinite(length) || !(length > 0.0) || !(beam.radius > 0.0))
      return false;

    return beam.flux[0] >= 0.0 && beam.flux[1] >= 0.0 && beam.flux[2] >= 0.0;
  }

  bool photonBeamQueryValid(const RTCBeamQuery& query)
  {
    if (!finiteDouble3(query.origin) || !finiteDouble3(query.direction))
      return false;
    if (!std::isfinite(query.tnear) || !std::isfinite(query.tfar) ||
        !std::isfinite(query.sigma_t) || !std::isfinite(query.sigma_s))
      return false;

    const bool directionNonZero =
      query.direction[0] != 0.0 || query.direction[1] != 0.0 || query.direction[2] != 0.0;
    if (!directionNonZero)
      return false;

    return query.tnear >= 0.0 && query.tnear <= query.tfar &&
           query.sigma_s >= 0.0 && query.sigma_s <= query.sigma_t;
  }

  bool photonBeamSupportInterval(const PhotonBeamData& beam,
                                 const RTCBeamQuery& query,
                                 LD& t0,
                                 LD& t1)
  {
    const LD inf = std::numeric_limits<LD>::infinity();
    LD lo = -inf;
    LD hi = inf;

    const LD o[3] = {(LD)query.origin[0], (LD)query.origin[1], (LD)query.origin[2]};
    const LD d[3] = {(LD)query.direction[0], (LD)query.direction[1], (LD)query.direction[2]};
    const LD f[3] = {(LD)beam.from[0], (LD)beam.from[1], (LD)beam.from[2]};
    const LD w[3] = {(LD)beam.w[0], (LD)beam.w[1], (LD)beam.w[2]};
    const LD radius = (LD)beam.radius;
    const LD length = (LD)beam.length;

    LD ro[3] = {o[0] - f[0], o[1] - f[1], o[2] - f[2]};

    /* Axial slab: 0 <= dot(X-from,w) <= L. */
    const LD s0 = dot3(ro, w);
    const LD sv = dot3(d, w);
    if (sv != 0.0L)
    {
      const LD ta = -s0 / sv;
      const LD tb = (length - s0) / sv;
      lo = std::max(lo, std::min(ta, tb));
      hi = std::min(hi, std::max(ta, tb));
    }
    else if (s0 < 0.0L || s0 > length)
    {
      return false;
    }

    /* Radial slab: distance(X, axis)^2 < radius^2.  Using cross products
       avoids subtracting two nearly equal projected vectors for near-parallel
       queries.  The quadratic is solved analytically and without an angle
       threshold or numerical stepping. */
    LD c0[3], c1[3];
    cross3(ro, w, c0);
    cross3(d, w, c1);

    const LD A = dot3(c1, c1);
    const LD B = dot3(c0, c1); /* polynomial linear coefficient is 2*B */
    const LD C = dot3(c0, c0) - radius * radius;

    if (A > 0.0L)
    {
      LD discriminant = B * B - A * C;
      /* FMA only repairs one rounding-level negative value caused by the
         algebra itself.  A genuinely tangent/non-intersecting set has no
         positive-length interval and therefore contributes zero. */
      if (discriminant < 0.0L)
        discriminant = std::fmal(-A, C, B * B);
      if (!(discriminant > 0.0L))
        return false;

      const LD root = std::sqrt(discriminant);
      const LD signedRoot = B >= 0.0L ? root : -root;
      const LD q = -(B + signedRoot);

      LD r0, r1;
      if (q != 0.0L)
      {
        r0 = q / A;
        r1 = C / q;
      }
      else
      {
        r0 = (-B - root) / A;
        r1 = (-B + root) / A;
      }

      const LD radialLo = std::min(r0, r1);
      const LD radialHi = std::max(r0, r1);
      lo = std::max(lo, radialLo);
      hi = std::min(hi, radialHi);
    }
    else
    {
      /* Exactly parallel to the axis.  Equality is the strict cylindrical
         boundary and has no interior support. */
      if (!(C < 0.0L))
        return false;
    }

    lo = std::max(lo, (LD)query.tnear);
    hi = std::min(hi, (LD)query.tfar);

    /* A zero-width interval (including exact tangency) has zero measure. */
    if (!(hi > lo))
      return false;

    t0 = lo;
    t1 = hi;
    return true;
  }

  void integratePhotonBeam(const PhotonBeamData& beam,
                           const RTCBeamQuery& query,
                           LD rgb[3],
                           uint64_t& supportIntervalCount)
  {
    supportIntervalCount = 0;

    LD t0, t1;
    if (!photonBeamSupportInterval(beam, query, t0, t1))
      return;

    supportIntervalCount = 1;

    const LD d[3] = {(LD)query.direction[0], (LD)query.direction[1], (LD)query.direction[2]};
    const LD dNorm = std::sqrt(dot3(d, d));
    const LD sigmaT = (LD)query.sigma_t;
    const LD sigmaS = (LD)query.sigma_s;
    const LD radius = (LD)beam.radius;
    const LD k = sigmaT * dNorm;
    const LD intervalLength = t1 - t0;
    const LD z = k * intervalLength;

    /* Eye-path integral:
       dNorm * integral exp(-k*(t-tnear)) dt.
       exp(-k*(t0-tnear)) is factored out, and -expm1(-z)/z avoids cancellation
       for very small optical thickness. */
    LD eyeIntegral;
    if (z == 0.0L)
    {
      eyeIntegral = dNorm * intervalLength;
    }
    else
    {
      LD finiteFactor;
      if (std::fabsl(z) < 1e-3L)
        finiteFactor = 1.0L - z / 2.0L + z * z / 6.0L - z * z * z / 24.0L +
                       z * z * z * z / 120.0L;
      else
        finiteFactor = -std::expm1l(-z) / z;

      eyeIntegral = dNorm * intervalLength *
                    std::expl(-k * (t0 - (LD)query.tnear)) * finiteFactor;
    }

    const LD pi2 = acosl(-1.0L) * acosl(-1.0L);
    for (int c = 0; c < 3; ++c)
    {
      const LD channelCoefficient =
        (LD)beam.flux[c] * sigmaS / ((LD)4.0L * pi2 * radius * radius);
      rgb[c] += channelCoefficient * eyeIntegral;
    }
  }
}

extern "C"
{
  RTC_API RTCPhotonBeamMap rtcNewPhotonBeamMap(RTCDevice hdevice,
                                               const RTCPhotonBeam* beams,
                                               size_t beamCount)
  {
    using namespace embree;

    Device* device = (Device*)hdevice;
    if (hdevice == nullptr)
    {
      embree::Device::setThreadErrorCode(RTC_ERROR_INVALID_ARGUMENT,
                                         "rtcNewPhotonBeamMap: null device");
      return nullptr;
    }

    if (beamCount != 0 && beams == nullptr)
    {
      embree::reportPhotonBeamError(device, "rtcNewPhotonBeamMap: null beam array");
      return nullptr;
    }

    for (size_t i = 0; i < beamCount; ++i)
    {
      if (!photonBeamDescriptorValid(beams[i]))
      {
        embree::reportPhotonBeamError(device, "rtcNewPhotonBeamMap: invalid photon beam");
        return nullptr;
      }
    }

    RTCPhotonBeamMap map = nullptr;
    bool deviceRetained = false;
    try
    {
      map = new RTCPhotonBeamMapTy;
      map->device = hdevice;
      map->embreeDevice = device;
      rtcRetainDevice(hdevice);
      deviceRetained = true;

      map->beams.resize(beamCount);
      for (size_t i = 0; i < beamCount; ++i)
      {
        PhotonBeamData& out = map->beams[i];
        std::memset(&out, 0, sizeof(out));

        for (int k = 0; k < 3; ++k)
        {
          out.from[k] = beams[i].from[k];
          out.to[k] = beams[i].to[k];
          out.flux[k] = beams[i].flux[k];
        }
        out.radius = beams[i].radius;

        const double dx = out.to[0] - out.from[0];
        const double dy = out.to[1] - out.from[1];
        const double dz = out.to[2] - out.from[2];
        out.length = std::hypot(dx, dy, dz);
        out.w[0] = dx / out.length;
        out.w[1] = dy / out.length;
        out.w[2] = dz / out.length;

        for (int k = 0; k < 3; ++k)
        {
          const double axialMin = std::min(out.from[k], out.to[k]);
          const double axialMax = std::max(out.from[k], out.to[k]);
          out.bmin[k] = axialMin - out.radius;
          out.bmax[k] = axialMax + out.radius;
        }
      }

      map->index.reset(createPhotonBeamBVH(device, map->beams));
      return map;
    }
    catch (const std::exception&)
    {
      if (map)
      {
        delete map;
        if (deviceRetained)
          rtcReleaseDevice(hdevice);
      }
      try
      {
        device->setDeviceErrorCode(RTC_ERROR_OUT_OF_MEMORY,
                                   "rtcNewPhotonBeamMap: allocation failed");
      }
      catch (...)
      {
      }
      return nullptr;
    }
    catch (...)
    {
      if (map)
      {
        delete map;
        if (deviceRetained)
          rtcReleaseDevice(hdevice);
      }
      return nullptr;
    }
  }

  RTC_API void rtcRetainPhotonBeamMap(RTCPhotonBeamMap map)
  {
    if (map)
      map->refCount.fetch_add(1, std::memory_order_relaxed);
  }

  RTC_API void rtcReleasePhotonBeamMap(RTCPhotonBeamMap map)
  {
    if (!map)
      return;

    if (map->refCount.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
      RTCDevice device = map->device;
      delete map;
      if (device)
        rtcReleaseDevice(device);
    }
  }

  RTC_API bool rtcIntegratePhotonBeams(RTCPhotonBeamMap map,
                                       const RTCBeamQuery* query,
                                       double rgb[3],
                                       RTCBeamStats* stats)
  {
    if (stats)
    {
      stats->nodeVisits = 0;
      stats->beamTests = 0;
      stats->supportIntervals = 0;
    }

    if (!map)
    {
      embree::Device::setThreadErrorCode(RTC_ERROR_INVALID_ARGUMENT,
                                 "rtcIntegratePhotonBeams: null beam map");
      return false;
    }

    if (!query || !rgb)
    {
      embree::reportPhotonBeamError(map->embreeDevice,
                            "rtcIntegratePhotonBeams: null query or rgb output");
      return false;
    }

    if (!embree::photonBeamQueryValid(*query))
    {
      embree::reportPhotonBeamError(map->embreeDevice,
                            "rtcIntegratePhotonBeams: invalid beam query");
      return false;
    }

    try
    {
      long double sum[3] = {0.0L, 0.0L, 0.0L};
      RTCBeamStats localStats{};
      map->index->integrate(*query, sum, localStats);

      rgb[0] = (double)sum[0];
      rgb[1] = (double)sum[1];
      rgb[2] = (double)sum[2];
      if (stats)
        *stats = localStats;
      return true;
    }
    catch (...)
    {
      embree::reportPhotonBeamError(map->embreeDevice,
                            "rtcIntegratePhotonBeams: integration failed");
      return false;
    }
  }
}
