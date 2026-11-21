// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "bvh_photon_beam.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <vector>

namespace embree
{
  typedef Vec3<double> Vec3d;
  typedef BVHN<4>::NodeRef BeamNodeRef;
  typedef AABBNode_t<BeamNodeRef, 4> BeamAABBNode;

  /*! Maximum number of beams stored in one leaf, limited by the NodeRef
   *  leaf encoding (items mask). */
  static const size_t MAX_LEAF_BEAMS = BeamNodeRef::maxLeafBlocks;
  static const size_t TARGET_LEAF_BEAMS = 4;

  struct BeamLeaf
  {
    size_t count;
    size_t beamID[1]; // flexible array, allocated with extra storage
  };

  static inline float safeArea(const BBox3fa& box)
  {
    if (box.empty()) return 0.0f;
    const Vec3fa d = box.upper - box.lower;
    return 2.0f * (d.x*d.y + d.y*d.z + d.z*d.x);
  }

  static inline void* beamAlloc(FastAllocator& allocator, size_t bytes, size_t align)
  {
    size_t request = bytes;
    return allocator.malloc(request, align, false);
  }

  BeamNodeRef PhotonBeamMap::buildRecursive(size_t begin, size_t end, size_t depth)
  {
    const size_t n = end - begin;
    BBox3fa bounds = BBox3fa(EmptyTy());
    BBox3fa centroidBounds = BBox3fa(EmptyTy());
    for (size_t i = begin; i < end; ++i)
    {
      const BBox3fa& b = leafBounds[indices[i]];
      bounds.extend(b);
      centroidBounds.extend(Vec3fa(0.5f*(b.lower.x+b.upper.x),
                                   0.5f*(b.lower.y+b.upper.y),
                                   0.5f*(b.lower.z+b.upper.z)));
    }

    /* Small enough ranges become leaves. At the depth limit a leaf is
     * only legal if the count fits the leaf encoding; otherwise a split
     * is forced below. */
    const bool depthLimit = depth >= BVHN<4>::maxBuildDepth;
    if ((n <= TARGET_LEAF_BEAMS ||
         (depthLimit && n <= MAX_LEAF_BEAMS)) && n <= MAX_LEAF_BEAMS)
    {
      const size_t bytes = sizeof(BeamLeaf) + (n-1) * sizeof(size_t);
      BeamLeaf* leaf = (BeamLeaf*) beamAlloc(alloc, bytes, 16);
      leaf->count = n;
      for (size_t i = 0; i < n; ++i)
        leaf->beamID[i] = indices[begin+i];
      return BeamNodeRef::encodeLeaf(leaf, n);
    }

    /* SAH split: evaluate all splits along each axis on sorted
     * centroids, exactly like a single-threaded SAH builder. */
    float bestCost = std::numeric_limits<float>::infinity();
    int bestAxis = 0;
    size_t bestSplit = begin + n / 2;
    const float parentArea = safeArea(bounds);

    for (int axis = 0; axis < 3; ++axis)
    {
      std::sort(indices.begin()+begin, indices.begin()+end,
        [&](size_t a, size_t b)
        {
          const BBox3fa& ba = leafBounds[a];
          const BBox3fa& bb = leafBounds[b];
          const float ca = 0.5f*((&ba.lower.x)[axis]+(&ba.upper.x)[axis]);
          const float cb = 0.5f*((&bb.lower.x)[axis]+(&bb.upper.x)[axis]);
          return ca < cb;
        });

      std::vector<BBox3fa> leftBoxes(n, BBox3fa(EmptyTy()));
      BBox3fa left = BBox3fa(EmptyTy());
      for (size_t i = 0; i < n-1; ++i)
      {
        left.extend(leafBounds[indices[begin+i]]);
        leftBoxes[i] = left;
      }

      BBox3fa right = BBox3fa(EmptyTy());
      for (size_t k = n-1; k >= 1; --k)
      {
        right.extend(leafBounds[indices[begin+k]]);
        const size_t leftCount = k;
        const size_t rightCount = n - k;
        const float cost = safeArea(leftBoxes[k-1]) * float(leftCount)
                         + safeArea(right) * float(rightCount);
        if (cost < bestCost)
        {
          bestCost = cost;
          bestAxis = axis;
          bestSplit = begin + k;
        }
      }
    }

    /* A leaf is cheaper than any split (and encodable): keep one. */
    if (n <= MAX_LEAF_BEAMS && !depthLimit &&
        (parentArea == 0.0f || bestCost >= parentArea * float(n)))
    {
      const size_t bytes = sizeof(BeamLeaf) + (n-1) * sizeof(size_t);
      BeamLeaf* leaf = (BeamLeaf*) beamAlloc(alloc, bytes, 16);
      leaf->count = n;
      for (size_t i = 0; i < n; ++i)
        leaf->beamID[i] = indices[begin+i];
      return BeamNodeRef::encodeLeaf(leaf, n);
    }

    /* Restore the ordering of the best axis before recursing. */
    std::sort(indices.begin()+begin, indices.begin()+end,
      [&](size_t a, size_t b)
      {
        const BBox3fa& ba = leafBounds[a];
        const BBox3fa& bb = leafBounds[b];
        const float ca = 0.5f*((&ba.lower.x)[bestAxis]+(&ba.upper.x)[bestAxis]);
        const float cb = 0.5f*((&bb.lower.x)[bestAxis]+(&bb.upper.x)[bestAxis]);
        return ca < cb;
      });

    /* Degenerate geometry with identical centroids on every axis:
     * force a median split so progress is guaranteed. */
    if (centroidBounds.empty())
      bestSplit = begin + n / 2;

    const BeamNodeRef leftRef  = buildRecursive(begin, bestSplit, depth+1);
    const BeamNodeRef rightRef = buildRecursive(bestSplit, end, depth+1);

    BBox3fa leftBounds = BBox3fa(EmptyTy());
    BBox3fa rightBounds = BBox3fa(EmptyTy());
    for (size_t i = begin; i < bestSplit; ++i) leftBounds.extend(leafBounds[indices[i]]);
    for (size_t i = bestSplit; i < end; ++i)  rightBounds.extend(leafBounds[indices[i]]);

    size_t nodeBytes = sizeof(BeamAABBNode);
    void* nodeMem = alloc.malloc(nodeBytes, BeamNodeRef::byteNodeAlignment, false);
    BeamAABBNode* node = new (nodeMem) BeamAABBNode();
    node->clear();
    node->set(0, leftRef, leftBounds);
    node->set(1, rightRef, rightBounds);
    return BeamNodeRef::encodeNode(node);
  }

