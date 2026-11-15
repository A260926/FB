// The tiled, multithreaded path must be a pure scheduling change: every scene renders
// byte-identical color and depth buffers and identical fragment counters against the
// immediate single-threaded reference.
#include <memory>

#include "doctest.h"
#include "softgpu/scenes.hpp"

using namespace softgpu;

namespace {

struct Result {
  std::vector<uint8_t> color;
  std::vector<float> depth;
  RasterCounters counters;
  uint64_t rasterized, bins;
};

Result render(const SceneOptions& opt, Mode mode, int threads, int tile) {
  // 322x198 is not a multiple of any tile size, so edge tiles are partial.
  auto r = std::make_unique<Renderer>(322, 198, Config{mode, threads, tile});
  REQUIRE(render_scene(*r, opt));
  return {r->fb().color.rgba, r->fb().depth, r->stats().raster, r->stats().triangles_rasterized,
          r->stats().tile_bins};
}

}  // namespace

TEST_CASE("tiled multithreaded rendering is byte-identical to the immediate reference for every scene") {
  for (const std::string& name : scene_names()) {
    SceneOptions opt;
    opt.name = name;
    opt.count = 600;
    CAPTURE(name);
    const Result ref = render(opt, Mode::Immediate, 1, 32);
    REQUIRE(ref.counters.fragments_shaded > 1000);
    REQUIRE(ref.rasterized > 0);
    const int configs[][2] = {{1, 32}, {8, 32}, {8, 16}, {3, 64}, {8, 32}};
    for (const auto& c : configs) {
      CAPTURE(c[0]);
      CAPTURE(c[1]);
      const Result t = render(opt, Mode::Tiled, c[0], c[1]);
      CHECK(t.color == ref.color);
      CHECK(t.depth == ref.depth);
      CHECK(t.counters.fragments_in == ref.counters.fragments_in);
      CHECK(t.counters.early_z_rejected == ref.counters.early_z_rejected);
      CHECK(t.counters.fragments_shaded == ref.counters.fragments_shaded);
      CHECK(t.counters.fragments_blended == ref.counters.fragments_blended);
      CHECK(t.rasterized == ref.rasterized);
      CHECK(t.bins >= t.rasterized);  // every rasterized triangle lands in at least one tile
    }
  }
}

TEST_CASE("a triangle spanning many tiles is drawn once per pixel and binned once per tile") {
  struct P {
    Vec3 p;
  };
  struct VS {
    VsOut<0> operator()(const P& v) const {
      VsOut<0> o;
      o.pos = Vec4(v.p, 1.0f);
      return o;
    }
  };
  struct Count {
    std::vector<int>* hits;
    int w;
    bool operator()(const Fragment<0>& f, Vec4& c) const {
      (*hits)[f.y * w + f.x]++;
      c = Vec4(1, 1, 1, 1);
      return true;
    }
  };
  const P verts[] = {{{-0.9f, -0.9f, 0}}, {{0.9f, -0.9f, 0}}, {{0.0f, 0.9f, 0}}};
  const uint32_t idx[] = {0, 1, 2};
  std::vector<int> hits(100 * 80, 0);
  Renderer r(100, 80, Config{Mode::Tiled, 4, 16});
  r.clear(Vec4(0, 0, 0, 1));
  DrawState st;
  st.cull = CullMode::None;
  r.draw<0>(verts, 3, idx, 3, VS{}, Count{&hits, 100}, st);
  CHECK(r.stats().tile_bins == 6u * 5u);  // bbox x 5..94, y 4..75 at 16px tiles = 6x5 tiles
  r.finish();
  int max_hit = 0;
  size_t total = 0;
  for (int h : hits) {
    max_hit = std::max(max_hit, h);
    total += h;
  }
  CHECK(max_hit == 1);
  CHECK(total == r.stats().raster.fragments_shaded);
  CHECK(total > 3000);
}
