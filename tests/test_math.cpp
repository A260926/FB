#include <cstdio>

#include "doctest.h"
#include "softgpu/image.hpp"
#include "softgpu/math.hpp"

using namespace softgpu;

TEST_CASE("perspective + look_at map the near/far planes and a centred point where expected") {
  const Mat4 proj = Mat4::perspective(kPi / 2, 1.0f, 1.0f, 10.0f);
  const Mat4 view = Mat4::look_at({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
  const Mat4 pv = proj * view;
  // Point on the near plane (z = 4 in world, 1 in front of the eye) -> ndc z = -1.
  Vec3 n = pv.transform_point({0, 0, 4});
  CHECK(n.z == doctest::Approx(-1.0f));
  Vec3 f = pv.transform_point({0, 0, -5});
  CHECK(f.z == doctest::Approx(1.0f));
  // fov 90 degrees: at distance 5 the frustum half-width is 5, so x = 5 is the right edge.
  Vec3 r = pv.transform_point({5, 0, 0});
  CHECK(r.x == doctest::Approx(1.0f));
  CHECK(r.y == doctest::Approx(0.0f));
  // Matrix product order: (A * B) * v == A * (B * v).
  const Mat4 a = Mat4::rotate_y(0.7f) * Mat4::translate({1, 2, 3});
  const Vec4 v(0.5f, -1.0f, 2.0f, 1.0f);
  const Vec4 lhs = (a * Mat4::rotate_x(0.3f)) * v;
  const Vec4 rhs = a * (Mat4::rotate_x(0.3f) * v);
  CHECK(lhs.x == doctest::Approx(rhs.x));
  CHECK(lhs.y == doctest::Approx(rhs.y));
  CHECK(lhs.z == doctest::Approx(rhs.z));
  // Rotation keeps normals unit length.
  CHECK(length(Mat4::rotate_z(1.1f).transform_dir(normalize({1, 1, 1}))) == doctest::Approx(1.0f));
}

TEST_CASE("ppm/png writers round-trip pixels") {
  Image img(3, 2);
  img.set(0, 0, 255, 0, 0);
  img.set(1, 0, 0, 255, 0);
  img.set(2, 0, 0, 0, 255);
  img.set(0, 1, 1, 2, 3);
  img.set(1, 1, 4, 5, 6);
  img.set(2, 1, 7, 8, 9);
  const std::string p = "softgpu_test_rt.ppm";
  REQUIRE(write_ppm(p, img));
  Image back;
  REQUIRE(read_ppm(p, back));
  REQUIRE(back.width == 3);
  REQUIRE(back.height == 2);
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 3; ++x)
      for (int c = 0; c < 3; ++c) CHECK(back.at(x, y)[c] == img.at(x, y)[c]);
  std::remove(p.c_str());

  const std::string q = "softgpu_test_rt.png";
  REQUIRE(write_png(q, img));
  std::FILE* f = std::fopen(q.c_str(), "rb");
  REQUIRE(f);
  unsigned char sig[8];
  REQUIRE(std::fread(sig, 1, 8, f) == 8);
  std::fclose(f);
  CHECK(sig[1] == 'P');
  CHECK(sig[2] == 'N');
  CHECK(sig[3] == 'G');
  std::remove(q.c_str());
}
