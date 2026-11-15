// Clipping: unit checks on the polygon, then "clipping is invisible" renders.
#include <cmath>
#include <cstring>

#include "doctest.h"
#include "softgpu/renderer.hpp"

using namespace softgpu;

namespace {

struct P3 {
  Vec3 p;
};

// World position passed through as the varying so we can compare interpolation across renders.
struct PosVS {
  Mat4 mvp;
  VsOut<3> operator()(const P3& v) const {
    VsOut<3> o;
    o.pos = mvp * Vec4(v.p, 1.0f);
    o.attr[0] = v.p.x;
    o.attr[1] = v.p.y;
    o.attr[2] = v.p.z;
    return o;
  }
};

struct PosFS {
  bool operator()(const Fragment<3>& f, Vec4& c) const {
    c = Vec4(f.attr[0] * 0.5f + 0.5f, f.attr[1] * 0.5f + 0.5f, f.attr[2] * 0.1f + 0.5f, 1.0f);
    return true;
  }
};

struct FlatFS {
  bool operator()(const Fragment<3>&, Vec4& c) const {
    c = Vec4(1, 1, 1, 1);
    return true;
  }
};

size_t covered(const Framebuffer& fb) {
  size_t n = 0;
  for (size_t i = 0; i < fb.color.rgba.size(); i += 4) n += fb.color.rgba[i] != 0;
  return n;
}

}  // namespace

TEST_CASE("clip_triangle: inside untouched, outside rejected, near-plane cut lands on the plane") {
  VsOut<3> tri[3], out[kMaxClipVerts];
  tri[0].pos = Vec4(-0.5f, -0.5f, 0.0f, 1.0f);
  tri[1].pos = Vec4(0.5f, -0.5f, 0.0f, 1.0f);
  tri[2].pos = Vec4(0.0f, 0.5f, 0.0f, 1.0f);
  bool clipped = true;
  CHECK(clip_triangle<3>(tri, out, &clipped) == 3);
  CHECK_FALSE(clipped);

  for (auto& v : tri) v.pos.z = -3.0f;  // entirely behind the near plane (z < -w)
  CHECK(clip_triangle<3>(tri, out, &clipped) == 0);
  CHECK(clipped);

  // One vertex behind the near plane: quad with two new vertices exactly on z = -w.
  tri[0].pos = Vec4(-1.0f, -1.0f, -3.0f, 1.0f);
  tri[1].pos = Vec4(1.0f, -1.0f, 0.5f, 1.0f);
  tri[2].pos = Vec4(0.0f, 1.0f, 0.5f, 1.0f);
  for (auto& v : tri) v.attr[0] = v.pos.x;
  const int n = clip_triangle<3>(tri, out, &clipped);
  CHECK(n == 4);
  CHECK(clipped);
  int on_plane = 0;
  for (int i = 0; i < n; ++i) {
    CHECK(out[i].pos.z >= -out[i].pos.w - 1e-6f);
    CHECK(out[i].attr[0] == doctest::Approx(out[i].pos.x));  // varyings lerp with the position
    if (std::fabs(out[i].pos.z + out[i].pos.w) < 1e-6f) ++on_plane;
  }
  CHECK(on_plane == 2);

  // A triangle poking out of all four side planes at once still yields a convex polygon.
  tri[0].pos = Vec4(-4.0f, -4.0f, 0.0f, 1.0f);
  tri[1].pos = Vec4(4.0f, -4.0f, 0.0f, 1.0f);
  tri[2].pos = Vec4(0.0f, 6.0f, 0.0f, 1.0f);
  const int m = clip_triangle<3>(tri, out, &clipped);
  CHECK(m >= 4);
  CHECK(m <= kMaxClipVerts);
  for (int i = 0; i < m; ++i) {
    CHECK(std::fabs(out[i].pos.x) <= 1.0f + 1e-5f);
    CHECK(std::fabs(out[i].pos.y) <= 1.0f + 1e-5f);
  }
}

TEST_CASE("side-plane clipping is invisible: cropped render equals the same view rendered whole") {
  // The 100x100 view is the central quarter of the 200x200 view; both use exact scale factors so
  // every unclipped vertex snaps to the same 28.4 position, and the clipped ones must too.
  const P3 verts[] = {{{-1.5f, -1.0f, -2.0f}}, {{2.5f, -1.8f, -6.0f}}, {{0.4f, 2.2f, -3.0f}},
                      {{-1.0f, 1.2f, -1.5f}},  {{1.5f, 1.5f, -8.0f}},  {{0.0f, -1.8f, -2.5f}}};
  const uint32_t idx[] = {0, 1, 2, 3, 4, 5};
  const Mat4 proj = Mat4::perspective(kPi / 2, 1.0f, 0.5f, 50.0f);
  DrawState st;
  st.cull = CullMode::None;

  Renderer big(200, 200, Config{Mode::Immediate, 1, 32});
  big.clear(Vec4(0, 0, 0, 1));
  big.draw<3>(verts, 6, idx, 6, PosVS{proj}, PosFS{}, st);
  big.finish();

  Renderer small(100, 100, Config{Mode::Immediate, 1, 32});
  small.clear(Vec4(0, 0, 0, 1));
  small.draw<3>(verts, 6, idx, 6, PosVS{Mat4::scale({2, 2, 1}) * proj}, PosFS{}, st);
  small.finish();

  CHECK(small.stats().triangles_clipped == 2);
  CHECK(big.stats().triangles_clipped == 0);
  CHECK(small.stats().triangles_rasterized > 2);  // clipping fanned the polygon
  // A cut vertex snaps to the 28.4 grid independently of the original edge, so a sample within
  // 1/32 px of a clipped edge may flip and the interpolation plane of a sliver fan triangle
  // tilts by ~1e-4 in depth; that is the same tolerance a real GPU has and is why they clip
  // with a guard band. Interior colors must match to the LSB.
  auto on_edge = [&](int x, int y) {  // silhouette or intersection edge in the reference
    const uint8_t* c = big.fb().color.at(x + 50, y + 50);
    for (int dy = -1; dy <= 1; ++dy)
      for (int dx = -1; dx <= 1; ++dx) {
        const uint8_t* n = big.fb().color.at(x + 50 + dx, y + 50 + dy);
        for (int k = 0; k < 3; ++k)
          if (std::abs(int(n[k]) - int(c[k])) > 8) return true;
      }
    return false;
  };
  size_t mismatched = 0, drawn = 0, edge_flips = 0;
  for (int y = 0; y < 100; ++y)
    for (int x = 0; x < 100; ++x) {
      const uint8_t* a = small.fb().color.at(x, y);
      const uint8_t* b = big.fb().color.at(x + 50, y + 50);
      drawn += a[0] != 0;
      bool same = std::fabs(small.fb().depth_at(x, y) - big.fb().depth_at(x + 50, y + 50)) < 1e-3f;
      for (int c = 0; c < 3; ++c) same = same && std::abs(int(a[c]) - int(b[c])) <= 1;
      if (same) continue;
      if (on_edge(x, y))
        ++edge_flips;
      else
        ++mismatched;
    }
  CHECK(drawn > 4000);
  CHECK(mismatched == 0);
  CHECK(edge_flips <= 8);
}

