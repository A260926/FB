#ifndef INCREMENTAL_QUERY_INDEX_HPP_
#define INCREMENTAL_QUERY_INDEX_HPP_

// Reverse spatial index for the incremental preview.
//
// Stores the finite dependency queries recorded while tracing a frame:
//   - nearest-hit segments (ray origin -> old hit, or the whole forward
//     ray conservatively truncated to the fixed edit domain on a miss)
//   - shadow segments (full point-light segment or directional ray,
//     truncated to the edit domain)
//   - medium probe points (SurroundingIOR)
//
// The index maps grid cells -> records so that, given the pre/post-edit
// bounds of an edited sphere, only the records whose segment/point exactly
// intersects the box are reported (candidates from the grid are filtered
// with an exact segment/box or point/box test). Records can be inserted,
// removed and locally updated (remove + insert) without rebuilding the
// whole index and without scanning all records per edit.

#include "vec.hpp"
#include "ray.hpp"
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace incremental {

// Exact overlap test between the closed segment [a,b] and a finite box
// (slab method over the segment parameter t in [0,1]).
inline bool SegmentIntersectsAABB(const Vec3f& a, const Vec3f& b,
                                  const AABB& box) {
  float tmin = 0.0f, tmax = 1.0f;
  for (int i = 0; i < 3; ++i) {
    const float d = b[i] - a[i];
    if (std::abs(d) < 1e-12f) {
      if (a[i] < box.min[i] || a[i] > box.max[i]) return false;
      continue;
    }
    const float inv = 1.0f / d;
    float t1 = (box.min[i] - a[i]) * inv;
    float t2 = (box.max[i] - a[i]) * inv;
    if (t1 > t2) std::swap(t1, t2);
    tmin = t1 > tmin ? t1 : tmin;
    tmax = t2 < tmax ? t2 : tmax;
    if (tmin > tmax) return false;
  }
  return true;
}

class QueryIndex {
public:
  enum class Kind : uint8_t { Hit = 0, Shadow = 1, Probe = 2 };

  // (Re)initialize the index over a fixed finite edit domain.
  // All records are dropped.
  void Reset(const AABB& domain);

  // Insert the closed finite segment [a,b] (use a == b for point probes).
  // Returns a handle (> 0), or 0 when the segment lies fully outside the
  // edit domain (such a record can never be invalidated by an in-domain
  // edit, so it needs no storage).
  uint64_t Insert(uint64_t consumer, Kind kind, const Vec3f& a,
                  const Vec3f& b);
  void Remove(uint64_t handle);

  // Call cb(consumer, kind) for every record whose segment/point exactly
  // intersects `box`. Grid cells overlapping the box give the candidates
  // (no full scan); each candidate is verified with an exact test.
  template <typename F>
  void Query(const AABB& box, F&& cb) {
    ++query_epoch_;
    int lo[3], hi[3];
    if (!BoxCellRange(box, lo, hi)) return;
    for (int x = lo[0]; x <= hi[0]; ++x)
      for (int y = lo[1]; y <= hi[1]; ++y)
        for (int z = lo[2]; z <= hi[2]; ++z) {
          ++visit_count;
          const auto& cell = cells_[CellId(x, y, z)];
          for (uint64_t h : cell) {
            auto it = entries_.find(h);
            if (it == entries_.end()) continue;
            Entry& e = it->second;
            if (e.stamp == query_epoch_) continue;  // already reported
            e.stamp = query_epoch_;
            const bool match =
                (e.kind == Kind::Probe)
                    ? PointInAABB(box, e.a)
                    : SegmentIntersectsAABB(e.a, e.b, box);
            if (match) cb(e.consumer, e.kind);
          }
        }
  }

  uint64_t LiveCount() const { return entries_.size(); }

  // number of grid nodes (cells) touched since the last reset; used for
  // the per-frame index_nodes_visited statistic
  uint64_t visit_count = 0;

private:
  struct Entry {
    uint64_t consumer = 0;
    Kind kind = Kind::Hit;
    Vec3f a{}, b{};
    std::vector<int> cells;
    uint64_t stamp = 0;  // last query epoch that reported this entry
  };

  static constexpr int kRes = 16;  // cells per axis

  static int CellId(int x, int y, int z) {
    return (x * kRes + y) * kRes + z;
  }
  int CellAxis(float v, int axis) const;
  bool BoxCellRange(const AABB& box, int lo[3], int hi[3]) const;
  bool ClipSegmentToDomain(const Vec3f& a, const Vec3f& b, float& t0,
                           float& t1) const;
  void CellsForSegment(const Vec3f& a, const Vec3f& b,
                       std::vector<int>& out) const;

  AABB domain_{};
  bool has_domain_ = false;
  std::vector<std::vector<uint64_t>> cells_;
  std::unordered_map<uint64_t, Entry> entries_;
  uint64_t next_handle_ = 1;
  uint64_t query_epoch_ = 0;
};

}  // namespace incremental

#endif  // INCREMENTAL_QUERY_INDEX_HPP_
