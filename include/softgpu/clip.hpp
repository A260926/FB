#pragma once
#include <cstdint>

#include "softgpu/math.hpp"

namespace softgpu {

template <int N>
constexpr int attr_count() {
  return N > 0 ? N : 1;
}

// Vertex shader output: clip-space position plus N varyings.
template <int N>
struct VsOut {
  Vec4 pos;
  float attr[attr_count<N>()] = {};
};

constexpr int kClipPlanes = 6;
constexpr int kMaxClipVerts = 3 + kClipPlanes;

// Signed distance to plane i in homogeneous clip space; inside is >= 0.
// Planes: x >= -w, x <= w, y >= -w, y <= w, z >= -w (near), z <= w (far).
inline float clip_dist(const Vec4& p, int plane) {
  switch (plane) {
    case 0: return p.w + p.x;
    case 1: return p.w - p.x;
    case 2: return p.w + p.y;
    case 3: return p.w - p.y;
    case 4: return p.w + p.z;
    default: return p.w - p.z;
  }
}

// Sutherland-Hodgman against all six frustum planes, in clip space so the lerp is affine and
// varyings stay exact. Writes up to kMaxClipVerts vertices to `out` and returns the count:
// 3 when nothing had to be done, 0 when the triangle is entirely outside some plane.
// The intersection point is always computed from the inside vertex towards the outside one, so
// two triangles sharing an edge produce bit-identical cut vertices and no cracks.
template <int N>
int clip_triangle(const VsOut<N>* in, VsOut<N>* out, bool* was_clipped = nullptr) {
  uint32_t any_out = 0, all_out = 0;
  for (int p = 0; p < kClipPlanes; ++p) {
    uint32_t mask = 0;
    for (int i = 0; i < 3; ++i)
      if (clip_dist(in[i].pos, p) < 0) mask |= 1u << i;
    if (mask) any_out |= 1u << p;
    if (mask == 7u) all_out |= 1u << p;
  }
  if (was_clipped) *was_clipped = any_out != 0;
  if (all_out) return 0;
  if (!any_out) {
    for (int i = 0; i < 3; ++i) out[i] = in[i];
    return 3;
  }

  VsOut<N> buf[2][kMaxClipVerts];
  VsOut<N>* cur = buf[0];
  VsOut<N>* nxt = buf[1];
  int n = 3;
  for (int i = 0; i < 3; ++i) cur[i] = in[i];

  for (int p = 0; p < kClipPlanes && n > 0; ++p) {
    if (!(any_out & (1u << p))) continue;
    int m = 0;
    for (int i = 0; i < n; ++i) {
      const VsOut<N>& a = cur[i];
      const VsOut<N>& b = cur[(i + 1) % n];
      const float da = clip_dist(a.pos, p), db = clip_dist(b.pos, p);
      const bool ina = da >= 0, inb = db >= 0;
      if (ina) nxt[m++] = a;
      if (ina != inb) {
        const VsOut<N>& from = ina ? a : b;
        const VsOut<N>& to = ina ? b : a;
        const float dfrom = ina ? da : db, dto = ina ? db : da;
        const float t = dfrom / (dfrom - dto);
        VsOut<N>& v = nxt[m++];
        v.pos = from.pos + (to.pos - from.pos) * t;
        for (int k = 0; k < attr_count<N>(); ++k) v.attr[k] = from.attr[k] + (to.attr[k] - from.attr[k]) * t;
      }
    }
    n = m;
    VsOut<N>* tmp = cur;
    cur = nxt;
    nxt = tmp;
  }
  for (int i = 0; i < n; ++i) out[i] = cur[i];
  return n;
}

}  // namespace softgpu
