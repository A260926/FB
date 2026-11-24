// Copyright 2025 Photon beam task.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <embree4/rtcore.h>
#include <embree4/rtcore_photon_beam.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace photon_beams
{
  /*! Simple double-precision 3-vector used by the public renderer diagnostics. */
  struct Vec3
  {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    Vec3() = default;
    Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3 operator*(double a) const { return {x * a, y * a, z * a}; }
    Vec3 operator/(double a) const { return {x / a, y / a, z / a}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(double a) { x *= a; y *= a; z *= a; return *this; }
  };

  inline Vec3 operator*(double a, const Vec3& v) { return v * a; }
  inline double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
  inline Vec3 cross(const Vec3& a, const Vec3& b)
  {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
  }
  inline double length(const Vec3& v) { return std::sqrt(dot(v, v)); }
  Vec3 normalize(const Vec3& v);

  struct Box
  {
    Vec3 lower;
    Vec3 upper;

    bool contains(const Vec3& p) const;
    /*! Raw slab interval; t0 may be negative when the origin is inside. */
    bool interval(const Vec3& origin, const Vec3& direction,
                  double& t0, double& t1) const;
  };

  struct PerspectiveCamera
  {
    Vec3 origin{0.0, 0.0, 5.0};
    Vec3 target{0.0, 0.0, 0.0};
    Vec3 up{0.0, 1.0, 0.0};
    double fovYDegrees = 45.0;
  };

  struct Settings
  {
    std::uint64_t seed = 1;
    int maxEvents = 8;
    double radius = 0.05;
  };

  enum EventType
  {
    EVENT_EMISSION = 0,
    EVENT_VOLUME_SCATTER = 1,
    EVENT_MIRROR = 2,
    EVENT_BOX_EXIT = 3,
    EVENT_TERMINATED = 4
  };

  enum SegmentEnd
  {
    END_VOLUME = EVENT_VOLUME_SCATTER,
    END_MIRROR = EVENT_MIRROR,
    END_BOX_EXIT = EVENT_BOX_EXIT
  };

  /*! One in-box path segment.  stored is false for a vacuum (sigma_t == 0)
      candidate, which remains diagnostic-only and is not put in the native map. */
  struct SegmentDiagnostic
  {
    RTCPhotonBeam beam{};
    std::size_t photon = 0;
    std::size_t segment = 0;
    int endType = END_BOX_EXIT;
    bool stored = false;
  };

  struct PhotonEvent
  {
    int type = EVENT_EMISSION;
    int eventIndex = 0;
    Vec3 position;
    Vec3 directionBefore;
    Vec3 directionAfter;
    Vec3 fluxBefore;
    Vec3 fluxAfter;
    double randomValue = 0.0;
    double sampledDistance = 0.0;
    std::size_t segmentIndex = static_cast<std::size_t>(-1);
  };

  struct PhotonDiagnostic
  {
    std::size_t index = 0;
    std::uint64_t identity = 0;
    Vec3 initialDirection;
    Vec3 initialFlux;
    bool producedStoredBeam = false;
    std::vector<std::size_t> segmentIndices;
    std::vector<PhotonEvent> events;
  };

  /*! Windowless progressive short-photon-beam renderer.

      The renderer retains the device/scene handles it is given.  The input
      RTCScene must be committed and geometrically frozen while steps run. */
  class Renderer
  {
  public:
    Renderer(RTCDevice device,
             RTCScene scene,
             const Box& mediumBox,
             double sigmaT,
             double sigmaS,
             const Vec3& lightPosition,
             const Vec3& lightPower,
             const PerspectiveCamera& camera,
             int imageWidth,
             int imageHeight,
             const Settings& settings);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /*! Runs one deterministic round of photonCount emissions.
        Zero is rejected and leaves the film unchanged. */
    bool step(std::size_t photonCount);

    /*! Final linear RGB pixel normalized by cumulative emissions. */
    Vec3 pixel(int x, int y) const;

    int width() const { return width_; }
    int height() const { return height_; }
    std::size_t filmIndex(int x, int y) const { return static_cast<std::size_t>(y) * width_ + x; }

    /*! Builds the same finite camera query used by step().  Returns false for
        pixels whose ray does not reach the medium box. */
    bool makePixelQuery(int x, int y, RTCBeamQuery& query) const;

    RTCPhotonBeamMap currentMap() const { return map_; }
    std::uint64_t identityFor(std::size_t round, std::size_t photonIndex) const;

    /* Constructor inputs and tunables, public for independent inspection. */
    RTCDevice deviceHandle = nullptr;
    RTCScene sceneHandle = nullptr;
    Box mediumBox;
    double sigmaT = 0.0;
    double sigmaS = 0.0;
    Vec3 lightPosition;
    Vec3 lightPower;
    PerspectiveCamera camera;
    int width_ = 0;
    int height_ = 0;
    Settings settings;
    Vec3 mirrorReflectance{1.0, 1.0, 1.0};

    /* Progressive state. */
    std::size_t completedRounds = 0;
    std::size_t emittedPhotons = 0;
    std::size_t lastRoundPhotons = 0;
    double currentRadius = 0.0;

    /*! Unnormalized sum of every round's native query results. */
    std::vector<Vec3> filmSum;

    /* Current-round production data and diagnostics. */
    std::vector<RTCPhotonBeam> currentBeams;
    std::vector<SegmentDiagnostic> currentSegments;
    std::vector<PhotonDiagnostic> photons;
    RTCBeamStats lastQueryStats{};

  private:
    RTCPhotonBeamMap map_ = nullptr;

    void releaseMap();
    bool traceSurface(const Vec3& origin, const Vec3& direction,
                       double& distance, Vec3& geometricNormal) const;
    Vec3 rayDirection(int x, int y) const;
  };
}
