// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace photon_beams
{
  namespace
  {
    // ------------------------------------------------------------------
    // Small double-precision vector helpers (standard library only).
    // ------------------------------------------------------------------

    struct V3
    {
      double x, y, z;

      V3() : x(0), y(0), z(0) {}
      V3(double a, double b, double c) : x(a), y(b), z(c) {}
      explicit V3(const double v[3]) : x(v[0]), y(v[1]), z(v[2]) {}

      V3 operator+(const V3& o) const { return V3(x+o.x, y+o.y, z+o.z); }
      V3 operator-(const V3& o) const { return V3(x-o.x, y-o.y, z-o.z); }
      V3 operator*(double s) const { return V3(x*s, y*s, z*s); }
      V3 operator/(double s) const { return V3(x/s, y/s, z/s); }
      V3& operator+=(const V3& o) { x+=o.x; y+=o.y; z+=o.z; return *this; }
    };

    double dot(const V3& a, const V3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
    V3 cross(const V3& a, const V3& b)
    {
      return V3(a.y*b.z - a.z*b.y,
                a.z*b.x - a.x*b.z,
                a.x*b.y - a.y*b.x);
    }
    double length(const V3& a) { return std::sqrt(dot(a,a)); }
    V3 normalize(const V3& a) { return a * (1.0 / length(a)); }

    // ------------------------------------------------------------------
    // Deterministic random stream. The state is derived solely from
    // (seed, round index, photon index), so two renderers executing the
    // same photons produce identical draws regardless of interleaving.
    // ------------------------------------------------------------------

    class PhotonRng
    {
    public:

      PhotonRng(uint64_t seed, uint64_t round, uint64_t photon)
      {
        state = mix(seed, 0x9E3779B97F4A7C15ull);
        state = mix(state + round, 0xC2B2AE3D27D4EB4Full);
        state = mix(state + photon, 0x165667B19E3779F9ull);
      }

      uint64_t nextU64()
      {
        // splitmix64
        state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
      }

      double nextUnit()
      {
        // 53-bit uniform value in [0,1)
        return double(nextU64() >> 11) * (1.0 / 9007199254740992.0);
      }

      V3 uniformSphere()
      {
        const double u1 = nextUnit();
        const double u2 = nextUnit();
        const double z = 1.0 - 2.0 * u1;
        const double r = std::sqrt(std::max(0.0, 1.0 - z*z));
        const double phi = 2.0 * 3.14159265358979323846 * u2;
        return V3(r * std::cos(phi), r * std::sin(phi), z);
      }

    private:

      static uint64_t mix(uint64_t z, uint64_t salt)
      {
        z += salt;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
      }

      uint64_t state;
    };

    // Ray/medium-box intersection. Returns false when the ray never
    // enters the box. tEnter/tExit are the ray parameters of the two
    // slab crossings.
    bool rayBox(const V3& origin, const V3& dir,
                const MediumBox& box, double& tEnter, double& tExit)
    {
      const V3 lo(box.lower), hi(box.upper);
      double t0 = -std::numeric_limits<double>::infinity();
      double t1 =  std::numeric_limits<double>::infinity();

      for (int axis = 0; axis < 3; ++axis)
      {
        const double o = (&origin.x)[axis];
        const double d = (&dir.x)[axis];
        const double b0 = (&lo.x)[axis];
        const double b1 = (&hi.x)[axis];
        if (d == 0.0)
        {
          if (o < b0 || o > b1) return false;
        }
        else
        {
          double ta = (b0 - o) / d;
          double tb = (b1 - o) / d;
          if (ta > tb) std::swap(ta, tb);
          t0 = std::max(t0, ta);
          t1 = std::min(t1, tb);
          if (t1 < t0) return false;
        }
      }
      tEnter = t0;
      tExit = t1;
      return true;
    }

    bool insideBox(const V3& p, const MediumBox& box)
    {
      return p.x >= box.lower[0] && p.x <= box.upper[0] &&
             p.y >= box.lower[1] && p.y <= box.upper[1] &&
             p.z >= box.lower[2] && p.z <= box.upper[2];
    }
  }

  // ----------------------------------------------------------------------
  // Per-photon simulation state.
  // ----------------------------------------------------------------------

  struct Renderer::PhotonState
  {
    V3 position;
    V3 direction;
    V3 flux;
    unsigned events = 0;     // volume + surface events consumed
    bool inside = true;      // currently inside the medium box
    V3 segmentStart;         // entry point of the current in-box segment
  };

  Renderer::Renderer(RTCDevice hdevice,
                     RTCScene hscene,
                     const MediumBox& box_,
                     const PointLight& light_,
                     const PerspectiveCamera& camera_,
                     const MediumModel& medium_,
                     const SurfaceModel& surface_,
                     size_t width,
                     size_t height,
                     const RenderParams& params_)
    : device(hdevice), scene(hscene), box(box_), light(light_),
      camera(camera_), medium(medium_), surface(surface_),
      imageWidth(width), imageHeight(height), params(params_)
  {
    if (device == nullptr || scene == nullptr)
      throw std::invalid_argument("photon_beams::Renderer: null device or scene");
    if (width == 0 || height == 0)
      throw std::invalid_argument("photon_beams::Renderer: empty image");
    if (params.maxEvents < 1 || params.maxEvents > 32)
      throw std::invalid_argument("photon_beams::Renderer: maxEvents must be in [1,32]");
    if (!(params.radius > 0.0) || !std::isfinite(params.radius))
      throw std::invalid_argument("photon_beams::Renderer: radius must be positive");
    if (!(medium.sigma_t >= 0.0) || medium.sigma_s < 0.0 ||
        medium.sigma_s > medium.sigma_t)
      throw std::invalid_argument("photon_beams::Renderer: invalid medium coefficients");
    if (!std::isfinite(camera.fovY) || camera.fovY <= 0.0 ||
        camera.fovY >= 3.14159265358979323846)
      throw std::invalid_argument("photon_beams::Renderer: invalid camera fov");

    rtcRetainDevice(device);
    rtcRetainScene(scene);

    const V3 fwd = normalize(V3(camera.direction));
    const V3 right = normalize(cross(fwd, V3(camera.up)));
    const V3 up = cross(right, fwd);
    const V3 org(camera.origin);

    for (int i = 0; i < 3; ++i)
    {
      camOrigin[i] = (&org.x)[i];
      camRight[i] = (&right.x)[i];
      camUp[i] = (&up.x)[i];
      camForward[i] = (&fwd.x)[i];
    }
    const double aspect = double(imageWidth) / double(imageHeight);
    const double tanHalfY = std::tan(0.5 * camera.fovY);
    pixelScaleX = tanHalfY * aspect;
    pixelScaleY = tanHalfY;

    filmAccum.assign(3 * imageWidth * imageHeight, 0.0);
    filmRound.assign(3 * imageWidth * imageHeight, 0.0);
  }

  Renderer::Renderer(Renderer&& o) noexcept
    : device(o.device), scene(o.scene), box(o.box), light(o.light),
      camera(o.camera), medium(o.medium), surface(o.surface),
      imageWidth(o.imageWidth), imageHeight(o.imageHeight), params(o.params),
      map(o.map), roundCount(o.roundCount), emissionsTotal(o.emissionsTotal),
      activeRadius(o.activeRadius),
      filmAccum(std::move(o.filmAccum)), filmRound(std::move(o.filmRound)),
      roundDiag(std::move(o.roundDiag))
  {
    for (int i = 0; i < 3; ++i)
    {
      camOrigin[i] = o.camOrigin[i];
      camRight[i] = o.camRight[i];
      camUp[i] = o.camUp[i];
      camForward[i] = o.camForward[i];
    }
    pixelScaleX = o.pixelScaleX;
    pixelScaleY = o.pixelScaleY;
    o.map = nullptr; // moved-from renderer releases nothing
  }

  Renderer::~Renderer()
  {
    if (map) rtcReleasePhotonBeamMap(map);
    rtcReleaseScene(scene);
    rtcReleaseDevice(device);
  }

  double Renderer::currentRadius() const
  {
    // Before the first step this reports the round-zero radius; after a
    // step it is exactly the radius carried by that round's beams.
    if (activeRadius != 0.0) return activeRadius;
    return params.radius * std::pow(double(roundCount + 1), -1.0 / 6.0);
  }

  void Renderer::resetRoundCounters(size_t photons)
  {
    roundDiag = RoundDiagnostics();
    roundDiag.round = roundCount;
    roundDiag.emissions = photons;
    std::fill(filmRound.begin(), filmRound.end(), 0.0);
  }

  void Renderer::storeBeam(const PhotonState& state, const double endpoint[3])
  {
    const V3 end(endpoint);
    const V3 start = state.segmentStart;
    const double segLen = length(end - start);
    if (!(segLen > 0.0))
      return; // only non-zero in-medium segments enter the pool
    if (medium.sigma_t == 0.0)
      return; // vacuum segments never enter the pool

    // The sampled free path already carries the photon's survival
    // probability along this segment (a beam only covers points up to
    // the drawn collision distance), so neither storage nor lookup
    // multiplies the photon-side transmittance. Only the eye path keeps
    // its exponential term.
    BeamDiagnostic diag;
    diag.photonIndex = 0; // stamped by tracePhoton over its beam range
    diag.eventIndex = state.events; // segments are indexed by events spent
    diag.beam.radius = currentRadius();
    diag.beam.from[0] = start.x; diag.beam.from[1] = start.y; diag.beam.from[2] = start.z;
    diag.beam.to[0] = end.x;   diag.beam.to[1] = end.y;   diag.beam.to[2] = end.z;
    diag.beam.flux[0] = state.flux.x;
    diag.beam.flux[1] = state.flux.y;
    diag.beam.flux[2] = state.flux.z;
    roundDiag.beams.push_back(diag);
  }

  void Renderer::tracePhoton(uint64_t photonIndex)
  {
    PhotonRng rng(params.seed, roundCount, photonIndex);
    const size_t beamBegin = roundDiag.beams.size();

    PhotonState state;
    state.position = V3(light.position);
    state.direction = rng.uniformSphere();
    state.flux = V3(light.power);
    state.inside = insideBox(state.position, box);
    state.segmentStart = state.position;

    EventRecord launch;
    launch.photonIndex = photonIndex;
    launch.eventIndex = 0;
    launch.kind = state.inside ? PHOTON_EVENT_BOX_ENTER : PHOTON_EVENT_TERMINATE;
    for (int i = 0; i < 3; ++i)
    {
      launch.position[i] = (&state.position.x)[i];
      launch.direction[i] = (&state.direction.x)[i];
      launch.flux[i] = (&state.flux.x)[i];
    }
    roundDiag.events.push_back(launch);

    // Hard iteration cap: box crossings alone cannot turn a ray, and
    // every turn consumes a surface event, so real paths terminate well
    // before this; it only guards against pathological geometry.
    for (unsigned iteration = 0; iteration < 1024; ++iteration)
    {
      // Nearest native triangle along the current direction.
      RTCRayHit rayhit;
      rayhit.ray.org_x = float(state.position.x);
      rayhit.ray.org_y = float(state.position.y);
      rayhit.ray.org_z = float(state.position.z);
      rayhit.ray.dir_x = float(state.direction.x);
      rayhit.ray.dir_y = float(state.direction.y);
      rayhit.ray.dir_z = float(state.direction.z);
      rayhit.ray.tnear = 0.0f;
      rayhit.ray.tfar = std::numeric_limits<float>::infinity();
      rayhit.ray.mask = ~0u;
      rayhit.ray.flags = 0;
      rayhit.ray.time = 0.0f;
      rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
      rayhit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
      rtcIntersect1(scene, &rayhit);

      const bool hasSurface = rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID;
      const double tSurface = hasSurface ? double(rayhit.ray.tfar)
                                         : std::numeric_limits<double>::infinity();

      // Box crossings of the current straight segment.
      double tEnter, tExit;
      const bool hitsBox = rayBox(state.position, state.direction, box, tEnter, tExit);
      const double tBoxAhead = hitsBox && tExit > 0.0
        ? (state.inside ? tExit : (tEnter >= 0.0 ? tEnter
                                                 : std::numeric_limits<double>::infinity()))
        : std::numeric_limits<double>::infinity();

      if (state.inside)
      {
        // Sample the true free path to the next volume collision.
        double tVolume = std::numeric_limits<double>::infinity();
        if (medium.sigma_t > 0.0)
        {
          double u = rng.nextUnit();
          if (u <= 0.0) u = std::numeric_limits<double>::min();
          tVolume = -std::log(1.0 - u) / medium.sigma_t;
        }

        const double tEnd = std::min(tVolume, std::min(tSurface, tBoxAhead));
        const V3 endpoint = state.position + state.direction * tEnd;
        double endArr[3] = { endpoint.x, endpoint.y, endpoint.z };

        EventRecord ev;
        ev.photonIndex = photonIndex;
        ev.eventIndex = state.events;
        for (int i = 0; i < 3; ++i)
        {
          ev.position[i] = (&endpoint.x)[i];
          ev.direction[i] = (&state.direction.x)[i];
          ev.flux[i] = (&state.flux.x)[i];
        }

        if (tEnd == tVolume)
        {
          // Volume collision wins: store the beam, apply the albedo.
          storeBeam(state, endArr);
          const double albedo = medium.sigma_t > 0.0 ? medium.sigma_s / medium.sigma_t : 0.0;
          state.flux = state.flux * albedo;
          state.position = endpoint;
          state.direction = rng.uniformSphere();
          state.segmentStart = state.position;
          ev.kind = PHOTON_EVENT_VOLUME;
          ++state.events;
          ev.eventIndex = state.events;
          for (int i = 0; i < 3; ++i)
          {
            ev.direction[i] = (&state.direction.x)[i];
            ev.flux[i] = (&state.flux.x)[i];
          }
        }
        else if (tEnd == tSurface)
        {
          // Specular surface wins: store the beam, reflect.
          storeBeam(state, endArr);
          V3 normal(double(rayhit.hit.Ng_x),
                    double(rayhit.hit.Ng_y),
                    double(rayhit.hit.Ng_z));
          normal = normalize(normal);
          if (dot(normal, state.direction) > 0.0) normal = normal * -1.0;
          state.position = endpoint;
          state.direction = normalize(state.direction -
                                      normal * (2.0 * dot(state.direction, normal)));
          state.flux = V3(state.flux.x * surface.reflectance[0],
                          state.flux.y * surface.reflectance[1],
                          state.flux.z * surface.reflectance[2]);
          ev.kind = PHOTON_EVENT_SURFACE;
          ++state.events;
          ev.eventIndex = state.events;
          for (int i = 0; i < 3; ++i)
          {
            ev.direction[i] = (&state.direction.x)[i];
            ev.flux[i] = (&state.flux.x)[i];
          }
          state.inside = insideBox(state.position, box);
          if (state.inside) state.segmentStart = state.position;
        }
        else
        {
          // Box exit wins: store the beam, continue in vacuum. Crossing
          // the boundary does not consume an event.
          storeBeam(state, endArr);
          state.position = endpoint;
          state.inside = false;
          ev.kind = PHOTON_EVENT_BOX_EXIT;
        }

        roundDiag.events.push_back(ev);

        if (state.events >= params.maxEvents)
        {
          EventRecord term = ev;
          term.kind = PHOTON_EVENT_TERMINATE;
          roundDiag.events.push_back(term);
          break;
        }
      }
      else
      {
        // Vacuum segment: no beams are stored here.
        const double tNext = std::min(tSurface, tBoxAhead);

        if (!std::isfinite(tNext))
        {
          EventRecord term;
          term.photonIndex = photonIndex;
          term.eventIndex = state.events;
          term.kind = PHOTON_EVENT_TERMINATE;
          for (int i = 0; i < 3; ++i)
          {
            term.position[i] = (&state.position.x)[i];
            term.direction[i] = (&state.direction.x)[i];
            term.flux[i] = (&state.flux.x)[i];
          }
          roundDiag.events.push_back(term);
          break;
        }

        const V3 endpoint = state.position + state.direction * tNext;

        if (tNext == tBoxAhead && tBoxAhead < tSurface)
        {
          // Re-enter the box without spending an event.
          state.position = endpoint;
          state.inside = true;
          state.segmentStart = state.position;

          EventRecord ev;
          ev.photonIndex = photonIndex;
          ev.eventIndex = state.events;
          ev.kind = PHOTON_EVENT_BOX_ENTER;
          for (int i = 0; i < 3; ++i)
          {
            ev.position[i] = (&endpoint.x)[i];
            ev.direction[i] = (&state.direction.x)[i];
            ev.flux[i] = (&state.flux.x)[i];
          }
          roundDiag.events.push_back(ev);
        }
        else
        {
          // External mirror reflection; may send the photon back in.
          V3 normal(double(rayhit.hit.Ng_x),
                    double(rayhit.hit.Ng_y),
                    double(rayhit.hit.Ng_z));
          normal = normalize(normal);
          if (dot(normal, state.direction) > 0.0) normal = normal * -1.0;
          state.position = endpoint;
          state.direction = normalize(state.direction -
                                      normal * (2.0 * dot(state.direction, normal)));
          state.flux = V3(state.flux.x * surface.reflectance[0],
                          state.flux.y * surface.reflectance[1],
                          state.flux.z * surface.reflectance[2]);
          ++state.events;

          EventRecord ev;
          ev.photonIndex = photonIndex;
          ev.eventIndex = state.events;
          ev.kind = PHOTON_EVENT_SURFACE;
          for (int i = 0; i < 3; ++i)
          {
            ev.position[i] = (&endpoint.x)[i];
            ev.direction[i] = (&state.direction.x)[i];
            ev.flux[i] = (&state.flux.x)[i];
          }
          roundDiag.events.push_back(ev);

          state.inside = insideBox(state.position, box);
          if (state.inside) state.segmentStart = state.position;

          if (state.events >= params.maxEvents)
          {
            EventRecord term = ev;
            term.kind = PHOTON_EVENT_TERMINATE;
            roundDiag.events.push_back(term);
            break;
          }
        }
      }
    }

    // Stamp the producing photon onto every beam of this path.
    for (size_t i = beamBegin; i < roundDiag.beams.size(); ++i)
      roundDiag.beams[i].photonIndex = photonIndex;
  }

  void Renderer::renderPixels()
  {
    const V3 origin(camOrigin);
    const V3 right(camRight);
    const V3 up(camUp);
    const V3 forward(camForward);

    for (size_t y = 0; y < imageHeight; ++y)
    {
      for (size_t x = 0; x < imageWidth; ++x)
      {
        const double u = (2.0 * (double(x) + 0.5) / double(imageWidth) - 1.0) * pixelScaleX;
        const double v = (1.0 - 2.0 * (double(y) + 0.5) / double(imageHeight)) * pixelScaleY;
        const V3 dir = normalize(forward + right * u + up * v);

        // Clip the camera segment at the nearest surface.
        RTCRayHit rayhit;
        rayhit.ray.org_x = float(origin.x);
        rayhit.ray.org_y = float(origin.y);
        rayhit.ray.org_z = float(origin.z);
        rayhit.ray.dir_x = float(dir.x);
        rayhit.ray.dir_y = float(dir.y);
        rayhit.ray.dir_z = float(dir.z);
        rayhit.ray.tnear = 0.0f;
        rayhit.ray.tfar = std::numeric_limits<float>::infinity();
        rayhit.ray.mask = ~0u;
        rayhit.ray.flags = 0;
        rayhit.ray.time = 0.0f;
        rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
        rayhit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
        rtcIntersect1(scene, &rayhit);

        // The medium only fills the box: clip the integration interval
        // to its intersection with the box, then to the nearest surface.
        double boxEnter, boxExit;
        const bool hitsBox = rayBox(origin, dir, box, boxEnter, boxExit);
        double tNear = 0.0;
        double tFar = rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID
          ? double(rayhit.ray.tfar) : 1.0e30;
        if (hitsBox)
        {
          if (insideBox(origin, box))
            boxEnter = 0.0;
          tNear = std::max(tNear, boxEnter);
          tFar = std::min(tFar, boxExit);
        }
        else
        {
          tNear = 1.0; tFar = 0.0; // empty interval: no medium seen
        }

        RTCBeamQuery query;
        query.origin[0] = origin.x; query.origin[1] = origin.y; query.origin[2] = origin.z;
        query.direction[0] = dir.x; query.direction[1] = dir.y; query.direction[2] = dir.z;
        query.tnear = tNear;
        query.tfar = tFar;
        query.sigma_t = medium.sigma_t;
        query.sigma_s = medium.sigma_s;

        double rgb[3] = { 0.0, 0.0, 0.0 };
        RTCBeamStats stats;
        if (tFar > tNear &&
            rtcIntegratePhotonBeams(map, &query, rgb, &stats))
        {
          roundDiag.nodeVisits += stats.nodeVisits;
          roundDiag.beamTests += stats.beamTests;
          roundDiag.supportIntervals += stats.supportIntervals;
          const size_t idx = 3 * (y * imageWidth + x);
          filmRound[idx+0] = rgb[0];
          filmRound[idx+1] = rgb[1];
          filmRound[idx+2] = rgb[2];
        }
      }
    }
  }

  bool Renderer::step(size_t photons)
  {
    if (photons == 0)
      return false; // rejected without touching the existing film

    activeRadius = params.radius * std::pow(double(roundCount + 1), -1.0 / 6.0);
    resetRoundCounters(photons);

    for (uint64_t i = 0; i < photons; ++i)
      tracePhoton(i);

    roundDiag.beamCount = roundDiag.beams.size();

    std::vector<RTCPhotonBeam> descriptors(roundDiag.beams.size());
    for (size_t i = 0; i < descriptors.size(); ++i)
      descriptors[i] = roundDiag.beams[i].beam;

    if (map)
    {
      rtcReleasePhotonBeamMap(map);
      map = nullptr;
    }
    map = rtcNewPhotonBeamMap(device,
                              descriptors.empty() ? nullptr : descriptors.data(),
                              descriptors.size());
    if (map == nullptr)
      throw std::runtime_error("photon_beams::Renderer: failed to build photon beam map");

    renderPixels();

    for (size_t i = 0; i < filmAccum.size(); ++i)
      filmAccum[i] += filmRound[i];

    emissionsTotal += photons;
    ++roundCount;
    return true;
  }

  void Renderer::pixel(size_t x, size_t y, double rgb[3]) const
  {
    const size_t idx = 3 * (y * imageWidth + x);
    const double scale = emissionsTotal > 0 ? 1.0 / double(emissionsTotal) : 0.0;
    rgb[0] = filmAccum[idx+0] * scale;
    rgb[1] = filmAccum[idx+1] * scale;
    rgb[2] = filmAccum[idx+2] * scale;
  }

  void Renderer::roundPixel(size_t x, size_t y, double rgb[3]) const
  {
    const size_t idx = 3 * (y * imageWidth + x);
    rgb[0] = filmRound[idx+0];
    rgb[1] = filmRound[idx+1];
    rgb[2] = filmRound[idx+2];
  }
}
