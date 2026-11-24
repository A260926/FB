// Copyright 2025 Photon beam task.
// SPDX-License-Identifier: Apache-2.0

#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace photon_beams
{
  namespace
  {
    constexpr double kRayFar = 1.0e6;
    constexpr double kBoundaryEps = 1.0e-9;

    std::uint64_t splitmix64(std::uint64_t x)
    {
      x += 0x9e3779b97f4a7c15ULL;
      x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
      x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
      return x ^ (x >> 31);
    }

    struct DeterministicRng
    {
      std::uint64_t state;

      explicit DeterministicRng(std::uint64_t identity)
        : state(splitmix64(identity ^ 0x243f6a8885a308d3ULL))
      {
      }

      std::uint64_t nextU64()
      {
        state += 0x9e3779b97f4a7c15ULL;
        std::uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
      }

      double uniform()
      {
        return double(nextU64() >> 11) * 0x1.0p-53;
      }
    };

    Vec3 uniformSphere(DeterministicRng& rng)
    {
      const double u = rng.uniform();
      const double z = 1.0 - 2.0 * u;
      const double phi = 2.0 * std::acos(-1.0) * rng.uniform();
      const double radial = std::sqrt(std::max(0.0, 1.0 - z * z));
      return {radial * std::cos(phi), radial * std::sin(phi), z};
    }

    RTCPhotonBeam makeBeam(const Vec3& from, const Vec3& to, double radius, const Vec3& flux)
    {
      RTCPhotonBeam b{};
      b.from[0] = from.x; b.from[1] = from.y; b.from[2] = from.z;
      b.to[0] = to.x;     b.to[1] = to.y;     b.to[2] = to.z;
      b.radius = radius;
      b.flux[0] = flux.x; b.flux[1] = flux.y; b.flux[2] = flux.z;
      return b;
    }

    RTCBeamStats addStats(const RTCBeamStats& a, const RTCBeamStats& b)
    {
      return {a.nodeVisits + b.nodeVisits,
              a.beamTests + b.beamTests,
              a.supportIntervals + b.supportIntervals};
    }
  }

  Vec3 normalize(const Vec3& v)
  {
    const double n = length(v);
    if (!(n > 0.0))
      return {0.0, 0.0, 1.0};
    return v / n;
  }

  bool Box::contains(const Vec3& p) const
  {
    return p.x >= lower.x && p.x <= upper.x &&
           p.y >= lower.y && p.y <= upper.y &&
           p.z >= lower.z && p.z <= upper.z;
  }

  bool Box::interval(const Vec3& origin, const Vec3& direction,
                     double& t0, double& t1) const
  {
    t0 = -std::numeric_limits<double>::infinity();
    t1 = std::numeric_limits<double>::infinity();

    const double o[3] = {origin.x, origin.y, origin.z};
    const double d[3] = {direction.x, direction.y, direction.z};
    const double lo[3] = {lower.x, lower.y, lower.z};
    const double hi[3] = {upper.x, upper.y, upper.z};

    for (int axis = 0; axis < 3; ++axis)
    {
      if (d[axis] == 0.0)
      {
        if (o[axis] < lo[axis] || o[axis] > hi[axis])
          return false;
      }
      else
      {
        double a = (lo[axis] - o[axis]) / d[axis];
        double b = (hi[axis] - o[axis]) / d[axis];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (!(t0 <= t1))
          return false;
      }
    }
    return std::isfinite(t0) && std::isfinite(t1);
  }

  Renderer::Renderer(RTCDevice device,
                     RTCScene scene,
                     const Box& mediumBox,
                     double sigmaT_,
                     double sigmaS_,
                     const Vec3& lightPosition_,
                     const Vec3& lightPower_,
                     const PerspectiveCamera& camera_,
                     int imageWidth,
                     int imageHeight,
                     const Settings& settings_)
    : mediumBox(mediumBox),
      sigmaT(sigmaT_),
      sigmaS(sigmaS_),
      lightPosition(lightPosition_),
      lightPower(lightPower_),
      camera(camera_),
      width_(std::max(1, imageWidth)),
      height_(std::max(1, imageHeight)),
      settings(settings_)
  {
    deviceHandle = device;
    sceneHandle = scene;
    if (deviceHandle) rtcRetainDevice(deviceHandle);
    if (sceneHandle) rtcRetainScene(sceneHandle);

    settings.maxEvents = std::clamp(settings.maxEvents, 1, 32);
    if (!(settings.radius > 0.0))
      settings.radius = 1.0e-3;
    currentRadius = settings.radius;

    sigmaT = std::max(0.0, sigmaT);
    sigmaS = std::clamp(sigmaS, 0.0, sigmaT);

    filmSum.assign(static_cast<std::size_t>(width_) * height_, Vec3{});
  }

  Renderer::~Renderer()
  {
    releaseMap();
    if (sceneHandle) rtcReleaseScene(sceneHandle);
    if (deviceHandle) rtcReleaseDevice(deviceHandle);
  }

  void Renderer::releaseMap()
  {
    if (map_)
    {
      rtcReleasePhotonBeamMap(map_);
      map_ = nullptr;
    }
  }

  std::uint64_t Renderer::identityFor(std::size_t round, std::size_t photonIndex) const
  {
    const std::uint64_t a = splitmix64(settings.seed ^ splitmix64(round + 0x123456789abcdef0ULL));
    const std::uint64_t b = splitmix64(static_cast<std::uint64_t>(photonIndex) +
                                       0xfedcba9876543210ULL);
    return splitmix64(a ^ b ^ 0x0ff1ce42ULL);
  }

  Vec3 Renderer::rayDirection(int x, int y) const
  {
    const double aspect = double(width_) / double(height_);
    const double scale = std::tan(camera.fovYDegrees * std::acos(-1.0) / 360.0);
    const double px = (2.0 * (double(x) + 0.5) / double(width_) - 1.0) * aspect * scale;
    const double py = (1.0 - 2.0 * (double(y) + 0.5) / double(height_)) * scale;

    const Vec3 forward = normalize(camera.target - camera.origin);
    Vec3 right = normalize(cross(forward, camera.up));
    if (length(right) == 0.0)
      right = {1.0, 0.0, 0.0};
    const Vec3 up = cross(right, forward);
    return normalize(forward + right * px + up * py);
  }

  bool Renderer::traceSurface(const Vec3& origin,
                              const Vec3& direction,
                              double& distance,
                              Vec3& geometricNormal) const
  {
    RTCRayHit rayHit{};
    rayHit.ray.org_x = float(origin.x);
    rayHit.ray.org_y = float(origin.y);
    rayHit.ray.org_z = float(origin.z);
    rayHit.ray.dir_x = float(direction.x);
    rayHit.ray.dir_y = float(direction.y);
    rayHit.ray.dir_z = float(direction.z);
    rayHit.ray.tnear = 0.0f;
    rayHit.ray.tfar = float(kRayFar);
    rayHit.ray.mask = ~0u;
    rayHit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    rayHit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;

    rtcIntersect1(sceneHandle, &rayHit);
    if (rayHit.hit.geomID == RTC_INVALID_GEOMETRY_ID)
      return false;

    distance = double(rayHit.ray.tfar);
    geometricNormal = normalize({double(rayHit.hit.Ng_x),
                                double(rayHit.hit.Ng_y),
                                double(rayHit.hit.Ng_z)});
    if (dot(geometricNormal, direction) > 0.0)
      geometricNormal = -geometricNormal;
    return true;
  }

  bool Renderer::makePixelQuery(int x, int y, RTCBeamQuery& query) const
  {
    if (x < 0 || y < 0 || x >= width_ || y >= height_)
      return false;

    const Vec3 d = rayDirection(x, y);
    double boxNear, boxFar;
    const bool boxHit = mediumBox.interval(camera.origin, d, boxNear, boxFar) && boxFar >= 0.0;

    double surfaceT = std::numeric_limits<double>::infinity();
    Vec3 normal;
    const bool surfaceHit = traceSurface(camera.origin, d, surfaceT, normal);

    double tnear = 0.0;
    double tfar = kRayFar;

    if (boxHit)
    {
      tnear = std::max(0.0, boxNear);
      tfar = boxFar;
    }

    if (surfaceHit)
      tfar = std::min(tfar, surfaceT);

    if (!boxHit)
    {
      /* A surface can still be visible outside the medium, but there are no
         beam supports there.  Keep the query valid and empty by using a zero
         interval only when a surface gives a finite endpoint. */
      if (!surfaceHit)
        return false;
      tnear = 0.0;
      tfar = std::max(0.0, surfaceT);
      if (!(tnear < tfar))
        return false;
    }

    if (!(tnear <= tfar))
      return false;

    query = RTCBeamQuery{};
    query.origin[0] = camera.origin.x;
    query.origin[1] = camera.origin.y;
    query.origin[2] = camera.origin.z;
    query.direction[0] = d.x;
    query.direction[1] = d.y;
    query.direction[2] = d.z;
    query.tnear = tnear;
    query.tfar = tfar;
    query.sigma_t = sigmaT;
    query.sigma_s = sigmaS;
    return true;
  }

  bool Renderer::step(std::size_t photonCount)
  {
    if (photonCount == 0)
      return false;

    releaseMap();

    const std::size_t round = completedRounds;
    currentRadius = settings.radius *
      std::pow(double(round + 1), -1.0 / 6.0);

    currentBeams.clear();
    currentSegments.clear();
    photons.clear();
    photons.resize(photonCount);
    lastQueryStats = {};

    const double albedo = sigmaT > 0.0 ? sigmaS / sigmaT : 0.0;

    for (std::size_t pi = 0; pi < photonCount; ++pi)
    {
      const std::uint64_t identity = identityFor(round, pi);
      DeterministicRng rng(identity);

      PhotonDiagnostic& diag = photons[pi];
      diag.index = pi;
      diag.identity = identity;
      diag.initialFlux = lightPower;

      Vec3 p = lightPosition;
      Vec3 d = uniformSphere(rng);
      diag.initialDirection = d;

      Vec3 flux = lightPower;
      int eventCount = 0;

      PhotonEvent emission{};
      emission.type = EVENT_EMISSION;
      emission.eventIndex = 0;
      emission.position = p;
      emission.directionBefore = d;
      emission.directionAfter = d;
      emission.fluxBefore = flux;
      emission.fluxAfter = flux;
      diag.events.push_back(emission);

      /* At most maxEvents volume/mirror events.  Box exits do not increment
         this counter and a straight path cannot re-enter a convex box. */
      for (int iteration = 0; iteration <= 2 * settings.maxEvents + 4; ++iteration)
      {
        double surfaceT = std::numeric_limits<double>::infinity();
        Vec3 surfaceNormal{0.0, 1.0, 0.0};
        const bool surfaceHit = traceSurface(p, d, surfaceT, surfaceNormal);

        double boxNear = 0.0, boxFar = -1.0;
        const bool boxHit = mediumBox.interval(p, d, boxNear, boxFar) && boxFar >= 0.0;
        const double boxEntry = boxHit ? std::max(0.0, boxNear)
                                       : std::numeric_limits<double>::infinity();
        const double boxExit = boxHit ? boxFar
                                     : std::numeric_limits<double>::infinity();

        /* Surface reached through vacuum before reaching the medium box. */
        if (surfaceHit && (!boxHit || surfaceT < boxEntry))
        {
          const Vec3 hitPoint = p + d * surfaceT;
          PhotonEvent mirror{};
          mirror.type = EVENT_MIRROR;
          mirror.eventIndex = eventCount;
          mirror.position = hitPoint;
          mirror.directionBefore = d;
          mirror.fluxBefore = flux;
          flux = {flux.x * mirrorReflectance.x,
                  flux.y * mirrorReflectance.y,
                  flux.z * mirrorReflectance.z};
          mirror.fluxAfter = flux;
          mirror.directionAfter = normalize(
            d - surfaceNormal * (2.0 * dot(d, surfaceNormal)));
          diag.events.push_back(mirror);

          ++eventCount;
          if (eventCount >= settings.maxEvents)
            break;

          p = hitPoint;
          d = mirror.directionAfter;
          continue;
        }

        if (!boxHit)
          break;

        double segmentStart = boxEntry;
        double segmentEnd = boxExit;
        int endType = END_BOX_EXIT;

        if (surfaceHit && surfaceT <= boxExit)
        {
          segmentEnd = std::min(segmentEnd, surfaceT);
          endType = END_MIRROR;
        }

        const Vec3 segmentFrom = p + d * segmentStart;
        Vec3 segmentTo = p + d * segmentEnd;
        const bool nonZero = segmentEnd > segmentStart;

        double randomValue = 0.0;
        double freeDistance = std::numeric_limits<double>::infinity();
        bool volumeCollision = false;
        double collisionParameter = std::numeric_limits<double>::infinity();

        if (sigmaT > 0.0)
        {
          randomValue = rng.uniform();
          freeDistance = -std::log(1.0 - randomValue) / sigmaT;
          collisionParameter = segmentStart + freeDistance;
          if (collisionParameter < segmentEnd)
          {
            volumeCollision = true;
            segmentEnd = collisionParameter;
            endType = END_VOLUME;
            segmentTo = p + d * segmentEnd;
          }
        }

        const bool stored = sigmaT > 0.0 && nonZero && segmentEnd > segmentStart;
        std::size_t segmentIndex = static_cast<std::size_t>(-1);
        if (nonZero)
        {
          SegmentDiagnostic segment{};
          segment.beam = makeBeam(segmentFrom, segmentTo, currentRadius, flux);
          segment.photon = pi;
          segment.segment = currentSegments.size();
          segment.endType = endType;
          segment.stored = stored;
          segmentIndex = currentSegments.size();
          currentSegments.push_back(segment);
          diag.segmentIndices.push_back(segmentIndex);

          if (stored)
          {
            currentBeams.push_back(segment.beam);
            diag.producedStoredBeam = true;
          }
        }

        if (volumeCollision)
        {
          const Vec3 beforeDirection = d;
          const Vec3 scattered = uniformSphere(rng);

          PhotonEvent event{};
          event.type = EVENT_VOLUME_SCATTER;
          event.eventIndex = eventCount;
          event.position = segmentTo;
          event.directionBefore = beforeDirection;
          event.directionAfter = scattered;
          event.fluxBefore = flux;
          flux = flux * albedo;
          event.fluxAfter = flux;
          event.randomValue = randomValue;
          event.sampledDistance = freeDistance;
          event.segmentIndex = segmentIndex;
          diag.events.push_back(event);

          ++eventCount;
          if (eventCount >= settings.maxEvents)
            break;

          p = segmentTo;
          d = scattered;
          continue;
        }

        if (endType == END_MIRROR && surfaceHit)
        {
          /* segmentTo was derived before segmentEnd mutation only in the
             volume branch, so here it is the surface hit point. */
          PhotonEvent event{};
          event.type = EVENT_MIRROR;
          event.eventIndex = eventCount;
          event.position = segmentTo;
          event.directionBefore = d;
          event.fluxBefore = flux;
          flux = {flux.x * mirrorReflectance.x,
                  flux.y * mirrorReflectance.y,
                  flux.z * mirrorReflectance.z};
          event.fluxAfter = flux;
          event.directionAfter = normalize(
            d - surfaceNormal * (2.0 * dot(d, surfaceNormal)));
          event.randomValue = randomValue;
          event.sampledDistance = freeDistance;
          event.segmentIndex = segmentIndex;
          diag.events.push_back(event);

          ++eventCount;
          if (eventCount >= settings.maxEvents)
            break;

          p = segmentTo;
          d = event.directionAfter;
          continue;
        }

        /* Box exit: not an event and does not consume an event slot.  Offset
           only the simulation origin; the diagnostic beam ends exactly on the
           mathematical boundary. */
        PhotonEvent exitEvent{};
        exitEvent.type = EVENT_BOX_EXIT;
        exitEvent.eventIndex = eventCount;
        exitEvent.position = segmentTo;
        exitEvent.directionBefore = d;
        exitEvent.directionAfter = d;
        exitEvent.fluxBefore = flux;
        exitEvent.fluxAfter = flux;
        exitEvent.randomValue = randomValue;
        exitEvent.sampledDistance = freeDistance;
        exitEvent.segmentIndex = segmentIndex;
        diag.events.push_back(exitEvent);

        p = segmentTo + d * kBoundaryEps;
      }
    }

    map_ = rtcNewPhotonBeamMap(deviceHandle,
                               currentBeams.empty() ? nullptr : currentBeams.data(),
                               currentBeams.size());
    if (!map_ && !currentBeams.empty())
      return false;

    RTCBeamStats totalStats{};
    for (int y = 0; y < height_; ++y)
    {
      for (int x = 0; x < width_; ++x)
      {
        RTCBeamQuery query{};
        if (!makePixelQuery(x, y, query))
          continue;

        double rgb[3] = {0.0, 0.0, 0.0};
        RTCBeamStats stats{};
        if (!rtcIntegratePhotonBeams(map_, &query, rgb, &stats))
          continue;

        filmSum[filmIndex(x, y)] += Vec3(rgb[0], rgb[1], rgb[2]);
        totalStats = addStats(totalStats, stats);
      }
    }

    lastQueryStats = totalStats;
    emittedPhotons += photonCount;
    lastRoundPhotons = photonCount;
    ++completedRounds;
    return true;
  }

  Vec3 Renderer::pixel(int x, int y) const
  {
    if (x < 0 || y < 0 || x >= width_ || y >= height_ || emittedPhotons == 0)
      return {0.0, 0.0, 0.0};
    return filmSum[filmIndex(x, y)] / double(emittedPhotons);
  }
}
