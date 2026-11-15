// Fill rule: a mesh of triangles sharing edges covers every pixel exactly once.
#include <random>
#include <vector>

#include "doctest.h"
#include "softgpu/raster.hpp"

using namespace softgpu;

namespace {

constexpr int W = 128, H = 96;

struct Coverage {
  std::vector<int> hits = std::vector<int>(W * H, 0);
  int& at(int x, int y) { return hits[y * W + x]; }
  size_t total() const {
    size_t n = 0;
    for (int h : hits) n += h;
    return n;
  }
  int max_hits() const {
    int m = 0;
    for (int h : hits) m = std::max(m, h);
    return m;
  }
};

ScreenVertex<0> sv(float x, float y) {
  ScreenVertex<0> v;
  v.x = x;
  v.y = y;
  return v;
}

// Draws a triangle with the fill rule and returns false if it was rejected.
bool draw(Coverage& c, ScreenVertex<0> a, ScreenVertex<0> b, ScreenVertex<0> d, CullMode cull = CullMode::None) {
  const ScreenVertex<0> v[3] = {a, b, d};
  SetupTri<0> t;
  if (setup_triangle(v, cull, W, H, t) != Setup::Ok) return false;
  rasterize(t, t.bbox, [&](int px, int py, float, float, float) { c.at(px, py)++; });
  return true;
}

}  // namespace

TEST_CASE("shared edges through pixel centres rasterize exactly once, boundary follows top-left rule") {
  // Square whose corners sit on pixel centres, split by a diagonal that passes through centres.
  Coverage c;
  REQUIRE(draw(c, sv(10.5f, 10.5f), sv(50.5f, 10.5f), sv(50.5f, 50.5f)));
  REQUIRE(draw(c, sv(10.5f, 10.5f), sv(50.5f, 50.5f), sv(10.5f, 50.5f)));
  CHECK(c.max_hits() == 1);
  CHECK(c.total() == 40u * 40u);  // top/left edges in, bottom/right out
  CHECK(c.at(10, 10) == 1);
  CHECK(c.at(50, 10) == 0);
  CHECK(c.at(10, 50) == 0);
  CHECK(c.at(49, 49) == 1);

  // Fan around a centre pixel: horizontal, vertical and diagonal shared edges through centres.
  Coverage f;
  const ScreenVertex<0> ctr = sv(60.5f, 40.5f);
  const ScreenVertex<0> ring[8] = {sv(30.5f, 20.5f), sv(60.5f, 20.5f), sv(90.5f, 20.5f), sv(90.5f, 40.5f),
                                   sv(90.5f, 60.5f), sv(60.5f, 60.5f), sv(30.5f, 60.5f), sv(30.5f, 40.5f)};
  for (int i = 0; i < 8; ++i) REQUIRE(draw(f, ctr, ring[i], ring[(i + 1) % 8]));
  CHECK(f.max_hits() == 1);
  CHECK(f.total() == 60u * 40u);
  CHECK(f.at(60, 40) == 1);  // the shared centre vertex is drawn once
}

TEST_CASE("random jittered grid meshes: no cracks, no double hits, winding independent") {
  std::mt19937 rng(11);
  for (int iter = 0; iter < 20; ++iter) {
    const int gx = 2 + iter % 5, gy = 2 + (iter * 7) % 5;
    const int x0 = 5, y0 = 5, x1 = 5 + 8 * gx * 2, y1 = 5 + 6 * gy * 2;
    // Vertices on a grid; interior ones jittered by whole pixels so many edges pass through centres.
    std::vector<std::vector<Vec2>> p(gy + 1, std::vector<Vec2>(gx + 1));
    for (int j = 0; j <= gy; ++j)
      for (int i = 0; i <= gx; ++i) {
        float x = x0 + (x1 - x0) * static_cast<float>(i) / gx + 0.5f;
        float y = y0 + (y1 - y0) * static_cast<float>(j) / gy + 0.5f;
        if (i > 0 && i < gx && j > 0 && j < gy) {
          x += static_cast<float>(static_cast<int>(rng() % 7) - 3);
          y += static_cast<float>(static_cast<int>(rng() % 5) - 2);
          if (rng() % 2) x += 0.25f;  // and some off-centre ones
        }
        p[j][i] = {x, y};
      }
    Coverage c;
    for (int j = 0; j < gy; ++j)
      for (int i = 0; i < gx; ++i) {
        const Vec2 a = p[j][i], b = p[j][i + 1], d = p[j + 1][i + 1], e = p[j + 1][i];
        const bool diag = rng() % 2;
        // Random winding per triangle: coverage must not depend on it with culling off.
        auto tri = [&](Vec2 u, Vec2 v, Vec2 w) {
          if (rng() % 2) std::swap(v, w);
          draw(c, sv(u.x, u.y), sv(v.x, v.y), sv(w.x, w.y));
        };
        if (diag) {
          tri(a, b, d);
          tri(a, d, e);
        } else {
          tri(a, b, e);
          tri(b, d, e);
        }
      }
    CHECK(c.max_hits() == 1);
    CHECK(c.total() == static_cast<size_t>(x1 - x0) * static_cast<size_t>(y1 - y0));
  }
}

TEST_CASE("degenerate, culled, sliver and off-screen triangles") {
  Coverage c;
  CHECK_FALSE(draw(c, sv(1, 1), sv(5, 5), sv(9, 9)));          // collinear
  CHECK_FALSE(draw(c, sv(1, 1), sv(1.01f, 1.02f), sv(1, 1.01f)));  // snaps to zero area
  // Clockwise on screen (y down) is counter-clockwise with y up: front-facing.
  CHECK_FALSE(draw(c, sv(10, 10), sv(10, 30), sv(30, 10), CullMode::Front));
  CHECK(draw(c, sv(10, 10), sv(10, 30), sv(30, 10), CullMode::Back));
  CHECK_FALSE(draw(c, sv(10, 10), sv(30, 10), sv(10, 30), CullMode::Back));
  CHECK(c.total() > 0);
  // Sliver between two pixel-centre columns: no sample inside, no crash, no hits.
  Coverage s;
  CHECK(draw(s, sv(20.6f, 5), sv(20.9f, 5), sv(20.7f, 60)));
  CHECK(s.total() == 0);
  // Mostly off-screen: bbox is clamped, only on-screen pixels are touched.
  Coverage o;
  CHECK(draw(o, sv(-100, -100), sv(300, -100), sv(-100, 300)));
  CHECK(o.max_hits() == 1);
  CHECK(o.total() > 0);
  CHECK(o.total() <= static_cast<size_t>(W) * H);
}
