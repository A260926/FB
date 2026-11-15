// Depth test / early-Z / depth write / blending on two overlapping triangles.
#include <cstdio>
#include <cstring>
#include <memory>

#include "doctest.h"
#include "softgpu/renderer.hpp"

using namespace softgpu;

namespace {

struct P {
  Vec3 p;
};

struct PassVS {
  Mat4 mvp;
  VsOut<0> operator()(const P& v) const {
    VsOut<0> o;
    o.pos = mvp * Vec4(v.p, 1.0f);
    return o;
  }
};

struct ColorFS {
  Vec4 color;
  bool operator()(const Fragment<0>&, Vec4& c) const {
    c = color;
    return true;
  }
};

const Mat4 kProj = Mat4::perspective(kPi / 2, 1.0f, 0.5f, 100.0f);
// Near triangle at z = -2 (left half of the view), far one at z = -5 (right half); they overlap
// in the middle band.
const P kNear[] = {{{-1.6f, -1.2f, -2}}, {{0.6f, -1.2f, -2}}, {{-0.5f, 1.4f, -2}}};
const P kFar[] = {{{-1.5f, -3.0f, -5}}, {{4.0f, -3.0f, -5}}, {{1.25f, 3.5f, -5}}};
const uint32_t kIdx[] = {0, 1, 2};

std::unique_ptr<Renderer> make() {
  auto r = std::make_unique<Renderer>(64, 64, Config{Mode::Immediate, 1, 32});
  r->clear(Vec4(0, 0, 0, 1));
  return r;
}

size_t count_color(const Framebuffer& fb, uint8_t r, uint8_t g, uint8_t b) {
  size_t n = 0;
  for (size_t i = 0; i < fb.color.rgba.size(); i += 4)
    n += fb.color.rgba[i] == r && fb.color.rgba[i + 1] == g && fb.color.rgba[i + 2] == b;
  return n;
}

}  // namespace

TEST_CASE("depth ordering is draw-order independent and early-Z counts the hidden fragments") {
  DrawState st;
  st.cull = CullMode::None;
  const ColorFS red{Vec4(1, 0, 0, 1)}, blue{Vec4(0, 0, 1, 1)};

  auto a = make();  // near first: far fragments behind it fail the early test
  a->draw<0>(kNear, 3, kIdx, 3, PassVS{kProj}, red, st);
  const uint64_t near_frags = a->stats().raster.fragments_shaded;
  a->draw<0>(kFar, 3, kIdx, 3, PassVS{kProj}, blue, st);
  a->finish();

  auto b = make();  // far first: everything shades, near overwrites
  b->draw<0>(kFar, 3, kIdx, 3, PassVS{kProj}, blue, st);
  b->draw<0>(kNear, 3, kIdx, 3, PassVS{kProj}, red, st);
  b->finish();

  CHECK(a->fb().color.rgba == b->fb().color.rgba);
  CHECK(a->fb().depth == b->fb().depth);
  CHECK(b->stats().raster.early_z_rejected == 0);
  CHECK(a->stats().raster.early_z_rejected > 100);
  CHECK(a->stats().raster.fragments_shaded + a->stats().raster.early_z_rejected == b->stats().raster.fragments_shaded);
  CHECK(count_color(a->fb(), 255, 0, 0) == near_frags);
  CHECK(count_color(a->fb(), 0, 0, 255) > 100);
  // Every red pixel is nearer than every blue pixel.
  float red_max = 0, blue_min = 1;
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 64; ++x) {
      const uint8_t* c = a->fb().color.at(x, y);
      if (c[0] == 255) red_max = std::max(red_max, a->fb().depth_at(x, y));
      if (c[2] == 255) blue_min = std::min(blue_min, a->fb().depth_at(x, y));
    }
  CHECK(red_max < blue_min);

  // Grayscale dump of the depth buffer is writable and encodes near as dark.
  REQUIRE(write_pgm("softgpu_test_depth.pgm", 64, 64, a->fb().depth.data(), 0.0f, 1.0f));
  std::remove("softgpu_test_depth.pgm");
}

TEST_CASE("depth write off, depth test off, and src-over blending") {
  const ColorFS red{Vec4(1, 0, 0, 1)}, blue{Vec4(0, 0, 1, 1)};
  DrawState st;
  st.cull = CullMode::None;

  // No depth write for the near triangle: the far one draws over it.
  auto a = make();
  st.depth_write = false;
  a->draw<0>(kNear, 3, kIdx, 3, PassVS{kProj}, red, st);
  st.depth_write = true;
  a->draw<0>(kFar, 3, kIdx, 3, PassVS{kProj}, blue, st);
  a->finish();
  CHECK(a->stats().raster.early_z_rejected == 0);
  CHECK(a->fb().depth_at(2, 40) == 1.0f);  // red-only region left no depth behind

  // Depth test off: later draw wins regardless of z.
  auto b = make();
  b->draw<0>(kNear, 3, kIdx, 3, PassVS{kProj}, red, st);
  st.depth_test = false;
  b->draw<0>(kFar, 3, kIdx, 3, PassVS{kProj}, blue, st);
  b->finish();
  st.depth_test = true;
  CHECK(b->stats().raster.early_z_rejected == 0);
  CHECK(a->fb().color.rgba == b->fb().color.rgba);

  // 50% red over opaque blue -> (128, 0, 128), and the blend counter sees exactly the overlap.
  auto c = make();
  c->draw<0>(kFar, 3, kIdx, 3, PassVS{kProj}, blue, st);
  st.blend = true;
  st.depth_test = false;
  c->draw<0>(kNear, 3, kIdx, 3, PassVS{kProj}, ColorFS{Vec4(1, 0, 0, 0.5f)}, st);
  c->finish();
  const size_t overlap = count_color(c->fb(), 128, 0, 128);
  CHECK(overlap > 100);
  CHECK(count_color(c->fb(), 128, 0, 0) > 100);  // red over black background
  CHECK(c->stats().raster.fragments_blended == count_color(c->fb(), 128, 0, 128) + count_color(c->fb(), 128, 0, 0));
}
