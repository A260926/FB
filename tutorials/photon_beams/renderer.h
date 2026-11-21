// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <embree4/rtcore.h>
#include <embree4/rtcore_photon_beam.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace photon_beams
{
  /* Axis-aligned box that contains the participating medium. Outside of
   *  the box the world is vacuum. */
  struct MediumBox
  {
    double lower[3];
    double upper[3];
  };

  /* Isotropic point light with spectrally constant total power. */
  struct PointLight
  {
    double position[3];
    double power[3];
  };

  /* Scalar homogeneous medium inside the box. */
  struct MediumModel
  {
    double sigma_t; // extinction, non-negative
    double sigma_s; // scattering, 0 <= sigma_s <= sigma_t
  };

  /* All scene surfaces share this one ideal specular RGB reflectance. */
  struct SurfaceModel
  {
    double reflectance[3];
  };

  /* Pinhole camera looking along "direction"; "up" only needs to be
   *  non-parallel to the direction. fovY is the vertical field of view
   *  in radians. */
  struct PerspectiveCamera
  {
    double origin[3];
    double direction[3];
    double up[3];
    double fovY;
  };

  /* User-facing simulation knobs. The radius used in round r is
   *  radius * (r+1)^(-1/6). */
  struct RenderParams
  {
    uint64_t seed;
    unsigned maxEvents; // in [1,32], volume and mirror events share it
    double radius;      // strictly positive
  };

  /* What happened to one photon at one point of its path. Flux is the
   *  photon flux directly after the event (the multiplication with the
   *  albedo or the reflectance is already included). */
  enum PhotonEventKind
  {
    PHOTON_EVENT_VOLUME = 0,   // sigma_s/sigma_t applied, isotropic re-scatter
    PHOTON_EVENT_SURFACE = 1,  // reflectance applied, specular reflection
    PHOTON_EVENT_BOX_EXIT = 2, // boundary crossing, no event budget spent
    PHOTON_EVENT_BOX_ENTER = 3,
    PHOTON_EVENT_TERMINATE = 4 // escaped to infinity or event budget spent
  };

  struct EventRecord
  {
    uint64_t photonIndex;
    unsigned eventIndex; // 0-based count of volume+surface events
    unsigned kind;       // one of PhotonEventKind
    double position[3];
    double direction[3];
    double flux[3];
  };

  /* One stored beam together with the identity of its producing photon.
   *  The beam's flux already carries the photon's survival probability
   *  along its free path; no transmittance is applied again at lookup. */
  struct BeamDiagnostic
  {
    RTCPhotonBeam beam;
    uint64_t photonIndex;
    unsigned eventIndex;
  };

  /* Everything a single step() round produced. Filled before any pixel
   *  query runs, and never touched by the RNG afterwards. */
  struct RoundDiagnostics
  {
    size_t round = 0;       // zero-based round index
    size_t emissions = 0;   // photons launched this round
    size_t beamCount = 0;   // beams actually stored this round
    std::vector<BeamDiagnostic> beams;
    std::vector<EventRecord> events;

    // Native query counters accumulated over all pixel queries.
    uint64_t nodeVisits = 0;
    uint64_t beamTests = 0;
    uint64_t supportIntervals = 0;
  };

  /* Progressive windowless photon-beam renderer.
   *
   *  The scene geometry is required to be frozen for the lifetime of
   *  the renderer (committed once and never updated): the renderer only
   *  reads the RTCScene. Each step() launches a fixed number of photons,
   *  stores their short in-medium beams in a fresh RTCPhotonBeamMap (the
   *  previous round's map is released), and accumulates one analytic
   *  beam query per pixel. */
  class Renderer
  {
  public:

    Renderer(RTCDevice device,
             RTCScene scene,
             const MediumBox& box,
             const PointLight& light,
             const PerspectiveCamera& camera,
             const MediumModel& medium,
             const SurfaceModel& surface,
             size_t width,
             size_t height,
             const RenderParams& params);

    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    Renderer(Renderer&& other) noexcept;
    Renderer& operator=(Renderer&&) = delete;

    /* Launches "photons" emissions in a new round, rebuilds the beam map
     *  and refreshes every pixel. photons == 0 is rejected and leaves
     *  the existing film untouched. Returns false on rejection. */
    bool step(size_t photons);

    /* Normalized linear RGB of pixel (x,y): the accumulated raw query
     *  results divided by the total number of emissions ever launched,
     *  including emissions that produced no stored beam. */
    void pixel(size_t x, size_t y, double rgb[3]) const;

    /* Unnormalized query result accumulated for this pixel during the
     *  latest round only (zero before the first step). */
    void roundPixel(size_t x, size_t y, double rgb[3]) const;

    /* Raw, unnormalized film access for independent verification. */
    const std::vector<double>& film() const { return filmAccum; }
    const std::vector<double>& roundFilm() const { return filmRound; }

    const RoundDiagnostics& diagnostics() const { return roundDiag; }
    size_t rounds() const { return roundCount; }
    uint64_t totalEmissions() const { return emissionsTotal; }
    double currentRadius() const;

    RTCPhotonBeamMap beamMap() const { return map; }
    size_t width() const { return imageWidth; }
    size_t height() const { return imageHeight; }

  private:

    struct PhotonState;
    void tracePhoton(uint64_t photonIndex);
    void storeBeam(const PhotonState& state, const double endpoint[3]);
    void renderPixels();
    void resetRoundCounters(size_t photons);

    RTCDevice device;
    RTCScene scene;
    MediumBox box;
    PointLight light;
    PerspectiveCamera camera;
    MediumModel medium;
    SurfaceModel surface;
    size_t imageWidth, imageHeight;
    RenderParams params;

    RTCPhotonBeamMap map = nullptr;
    size_t roundCount = 0;
    uint64_t emissionsTotal = 0;
    double activeRadius = 0.0; // radius used by the current/last round

    std::vector<double> filmAccum;  // cumulative raw query sums
    std::vector<double> filmRound;  // latest round raw query sums
    RoundDiagnostics roundDiag;

    // Camera basis, derived once from the camera description.
    double camOrigin[3];
    double camRight[3];
    double camUp[3];
    double camForward[3];
    double pixelScaleX, pixelScaleY;
  };
}
