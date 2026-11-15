// Texture sampling: exact texel fetch, bilinear weights, mip chain, lod selection, and a
// textured quad rendered at 1:1 reproducing the texture.
#include <cmath>

#include "doctest.h"
#include "softgpu/renderer.hpp"
#include "softgpu/texture.hpp"

using namespace softgpu;

namespace {

struct V {
  Vec3 p;
  float u, v;
};

struct UvVS {
  Mat4 mvp;
  VsOut<2> operator()(const V& in) const {
    VsOut<2> o;
    o.pos = mvp * Vec4(in.p, 1.0f);
    o.attr[0] = in.u;
    o.attr[1] = in.v;
    return o;
  }
};

struct TexFS {
  const Texture* tex;
  Filter filter;
  bool operator()(const Fragment<2>& f, Vec4& c) const {
    c = tex->sample_grad({f.attr[0], f.attr[1]}, {f.ddx[0], f.ddx[1]}, {f.ddy[0], f.ddy[1]}, filter);
    return true;
  }
};

}  // namespace

TEST_CASE("nearest and bilinear fetch, mip chain averages, lod from derivatives") {
  // 2x2: red, green / blue, white.
  const std::vector<uint8_t> px = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
  Texture t(2, 2, px);
  REQUIRE(t.levels() == 2);
  CHECK(t.level(1).width == 1);
  CHECK(t.level(1).height == 1);
  const Vec4 avg = t.fetch(1, 0, 0);
  CHECK(avg.x == doctest::Approx(0.5f).epsilon(0.01));
  CHECK(avg.y == doctest::Approx(0.5f).epsilon(0.01));
  CHECK(avg.z == doctest::Approx(0.5f).epsilon(0.01));

  CHECK(t.sample_nearest(0, {0.25f, 0.25f}).x == 1.0f);
  CHECK(t.sample_nearest(0, {0.75f, 0.25f}).y == 1.0f);
  CHECK(t.sample_nearest(0, {0.25f, 0.75f}).z == 1.0f);
  CHECK(t.sample_nearest(0, {1.25f, 0.25f}).x == 1.0f);   // repeat
  CHECK(t.sample_nearest(0, {-0.25f, 0.25f}).y == 1.0f);  // repeat, negative side
  CHECK(t.sample_nearest(0, {1.25f, 0.25f}, Wrap::Clamp).y == 1.0f);

  // At a texel centre bilinear == nearest; at the texture centre it is the 4-texel average.
  const Vec4 c = t.sample_bilinear(0, {0.25f, 0.25f});
  CHECK(c.x == 1.0f);
  CHECK(c.y == 0.0f);
  const Vec4 mid = t.sample_bilinear(0, {0.5f, 0.5f});
  CHECK(mid.x == doctest::Approx(0.5f));
  CHECK(mid.y == doctest::Approx(0.5f));
  CHECK(mid.z == doctest::Approx(0.5f));
  // Halfway between red and green horizontally.
  const Vec4 rg = t.sample_bilinear(0, {0.5f, 0.25f});
  CHECK(rg.x == doctest::Approx(0.5f));
  CHECK(rg.y == doctest::Approx(0.5f));
  CHECK(rg.z == 0.0f);

  Texture big = Texture::checkerboard(64, 32, 8, Vec4(1, 1, 1, 1), Vec4(0, 0, 0, 1));
  CHECK(big.levels() == 7);  // 64x32 -> 32x16 -> ... -> 2x1 -> 1x1
  CHECK(big.lod({1.0f / 64, 0}, {0, 1.0f / 32}) == doctest::Approx(0.0f));  // one texel per pixel
  CHECK(big.lod({2.0f / 64, 0}, {0, 1.0f / 32}) == doctest::Approx(1.0f));  // two texels per pixel on x
  CHECK(big.lod({0, 0}, {0, 8.0f / 32}) == doctest::Approx(3.0f));
  // Minified far enough, every filter converges on the average grey of the checkerboard.
  for (Filter f : {Filter::Nearest, Filter::Bilinear, Filter::Trilinear}) {
    const Vec4 g = big.sample_grad({0.3f, 0.7f}, {1.0f, 0}, {0, 1.0f}, f);
    CHECK(g.x == doctest::Approx(0.5f).epsilon(0.02));
  }
  // Magnified, nearest returns pure texels.
  const Vec4 pure = big.sample_grad({0.01f, 0.01f}, {0.001f, 0}, {0, 0.001f}, Filter::Nearest);
  CHECK(pure.x == 1.0f);
  // Trilinear between levels 0 and 1 at lod 0.5 is a blend of both.
  const Vec4 tri = big.sample(Vec2{0.5f / 64, 0.5f / 32}, 0.5f, Filter::Trilinear);
  CHECK(tri.x > 0.5f);
  CHECK(tri.x < 1.0f);
}

TEST_CASE("a screen-aligned quad at 1:1 texel scale reproduces the texture with nearest and bilinear") {
  const int W = 64, H = 64;
  Texture tex = Texture::checkerboard(W, H, 4, Vec4(1, 0.5f, 0, 1), Vec4(0, 0.25f, 1, 1));
  // Orthographic quad exactly covering the viewport; v flips because texture rows go down.
  const V verts[] = {{{-1, 1, 0}, 0, 0}, {{1, 1, 0}, 1, 0}, {{1, -1, 0}, 1, 1}, {{-1, -1, 0}, 0, 1}};
  const uint32_t idx[] = {0, 2, 1, 0, 3, 2};
  for (Filter f : {Filter::Nearest, Filter::Bilinear, Filter::Trilinear}) {
    Renderer r(W, H, Config{Mode::Immediate, 1, 32});
    r.clear(Vec4(0, 0, 0, 1));
    DrawState st;
    st.cull = CullMode::None;
    r.draw<2>(verts, 4, idx, 6, UvVS{Mat4::identity()}, TexFS{&tex, f}, st);
    r.finish();
    CHECK(r.stats().raster.fragments_shaded == static_cast<uint64_t>(W) * H);
    int wrong = 0;
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        const uint8_t* got = r.fb().color.at(x, y);
        const uint8_t* want = &tex.level(0).rgba[(static_cast<size_t>(y) * W + x) * 4];
        for (int c = 0; c < 3; ++c)
          if (std::abs(int(got[c]) - int(want[c])) > 1) ++wrong;
      }
    CHECK(wrong == 0);
  }
}
