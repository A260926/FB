#include "query_index.hpp"

#include <algorithm>
#include <limits>

namespace incremental {

void QueryIndex::Reset(const AABB& domain) {
  domain_ = domain;
  has_domain_ = true;
  cells_.assign(kRes * kRes * kRes, {});
  entries_.clear();
  next_handle_ = 1;
  query_epoch_ = 0;
  visit_count = 0;
}

int QueryIndex::CellAxis(float v, int axis) const {
  const float mn = domain_.min[axis];
  const float mx = domain_.max[axis];
  if (!(mx > mn)) return 0;
  const float t = (v - mn) / (mx - mn);
  const int i = static_cast<int>(std::floor(t * kRes));
  return i < 0 ? 0 : (i >= kRes ? kRes - 1 : i);
}

bool QueryIndex::BoxCellRange(const AABB& box, int lo[3], int hi[3]) const {
  if (!has_domain_) return false;
  for (int i = 0; i < 3; ++i) {
    const float mn = std::max(box.min[i], domain_.min[i]);
    const float mx = std::min(box.max[i], domain_.max[i]);
    if (mn > mx) return false;
    lo[i] = CellAxis(mn, i);
    hi[i] = CellAxis(mx, i);
  }
  return true;
}

bool QueryIndex::ClipSegmentToDomain(const Vec3f& a, const Vec3f& b,
                                     float& t0, float& t1) const {
  t0 = 0.0f;
  t1 = 1.0f;
  for (int i = 0; i < 3; ++i) {
    const float d = b[i] - a[i];
    const float mn = domain_.min[i];
    const float mx = domain_.max[i];
    if (std::abs(d) < 1e-12f) {
      if (a[i] < mn || a[i] > mx) return false;
      continue;
    }
    const float inv = 1.0f / d;
    float ta = (mn - a[i]) * inv;
    float tb = (mx - a[i]) * inv;
    if (ta > tb) std::swap(ta, tb);
    t0 = std::max(t0, ta);
    t1 = std::min(t1, tb);
    if (t0 > t1) return false;
  }
  return true;
}

void QueryIndex::CellsForSegment(const Vec3f& a, const Vec3f& b,
                                 std::vector<int>& out) const {
  out.clear();
  float t0, t1;
  if (!ClipSegmentToDomain(a, b, t0, t1)) return;
  const Vec3f p0 = a + (b - a) * t0;
  const Vec3f p1 = a + (b - a) * t1;
  const Vec3f d = p1 - p0;

  int cur[3], end[3], step[3];
  float t_max[3], t_delta[3];
  constexpr float inf = std::numeric_limits<float>::infinity();
  for (int i = 0; i < 3; ++i) {
    cur[i] = CellAxis(p0[i], i);
    end[i] = CellAxis(p1[i], i);
    const float cell_size = (domain_.max[i] - domain_.min[i]) / kRes;
    if (d[i] > 0.0f) {
      step[i] = 1;
    } else if (d[i] < 0.0f) {
      step[i] = -1;
    } else {
      step[i] = 0;
    }
    if (step[i] != 0 && cell_size > 0.0f) {
      // parametric distance to the first cell boundary along this axis
      const float boundary =
          domain_.min[i] + (cur[i] + (step[i] > 0 ? 1 : 0)) * cell_size;
      t_max[i] = (boundary - p0[i]) / d[i];
      t_delta[i] = cell_size / std::abs(d[i]);
    } else {
      t_max[i] = inf;
      t_delta[i] = inf;
    }
  }

  // 3D DDA walk (Amanatides & Woo) from the entering to the leaving cell
  out.push_back(CellId(cur[0], cur[1], cur[2]));
  const int max_steps = 3 * kRes + 3;
  for (int guard = 0;
       (cur[0] != end[0] || cur[1] != end[1] || cur[2] != end[2]) &&
       guard < max_steps;
       ++guard) {
    int axis = 0;
    if (t_max[1] < t_max[axis]) axis = 1;
    if (t_max[2] < t_max[axis]) axis = 2;
    cur[axis] += step[axis];
    t_max[axis] += t_delta[axis];
    for (int i = 0; i < 3; ++i)
      cur[i] = cur[i] < 0 ? 0 : (cur[i] >= kRes ? kRes - 1 : cur[i]);
    out.push_back(CellId(cur[0], cur[1], cur[2]));
  }
}

uint64_t QueryIndex::Insert(uint64_t consumer, Kind kind, const Vec3f& a,
                            const Vec3f& b) {
  if (!has_domain_) return 0;
  Entry e;
  e.consumer = consumer;
  e.kind = kind;
  e.a = a;
  e.b = b;
  CellsForSegment(a, b, e.cells);
  if (e.cells.empty()) return 0;
  const uint64_t h = next_handle_++;
  for (int c : e.cells) {
    cells_[c].push_back(h);
    ++visit_count;
  }
  entries_.emplace(h, std::move(e));
  return h;
}

void QueryIndex::Remove(uint64_t handle) {
  if (!handle) return;
  auto it = entries_.find(handle);
  if (it == entries_.end()) return;
  for (int c : it->second.cells) {
    ++visit_count;
    auto& v = cells_[c];
    for (std::size_t i = 0; i < v.size(); ++i) {
      if (v[i] == handle) {
        v[i] = v.back();
        v.pop_back();
        break;
      }
    }
  }
  entries_.erase(it);
}

}  // namespace incremental
