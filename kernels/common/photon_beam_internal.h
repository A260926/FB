// Copyright 2025 Photon beam task.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../../include/embree4/rtcore_photon_beam.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace embree
{
  class Device;

  /*! Validated, immutable double-precision representation of one beam. */
  struct PhotonBeamData
  {
    double from[3];
    double to[3];
    double w[3];      /*!< Unit beam-axis direction. */
    double length;    /*!< Beam length L. */
    double radius;
    double flux[3];
    double bmin[3];   /*!< Conservative axis-aligned support bounds. */
    double bmax[3];
  };

  /*! Spatial index interface. The production implementation is BVH4/SAH. */
  class PhotonBeamIndex
  {
  public:
    virtual ~PhotonBeamIndex() = default;

    /*! Adds every beam contribution and every diagnostic event for this query. */
    virtual void integrate(const RTCBeamQuery& query,
                           long double rgb[3],
                           RTCBeamStats& stats) const = 0;
  };

  PhotonBeamIndex* createPhotonBeamBVH(Device* device,
                                       const std::vector<PhotonBeamData>& beams);

  /*! Finds the positive-length parameter interval shared by beam support,
      query interval, and [tnear,tfar]. Returns false for empty/tangent sets. */
  bool photonBeamSupportInterval(const PhotonBeamData& beam,
                                 const RTCBeamQuery& query,
                                 long double& t0,
                                 long double& t1);

  /*! Analytically integrates one beam over its current support interval.
      supportIntervalCount is either zero or one for a convex finite cylinder. */
  void integratePhotonBeam(const PhotonBeamData& beam,
                           const RTCBeamQuery& query,
                           long double rgb[3],
                           uint64_t& supportIntervalCount);

  /*! Validates a public beam descriptor. */
  bool photonBeamDescriptorValid(const RTCPhotonBeam& beam);

  /*! Validates a query descriptor. */
  bool photonBeamQueryValid(const RTCBeamQuery& query);
}

/*! Concrete opaque map visible to the two native implementation files. */
struct RTCPhotonBeamMapTy
{
  std::atomic<size_t> refCount{1};
  RTCDevice device = nullptr;
  embree::Device* embreeDevice = nullptr;
  std::vector<embree::PhotonBeamData> beams;
  std::unique_ptr<embree::PhotonBeamIndex> index;
};
