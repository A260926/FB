// Copyright 2025 Photon beam task.
// SPDX-License-Identifier: Apache-2.0

#define RTC_EXPORT_API

#include "../common/photon_beam_internal.h"

#include "../common/device.h"
#include "bvh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace embree
{
  namespace
  {
    /*! Leaf payload.  The uint32 range indexes the build's identity-safe
        permutation array; reordering leaves never changes original beam IDs. */
    struct PhotonBeamLeaf
    {
      uint32_t first;
      uint32_t count;
    };

    struct DoubleBounds
    {
      double bmin[3];
      double bmax[3];
    };

    struct BuildResult
    {
      BVH4::NodeRef ref;
      DoubleBounds bounds;
    };

    DoubleBounds beamBounds(const std::vector<PhotonBeamData>& beams,
                            const std::vector<uint32_t>& ids,
                            size_t begin,
                            size_t end)
    {
      DoubleBounds out;
      for (int k = 0; k < 3; ++k)
      {
        out.bmin[k] = std::numeric_limits<double>::infinity();
        out.bmax[k] = -std::numeric_limits<double>::infinity();
      }

      for (size_t i = begin; i < end; ++i)
      {
        const PhotonBeamData& beam = beams[ids[i]];
        for (int k = 0; k < 3; ++k)
        {
          out.bmin[k] = std::min(out.bmin[k], beam.bmin[k]);
          out.bmax[k] = std::max(out.bmax[k], beam.bmax[k]);
        }
      }
      return out;
    }

    DoubleBounds beamCentroidBounds(const std::vector<PhotonBeamData>& beams,
                                    const std::vector<uint32_t>& ids,
                                    size_t begin,
                                    size_t end,
                                    int& axis)
    {
      DoubleBounds out;
      for (int k = 0; k < 3; ++k)
      {
        out.bmin[k] = std::numeric_limits<double>::infinity();
        out.bmax[k] = -std::numeric_limits<double>::infinity();
      }

      for (size_t i = begin; i < end; ++i)
      {
        const PhotonBeamData& beam = beams[ids[i]];
        for (int k = 0; k < 3; ++k)
        {
          const double c = 0.5 * (beam.bmin[k] + beam.bmax[k]);
          out.bmin[k] = std::min(out.bmin[k], c);
          out.bmax[k] = std::max(out.bmax[k], c);
        }
      }

      double extent[3] = {out.bmax[0] - out.bmin[0],
                          out.bmax[1] - out.bmin[1],
                          out.bmax[2] - out.bmin[2]};
      axis = 0;
      if (extent[1] > extent[0]) axis = 1;
      if (extent[2] > extent[axis]) axis = 2;
      return out;
    }

    float outwardLower(float value)
    {
      if (std::isfinite(value))
        return std::nextafterf(value, -std::numeric_limits<float>::infinity());
      return value;
    }

    float outwardUpper(float value)
    {
      if (std::isfinite(value))
        return std::nextafterf(value, std::numeric_limits<float>::infinity());
      return value;
    }

    BBox3fa conservativeNodeBounds(const DoubleBounds& bounds)
    {
      float lower[3], upper[3];
      for (int k = 0; k < 3; ++k)
      {
        double lo = bounds.bmin[k];
        double hi = bounds.bmax[k];

        /* An out-of-float-range bound is widened to an infinite child bound.
           Exact beam support is still tested in double/long double later. */
        if (std::isfinite(lo) && lo >= -std::numeric_limits<float>::max() &&
            lo <= std::numeric_limits<float>::max())
          lower[k] = outwardLower((float)lo);
        else
          lower[k] = -std::numeric_limits<float>::infinity();

        if (std::isfinite(hi) && hi >= -std::numeric_limits<float>::max() &&
            hi <= std::numeric_limits<float>::max())
          upper[k] = outwardUpper((float)hi);
        else
          upper[k] = std::numeric_limits<float>::infinity();
      }

      return BBox3fa(Vec3fa(lower[0], lower[1], lower[2]),
                     Vec3fa(upper[0], upper[1], upper[2]));
    }

    /*! Conservative slab test against one float BVH4 child bound.  Exact beam
        support is still evaluated in long double after this index test. */
    bool nodeBoundsHit(const BBox3fa& box, const RTCBeamQuery& query)
    {
      double tmin = query.tnear;
      double tmax = query.tfar;
      const double origin[3] = {query.origin[0], query.origin[1], query.origin[2]};
      const double dir[3] = {query.direction[0], query.direction[1], query.direction[2]};
      const double lo[3] = {box.lower.x, box.lower.y, box.lower.z};
      const double hi[3] = {box.upper.x, box.upper.y, box.upper.z};

      for (int axis = 0; axis < 3; ++axis)
      {
        if (dir[axis] == 0.0)
        {
          if (origin[axis] < lo[axis] || origin[axis] > hi[axis])
            return false;
        }
        else
        {
          double t0 = (lo[axis] - origin[axis]) / dir[axis];
          double t1 = (hi[axis] - origin[axis]) / dir[axis];
          if (t0 > t1) std::swap(t0, t1);
          tmin = std::max(tmin, t0);
          tmax = std::min(tmax, t1);
          if (!(tmin <= tmax))
            return false;
        }
      }
      return true;
    }
  }

  class BVH4PhotonBeamIndex final : public PhotonBeamIndex
  {
  public:
    static constexpr size_t LEAF_SIZE = 4;
    static constexpr int SAH_BINS = 16;

    BVH4PhotonBeamIndex(Device* device, const std::vector<PhotonBeamData>& inputBeams)
      : beams(inputBeams), allocator(device, false)
    {
      const size_t n = beams.size();
      ids.resize(n);
      for (uint32_t i = 0; i < n; ++i)
        ids[i] = i;

      if (n == 0)
      {
        root = BVH4::NodeRef(BVH4::emptyNode);
        return;
      }

      auto cached = allocator.getCachedAllocator();
      BuildResult result = build(0, n, cached);
      root = result.ref;
    }

    ~BVH4PhotonBeamIndex() override = default;

    void integrate(const RTCBeamQuery& query,
                   long double rgb[3],
                   RTCBeamStats& stats) const override
    {
      if (ids.empty() || root == BVH4::NodeRef(BVH4::emptyNode))
        return;

      std::vector<BVH4::NodeRef> stack;
      stack.reserve(64);
      stack.push_back(root);

      while (!stack.empty())
      {
        BVH4::NodeRef ref = stack.back();
        stack.pop_back();
        ++stats.nodeVisits;

        if (ref.isLeaf())
        {
          size_t leafBytes = 0;
          char* leafMemory = ref.leaf(leafBytes);
          const PhotonBeamLeaf* leaf = reinterpret_cast<const PhotonBeamLeaf*>(leafMemory);

          for (uint32_t j = 0; j < leaf->count; ++j)
          {
            const uint32_t beamId = ids[leaf->first + j];
            ++stats.beamTests;

            uint64_t intervals = 0;
            integratePhotonBeam(beams[beamId], query, rgb, intervals);
            stats.supportIntervals += intervals;
          }
          continue;
        }

        BVH4::AABBNode* node = ref.getAABBNode();
        for (int i = 0; i < 4; ++i)
        {
          BVH4::NodeRef child = node->child(i);
          if (child == BVH4::NodeRef(BVH4::emptyNode))
            continue;

          const BBox3fa childBounds = node->bounds(i);
          if (!nodeBoundsHit(childBounds, query))
            continue;

          stack.push_back(child);
        }
      }
    }

  private:
    BuildResult makeLeaf(size_t begin, size_t end,
                         const FastAllocator::CachedAllocator& alloc,
                         const DoubleBounds& bounds)
    {
      PhotonBeamLeaf* leaf =
        (PhotonBeamLeaf*)alloc(sizeof(PhotonBeamLeaf), BVH4::byteNodeAlignment);
      leaf->first = (uint32_t)begin;
      leaf->count = (uint32_t)(end - begin);

      BVH4::NodeRef ref = BVH4::encodeTypedLeaf(leaf, 0);
      return {ref, bounds};
    }

    size_t sahSplit(size_t begin, size_t end, int axis,
                    const DoubleBounds& beamCentroidBounds)
    {
      struct Bin
      {
        int count = 0;
        DoubleBounds bounds;
        Bin()
        {
          for (int k = 0; k < 3; ++k)
          {
            bounds.bmin[k] = std::numeric_limits<double>::infinity();
            bounds.bmax[k] = -std::numeric_limits<double>::infinity();
          }
        }
      };

      std::array<Bin, SAH_BINS> bins;
      const double cmin = beamCentroidBounds.bmin[axis];
      const double cmax = beamCentroidBounds.bmax[axis];
      const double scale = SAH_BINS / (cmax - cmin);

      auto centroid = [&](uint32_t id) {
        return 0.5 * (beams[id].bmin[axis] + beams[id].bmax[axis]);
      };

      for (size_t i = begin; i < end; ++i)
      {
        const double c = centroid(ids[i]);
        int bin = (int)((c - cmin) * scale);
        bin = std::max(0, std::min(SAH_BINS - 1, bin));
        Bin& b = bins[bin];
        ++b.count;
        for (int k = 0; k < 3; ++k)
        {
          const PhotonBeamData& beam = beams[ids[i]];
          b.bounds.bmin[k] = std::min(b.bounds.bmin[k], beam.bmin[k]);
          b.bounds.bmax[k] = std::max(b.bounds.bmax[k], beam.bmax[k]);
        }
      }

      std::array<double, SAH_BINS + 1> leftCost{};
      std::array<double, SAH_BINS + 1> rightCost{};

      DoubleBounds leftBounds;
      int leftCount = 0;
      for (int i = 0; i < SAH_BINS; ++i)
      {
        if (bins[i].count)
        {
          if (leftCount == 0) leftBounds = bins[i].bounds;
          else
            for (int k = 0; k < 3; ++k)
            {
              leftBounds.bmin[k] = std::min(leftBounds.bmin[k], bins[i].bounds.bmin[k]);
              leftBounds.bmax[k] = std::max(leftBounds.bmax[k], bins[i].bounds.bmax[k]);
            }
          leftCount += bins[i].count;
        }
        leftCost[i + 1] = (double)leftCount * boundsArea(leftBounds);
      }

      DoubleBounds rightBounds;
      int rightCount = 0;
      for (int i = SAH_BINS - 1; i >= 0; --i)
      {
        if (bins[i].count)
        {
          if (rightCount == 0) rightBounds = bins[i].bounds;
          else
            for (int k = 0; k < 3; ++k)
            {
              rightBounds.bmin[k] = std::min(rightBounds.bmin[k], bins[i].bounds.bmin[k]);
              rightBounds.bmax[k] = std::max(rightBounds.bmax[k], bins[i].bounds.bmax[k]);
            }
          rightCount += bins[i].count;
        }
        rightCost[i] = (double)rightCount * boundsArea(rightBounds);
      }

      double bestCost = std::numeric_limits<double>::infinity();
      int bestSplit = -1;
      for (int s = 1; s < SAH_BINS; ++s)
      {
        if (leftCost[s] <= 0.0 || rightCost[s] <= 0.0)
          continue;
        const double cost = leftCost[s] + rightCost[s];
        if (cost < bestCost)
        {
          bestCost = cost;
          bestSplit = s;
        }
      }

      if (bestSplit < 0)
        return begin + (end - begin) / 2;

      auto middle = std::partition(ids.begin() + begin, ids.begin() + end,
                                   [&](uint32_t id) {
                                     const double c = centroid(id);
                                     int bin = (int)((c - cmin) * scale);
                                     bin = std::max(0, std::min(SAH_BINS - 1, bin));
                                     return bin < bestSplit;
                                   });
      size_t mid = (size_t)(middle - ids.begin());
      if (mid == begin || mid == end)
        return begin + (end - begin) / 2;
      return mid;
    }

    static double boundsArea(const DoubleBounds& b)
    {
      if (!std::isfinite(b.bmin[0]) || !std::isfinite(b.bmax[0]))
        return 0.0;
      const double x = std::max(0.0, b.bmax[0] - b.bmin[0]);
      const double y = std::max(0.0, b.bmax[1] - b.bmin[1]);
      const double z = std::max(0.0, b.bmax[2] - b.bmin[2]);
      return 2.0 * (x * y + y * z + z * x);
    }

    BuildResult build(size_t begin, size_t end,
                      const FastAllocator::CachedAllocator& alloc)
    {
      const DoubleBounds bounds = beamBounds(beams, ids, begin, end);
      const size_t count = end - begin;

      if (count <= LEAF_SIZE)
        return makeLeaf(begin, end, alloc, bounds);

      int axis = 0;
      DoubleBounds centroids = beamCentroidBounds(beams, ids, begin, end, axis);
      const double centroidExtent = centroids.bmax[axis] - centroids.bmin[axis];

      size_t mid;
      if (centroidExtent > 0.0 && std::isfinite(centroidExtent))
      {
        mid = sahSplit(begin, end, axis, centroids);
        if (mid == begin || mid == end)
        {
          mid = begin + count / 2;
          std::nth_element(ids.begin() + begin, ids.begin() + mid, ids.begin() + end,
                            [&](uint32_t a, uint32_t b) {
                              const double ca = 0.5 * (beams[a].bmin[axis] + beams[a].bmax[axis]);
                              const double cb = 0.5 * (beams[b].bmin[axis] + beams[b].bmax[axis]);
                              if (ca != cb) return ca < cb;
                              return a < b;
                            });
        }
      }
      else
      {
        /* Coincident centroids: balanced identity/tie split preserves all
           IDs and still gives logarithmic traversal for sparse inputs. */
        mid = begin + count / 2;
        std::nth_element(ids.begin() + begin, ids.begin() + mid, ids.begin() + end,
                          [&](uint32_t a, uint32_t b) { return a < b; });
      }

      BuildResult left = build(begin, mid, alloc);
      BuildResult right = build(mid, end, alloc);

      BVH4::NodeRef nodeRef = BVH4::AABBNode::Create()(alloc);
      BVH4::AABBNode* node = nodeRef.getAABBNode();
      node->set(0, left.ref, conservativeNodeBounds(left.bounds));
      node->set(1, right.ref, conservativeNodeBounds(right.bounds));
      /* Children 2 and 3 remain empty because Node::Create clears the node. */

      return {nodeRef, bounds};
    }

    const std::vector<PhotonBeamData>& beams;
    std::vector<uint32_t> ids;
    FastAllocator allocator;
    BVH4::NodeRef root;
  };

  PhotonBeamIndex* createPhotonBeamBVH(Device* device,
                                       const std::vector<PhotonBeamData>& beams)
  {
    return new BVH4PhotonBeamIndex(device, beams);
  }
}
