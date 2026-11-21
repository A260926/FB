// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../../include/embree4/rtcore_photon_beam.h"
#include "../common/device.h"
#include "../common/alloc.h"
#include "bvh.h"

#include <atomic>
#include <cmath>
#include <vector>

namespace embree
{
  /* Plain double-precision vector math. The repo's Vec3<double> dot/cross
   * helpers funnel into float-only madd/msub intrinsics, so all beam
   * geometry uses these explicit scalar operations instead. */
  static inline double beamDot(const Vec3<double>& a, const Vec3<double>& b)
  {
    return a.x*b.x + a.y*b.y + a.z*b.z;
  }
  static inline Vec3<double> beamCross(const Vec3<double>& a, const Vec3<double>& b)
  {
    return Vec3<double>(a.y*b.z - a.z*b.y,
                        a.z*b.x - a.x*b.z,
                        a.x*b.y - a.y*b.x);
  }

  /*! Internal representation of an immutable photon beam map.
   *
   *  The beams themselves are stored as an exact deep copy of the user
   *  data (original order and identity preserved). A BVH4 of flat
   *  cylinder bounding boxes is built on top, using the same AABB node
   *  layout and FastAllocator machinery as the regular ray tracing
   *  BVH4/SAH acceleration structures. */
  class PhotonBeamMap
  {
  public:

    /*! Geometry of a single beam, precomputed in double precision. */
    struct Beam
    {
      Vec3<double> from;
      Vec3<double> to;
      Vec3<double> w;     // unit axis direction
      double length;      // cylinder length, strictly positive
      double radius;      // cylinder radius, strictly positive
      Vec3<double> flux;  // spectral power
    };

    /*! Result of intersecting an eye ray with one beam support. */
    struct Interval
    {
      bool valid;         // non-empty interval of positive length
      double t0, t1;      // ray parameter range inside the support
    };

    PhotonBeamMap(Device* device);
    ~PhotonBeamMap();

    PhotonBeamMap(const PhotonBeamMap&) = delete;
    PhotonBeamMap& operator=(const PhotonBeamMap&) = delete;

    /*! Validates and deep-copies the beams, then builds the BVH4.
     *  Throws rtcore_error(INVALID_ARGUMENT) on illegal input. */
    void build(const RTCPhotonBeam* beams, size_t count);

    /*! Analytic integration of all beams overlapping the query. */
    bool integrate(const RTCBeamQuery& query, double rgb[3], RTCBeamStats* stats) const;

    /*! Exact support/ray interval intersection, shared with the tests'
     *  independent reference enumeration. */
    static Interval intersectBeam(const Beam& beam,
                                  const Vec3<double>& origin,
                                  const Vec3<double>& direction,
                                  double tnear, double tfar);

    static bool validateBeam(const RTCPhotonBeam& beam);

    void refInc() { refCounter.fetch_add(1); }
    void refDec() { if (refCounter.fetch_sub(1) == 1) delete this; }

    size_t size() const { return beams.size(); }
    Device* getDevice() const { return device; }

  private:

    typedef BVHN<4>::NodeRef NodeRef;

    /*! SAH-style recursive build over the index permutation. */
    NodeRef buildRecursive(size_t begin, size_t end, size_t depth);

    Device* device;
    std::atomic<size_t> refCounter;
    FastAllocator alloc;

    std::vector<Beam> beams;          // exact copy, original identity
    std::vector<size_t> indices;      // permutation used by the BVH
    std::vector<BBox3fa> leafBounds;  // flat cylinder bounds per beam
    NodeRef root;
  };
}