TEST_CASE("near-plane clipping matches the analytic visible region of a ground strip") {
  // Strip on y = -0.25 from z = -10 to z = +2 (crossing the eye), x in [-0.25, 0.25].
  const P3 verts[] = {{{-0.25f, -0.25f, -10.0f}}, {{0.25f, -0.25f, -10.0f}}, {{0.25f, -0.25f, 2.0f}},
                      {{-0.25f, -0.25f, 2.0f}}};
  const uint32_t idx[] = {0, 1, 2, 0, 2, 3};
  const int W = 160, H = 160;
  const float znear = 0.5f;
  const Mat4 proj = Mat4::perspective(kPi / 2, 1.0f, znear, 100.0f);
  Renderer r(W, H, Config{Mode::Immediate, 1, 32});
  r.clear(Vec4(0, 0, 0, 1));
  DrawState st;
  st.cull = CullMode::None;
  r.draw<3>(verts, 4, idx, 6, PosVS{proj}, FlatFS{}, st);
  r.finish();
  CHECK(r.stats().triangles_in == 2);
  CHECK(r.stats().triangles_clipped == 2);
  CHECK(r.stats().triangles_rejected == 0);

  size_t drawn = 0, expected = 0, mismatched = 0;
  for (int y = 0; y < H; ++y) {
    const float ndc_y = 1.0f - 2.0f * (y + 0.5f) / H;
    const float d = ndc_y < 0 ? 0.25f / -ndc_y : 1e9f;  // eye-space distance of the strip at this row
    for (int x = 0; x < W; ++x) {
      const float ndc_x = 2.0f * (x + 0.5f) / W - 1.0f;
      const bool inside = d >= znear && d <= 10.0f && std::fabs(ndc_x) * d <= 0.25f;
      const bool hit = r.fb().color.at(x, y)[0] != 0;
      drawn += hit;
      expected += inside;
      // Tolerate the pixel row/column that straddles a boundary.
      const bool near_edge = std::fabs(std::fabs(ndc_x) * d - 0.25f) < 0.25f * 2.0f / W * d * 1.5f ||
                             std::fabs(d - znear) < 0.02f || std::fabs(d - 10.0f) < 0.5f;
      if (hit != inside && !near_edge) ++mismatched;
      if (hit) {
        CHECK(r.fb().depth_at(x, y) >= 0.0f);
        CHECK(r.fb().depth_at(x, y) <= 1.0f);
      }
    }
  }
  CHECK(mismatched == 0);
  CHECK(drawn > 0);
  CHECK(std::abs(int(drawn) - int(expected)) < W);
  // Rows below the near-plane cut (d < znear) are empty; the cut row itself is drawn.
  const int cut_row = static_cast<int>((1.0f - (-0.25f / znear)) * 0.5f * H);  // ndc_y = -0.5 -> y = 120
  CHECK(r.fb().color.at(W / 2, cut_row + 2)[0] == 0);
  CHECK(r.fb().color.at(W / 2, cut_row - 2)[0] != 0);
  // Depth increases going up the column (farther away).
  float prev = -1.0f;
  for (int y = cut_row - 2; y > H / 2 + 6; y -= 4) {
    const float dz = r.fb().depth_at(W / 2, y);
    CHECK(dz > prev);
    prev = dz;
  }
}

TEST_CASE("fully outside triangles are rejected before setup, behind-camera never rasterizes") {
  const P3 verts[] = {{{-1, -1, 5}}, {{1, -1, 5}}, {{0, 1, 5}}, {{-1, -1, -5}}, {{1, -1, -5}}, {{0, 1, -5}}};
  const uint32_t idx[] = {0, 1, 2, 3, 4, 5};
  const Mat4 proj = Mat4::perspective(kPi / 2, 1.0f, 0.5f, 100.0f);
  Renderer r(64, 64, Config{Mode::Immediate, 1, 32});
  r.clear(Vec4(0, 0, 0, 1));
  DrawState st;
  st.cull = CullMode::None;
  r.draw<3>(verts, 6, idx, 6, PosVS{proj}, FlatFS{}, st);
  r.finish();
  CHECK(r.stats().triangles_rejected == 1);
  CHECK(r.stats().triangles_rasterized == 1);
  CHECK(covered(r.fb()) > 50);
}