  static inline bool rayHitsBox(const BBox3fa& box,
                                const Vec3d& O, const Vec3d& D,
                                double tnear, double tfar)
  {
    double t0 = tnear;
    double t1 = tfar;
    const double lo[3] = { double(box.lower.x), double(box.lower.y), double(box.lower.z) };
    const double hi[3] = { double(box.upper.x), double(box.upper.y), double(box.upper.z) };
    const double o[3] = { O.x, O.y, O.z };
    const double d[3] = { D.x, D.y, D.z };

    for (int axis = 0; axis < 3; ++axis)
    {
      if (d[axis] == 0.0)
      {
        if (o[axis] < lo[axis] || o[axis] > hi[axis])
          return false;
      }
      else
      {
        double tlo = (lo[axis]-o[axis]) / d[axis];
        double thi = (hi[axis]-o[axis]) / d[axis];
        if (tlo > thi) std::swap(tlo, thi);
        t0 = std::max(t0, tlo);
        t1 = std::min(t1, thi);
        if (t1 < t0) return false;
      }
    }
    return true;
  }

  PhotonBeamMap::Interval PhotonBeamMap::intersectBeam(const Beam& beam,
                                                       const Vec3d& O,
                                                       const Vec3d& D,
                                                       double tnear, double tfar)
  {
    Interval result { false, 0.0, 0.0 };

    const Vec3d P0 = O - beam.from;
    const double sd = beamDot(D, beam.w);
    const double s0 = beamDot(P0, beam.w);

    /* Flat end caps: 0 <= s0 + sd*t <= L. */
    double lo = tnear;
    double hi = tfar;
    if (sd == 0.0)
    {
      if (s0 < 0.0 || s0 > beam.length)
        return result;
    }
    else
    {
      const double tCap0 = -s0 / sd;
      const double tCapL = (beam.length - s0) / sd;
      lo = std::max(lo, std::min(tCap0, tCapL));
      hi = std::min(hi, std::max(tCap0, tCapL));
      if (hi <= lo)
        return result;
    }

    /* Radial quadratic expressed through cross products. For a unit
     * axis, |(X-from) x w| is the distance to the axis line; building
     * the quadratic this way never subtracts nearly equal large terms,
     * so near-parallel rays stay accurate regardless of FMA contraction
     * in the vector primitives. */
    const Vec3d rel = O - beam.from;
    const Vec3d u = beamCross(rel, beam.w);          // (O-from) x w
    const Vec3d v = beamCross(D, beam.w);            // D x w
    const double a = beamDot(v, v);
    const double b = 2.0 * beamDot(u, v);
    const double c = beamDot(u, u) - beam.radius * beam.radius;

    if (a == 0.0)
    {
      /* Ray parallel to the beam axis. */
      if (!(c < 0.0))
        return result;
    }
    else
    {
      const double disc = b*b - 4.0*a*c;
      if (!(disc > 0.0))
        return result; // disc == 0 is a tangent: zero measure, contribution 0
      /* Stable quadratic roots (Numerical Recipes style). */
      const double sq = sqrt(disc);
      const double q = -0.5 * (b + (b >= 0.0 ? sq : -sq));
      double tRoot0 = q / a;
      double tRoot1 = c / q;
      if (tRoot0 > tRoot1) std::swap(tRoot0, tRoot1);
      lo = std::max(lo, tRoot0);
      hi = std::min(hi, tRoot1);
      if (hi <= lo)
        return result;
    }

    result.valid = true;
    result.t0 = lo;
    result.t1 = hi;
    return result;
  }

