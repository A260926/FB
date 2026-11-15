#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "softgpu/math.hpp"

namespace softgpu {

// Vertices are snapped to a 28.4 fixed-point grid (1/16 pixel) before setup, so every
// edge function is evaluated in exact int64 arithmetic: coverage never depends on
// float rounding, on the tile a pixel lands in, or on which thread rasterizes it.
constexpr int kSubPixelBits = 4;
constexpr int kSubPixelScale = 1 << kSubPixelBits;

// Inclusive pixel bounds. Empty when x0 > x1 or y0 > y1.
struct Rect {
  int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
  bool empty() const { return x0 > x1 || y0 > y1; }
  Rect intersect(const Rect& o) const {
    return {std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1), std::min(y1, o.y1)};
  }
};

enum class CullMode { None, Back, Front };

// Post-viewport vertex: x, y in pixels (y down), z in [0, 1], invw = 1 / w_clip, attr already
// divided by w_clip so both interpolate linearly in screen space.
template <int N>
struct ScreenVertex {
  float x = 0, y = 0, z = 0, invw = 1;
  float attr[N > 0 ? N : 1] = {};
};

// Everything the per-tile loop needs: three edge functions E_i(px, py) = a_i*px + b_i*py + c_i
// in fixed-point sample coordinates, fill-rule biases, and the per-vertex values to interpolate.
// Edge i is opposite vertex i, so E_i / area is barycentric weight i.
template <int N>
struct SetupTri {
  int64_t a[3], b[3], c[3];
  int64_t bias[3];
  float inv_area;
  float z[3], invw[3];
  float attr[3][N > 0 ? N : 1];
  float dl_dx[3], dl_dy[3];  // d(lambda_i)/d(pixel)
  // Screen-space gradients of the linearly interpolated quantities (attr/w and 1/w), constant
  // per triangle; the perspective-correct derivative is assembled per pixel by the quotient rule.
  float dattr_dx[N > 0 ? N : 1], dattr_dy[N > 0 ? N : 1];
  float dinvw_dx, dinvw_dy;
  Rect bbox;
  bool front_facing;
};

inline int floor_div16(int v) { return v >= 0 ? v >> kSubPixelBits : -((-v + kSubPixelScale - 1) >> kSubPixelBits); }

// Top-left rule. With E > 0 as "inside" after orientation, a "top" edge is horizontal with the
// interior below it (q.x > p.x) and a "left" edge has the interior on its right (q.y < p.y).
// Samples exactly on such an edge are owned by this triangle; on any other edge they are not,
// so a sample on an edge shared by two triangles is drawn exactly once.
inline bool is_top_left(int px, int py, int qx, int qy) { return (py == qy && qx > px) || (qy < py); }

enum class Setup { Ok, Degenerate, Culled, Offscreen };