  bool PhotonBeamMap::integrate(const RTCBeamQuery& query,
                                double rgb[3], RTCBeamStats* stats) const
  {
    /* Validate the query. On failure the device error is raised and the
     * caller returns false without touching rgb. */
    for (int i = 0; i < 3; ++i)
    {
      if (!std::isfinite(query.origin[i]) || !std::isfinite(query.direction[i]))
        throw_RTCError(RTC_ERROR_INVALID_ARGUMENT, "non-finite beam query");
    }
    if (!std::isfinite(query.tnear) || !std::isfinite(query.tfar) ||
        !std::isfinite(query.sigma_t) || !std::isfinite(query.sigma_s))
      throw_RTCError(RTC_ERROR_INVALID_ARGUMENT, "non-finite beam query");

    const Vec3d D(query.direction[0], query.direction[1], query.direction[2]);
    const Vec3d O(query.origin[0], query.origin[1], query.origin[2]);
    const double dlen2 = beamDot(D, D);
    if (!(dlen2 > 0.0))
      throw_RTCError(RTC_ERROR_INVALID_ARGUMENT, "beam query direction must be non-zero");
    if (query.tnear < 0.0 || query.tfar < query.tnear)
      throw_RTCError(RTC_ERROR_INVALID_ARGUMENT, "invalid beam query t range");
    if (query.sigma_t < 0.0 || query.sigma_s < 0.0 || query.sigma_s > query.sigma_t)
      throw_RTCError(RTC_ERROR_INVALID_ARGUMENT, "invalid beam query coefficients");

    if (stats)
    {
      stats->nodeVisits = 0;
      stats->beamTests = 0;
      stats->supportIntervals = 0;
    }

    if (beams.empty())
      return true;

    const double dlen = sqrt(dlen2);
    const double k = query.sigma_t * dlen;
    static const double PI = 3.14159265358979323846;
    const double fourPi2 = 4.0 * PI * PI;

    BeamNodeRef stack[BVHN<4>::maxDepth + 2];
    size_t sp = 0;
    stack[sp++] = root;

    while (sp > 0)
    {
      const BeamNodeRef ref = stack[--sp];

      if (ref.isLeaf())
      {
        size_t num = 0;
        BeamLeaf* leaf = (BeamLeaf*) ref.leaf(num);
        for (size_t j = 0; j < num; ++j)
        {
          const size_t id = leaf->beamID[j];
          if (stats) stats->beamTests++;

          const Interval iv = intersectBeam(beams[id], O, D,
                                             query.tnear, query.tfar);
          if (!iv.valid)
            continue;
          if (stats) stats->supportIntervals++;

          /* Analytic transmittance integral, stable for tiny extinction
           * and very thin intervals via expm1. */
          double lengthIntegral;
          const double span = iv.t1 - iv.t0;
          if (k == 0.0)
          {
            lengthIntegral = dlen * span;
          }
          else
          {
            const double weight = -expm1(-k * span) / k;
            lengthIntegral = dlen * weight * exp(-k * (iv.t0 - query.tnear));
          }

          const Beam& beam = beams[id];
          const double density = 1.0 / (fourPi2 * beam.radius * beam.radius);
          const double factor = query.sigma_s * density * lengthIntegral;
          rgb[0] += beam.flux.x * factor;
          rgb[1] += beam.flux.y * factor;
          rgb[2] += beam.flux.z * factor;
        }
      }
      else
      {
        if (stats) stats->nodeVisits++;
        const BeamAABBNode* node = ref.getAABBNode();
        for (size_t i = 0; i < 4; ++i)
        {
          const BeamNodeRef child = node->child(i);
          if (child == BeamNodeRef::emptyNode)
            continue;
          if (rayHitsBox(node->bounds(i), O, D, query.tnear, query.tfar))
            stack[sp++] = child;
        }
      }
    }

    return true;
  }
}