// Front-facing = counter-clockwise on screen with y up (OpenGL's default), which is a
// negative signed area in y-down pixel space.
template <int N>
Setup setup_triangle(const ScreenVertex<N>* v, CullMode cull, int width, int height, SetupTri<N>& t) {
  int x[3], y[3];
  for (int i = 0; i < 3; ++i) {
    x[i] = static_cast<int>(std::lround(v[i].x * kSubPixelScale));
    y[i] = static_cast<int>(std::lround(v[i].y * kSubPixelScale));
  }
  const int64_t area = static_cast<int64_t>(x[1] - x[0]) * (y[2] - y[0]) - static_cast<int64_t>(y[1] - y[0]) * (x[2] - x[0]);
  if (area == 0) return Setup::Degenerate;
  t.front_facing = area < 0;
  if (cull == CullMode::Back && !t.front_facing) return Setup::Culled;
  if (cull == CullMode::Front && t.front_facing) return Setup::Culled;

  // Reorder so the signed area is positive and every edge function is positive inside.
  int idx[3] = {0, 1, 2};
  if (area < 0) std::swap(idx[1], idx[2]);
  const ScreenVertex<N>* sv[3] = {&v[idx[0]], &v[idx[1]], &v[idx[2]]};
  const int X[3] = {x[idx[0]], x[idx[1]], x[idx[2]]};
  const int Y[3] = {y[idx[0]], y[idx[1]], y[idx[2]]};

  for (int i = 0; i < 3; ++i) {
    const int p = (i + 1) % 3, q = (i + 2) % 3;  // edge opposite vertex i runs p -> q
    t.a[i] = Y[p] - Y[q];
    t.b[i] = X[q] - X[p];
    t.c[i] = static_cast<int64_t>(X[p]) * Y[q] - static_cast<int64_t>(Y[p]) * X[q];
    t.bias[i] = is_top_left(X[p], Y[p], X[q], Y[q]) ? 0 : -1;
    t.z[i] = sv[i]->z;
    t.invw[i] = sv[i]->invw;
    for (int k = 0; k < N; ++k) t.attr[i][k] = sv[i]->attr[k];
  }
  t.inv_area = 1.0f / static_cast<float>(area < 0 ? -area : area);
  for (int i = 0; i < 3; ++i) {
    t.dl_dx[i] = static_cast<float>(t.a[i] * kSubPixelScale) * t.inv_area;
    t.dl_dy[i] = static_cast<float>(t.b[i] * kSubPixelScale) * t.inv_area;
  }
  t.dinvw_dx = t.dl_dx[0] * t.invw[0] + t.dl_dx[1] * t.invw[1] + t.dl_dx[2] * t.invw[2];
  t.dinvw_dy = t.dl_dy[0] * t.invw[0] + t.dl_dy[1] * t.invw[1] + t.dl_dy[2] * t.invw[2];
  for (int k = 0; k < N; ++k) {
    t.dattr_dx[k] = t.dl_dx[0] * t.attr[0][k] + t.dl_dx[1] * t.attr[1][k] + t.dl_dx[2] * t.attr[2][k];
    t.dattr_dy[k] = t.dl_dy[0] * t.attr[0][k] + t.dl_dy[1] * t.attr[1][k] + t.dl_dy[2] * t.attr[2][k];
  }
  // Pixel px covers sample 16*px + 8, so floor(min/16) .. floor(max/16) is a tight-enough box.
  t.bbox.x0 = std::max(0, floor_div16(std::min({X[0], X[1], X[2]})));
  t.bbox.y0 = std::max(0, floor_div16(std::min({Y[0], Y[1], Y[2]})));
  t.bbox.x1 = std::min(width - 1, floor_div16(std::max({X[0], X[1], X[2]})));
  t.bbox.y1 = std::min(height - 1, floor_div16(std::max({Y[0], Y[1], Y[2]})));
  return t.bbox.empty() ? Setup::Offscreen : Setup::Ok;
}

// Walks every pixel of `clip` (already intersected with the bbox by the caller) and calls
// frag(px, py, l0, l1, l2) for covered samples. Edge values at each row start are computed
// from absolute coordinates, so the same pixel gets bit-identical barycentrics no matter
// which rectangle it was visited through.
template <int N, class FragFn>
void rasterize(const SetupTri<N>& t, const Rect& clip, FragFn&& frag) {
  if (clip.empty()) return;
  const int64_t sx0 = static_cast<int64_t>(clip.x0) * kSubPixelScale + kSubPixelScale / 2;
  const int64_t step[3] = {t.a[0] * kSubPixelScale, t.a[1] * kSubPixelScale, t.a[2] * kSubPixelScale};
  for (int py = clip.y0; py <= clip.y1; ++py) {
    const int64_t sy = static_cast<int64_t>(py) * kSubPixelScale + kSubPixelScale / 2;
    int64_t e[3];
    for (int i = 0; i < 3; ++i) e[i] = t.a[i] * sx0 + t.b[i] * sy + t.c[i];
    for (int px = clip.x0; px <= clip.x1; ++px) {
      if (((e[0] + t.bias[0]) | (e[1] + t.bias[1]) | (e[2] + t.bias[2])) >= 0) {
        frag(px, py, static_cast<float>(e[0]) * t.inv_area, static_cast<float>(e[1]) * t.inv_area,
             static_cast<float>(e[2]) * t.inv_area);
      }
      e[0] += step[0];
      e[1] += step[1];
      e[2] += step[2];
    }
  }
}

}  // namespace softgpu
