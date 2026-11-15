// Perspective-correct interpolation against the analytic ray/plane answer, and derivatives
// against finite differences of neighbouring pixels.
#include <cmath>

#include "doctest.h"
#include "softgpu/renderer.hpp"

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

// Captures raw varyings + derivatives per pixel instead of a color.
struct CaptureFS {
  std::vector<float>* u;
  std::vector<float>* v;
  std::vector<float>* dudy;
  std::vector<float>* dvdx;
  int w;
  bool operator()(const Fragment<2>& f, Vec4& c) const {
    (*u)[f.y * w + f.x] = f.attr[0];
    (*v)[f.y * w + f.x] = f.attr[1];
    (*dudy)[f.y * w + f.x] = f.ddy[0];
    (*dvdx)[f.y * w + f.x] = f.ddx[1];
    c = Vec4(1, 1, 1, 1);
    return true;
  }
};

}  // namespace

TEST_CASE("perspective-correct varyings match the analytic ground-plane parameter, not the affine one") {
  // Ground plane y = -0.5 from z = -1 (u = 0) to z = -10 (u = 1), x in [-10, 10] (v = x).
  const V verts[] = {{{-10, -0.5f, -1}, 0, -10}, {{10, -0.5f, -1}, 0, 10}, {{10, -0.5f, -10}, 1, 10},
                     {{-10, -0.5f, -10}, 1, -10}};
  const uint32_t idx[] = {0, 1, 2, 0, 2, 3};
  const int W = 160, H = 160;
  const Mat4 proj = Mat4::perspective(kPi / 2, 1.0f, 0.5f, 100.0f);
  std::vector<float> u(W * H, -1.0f), v(W * H, 0.0f), dudy(W * H, 0.0f), dvdx(W * H, 0.0f);
  Renderer r(W, H, Config{Mode::Immediate, 1, 32});
  r.clear(Vec4(0, 0, 0, 1));
  DrawState st;
  st.cull = CullMode::None;
  r.draw<2>(verts, 4, idx, 6, UvVS{proj}, CaptureFS{&u, &v, &dudy, &dvdx, W}, st);
  r.finish();

  // Screen rows of the near (z=-1) and far (z=-10) edges, for the affine (wrong) prediction.
  const float y_near = (0.5f - (-0.5f / 1.0f) * 0.5f) * H;   // ndc_y = -0.5
  const float y_far = (0.5f - (-0.5f / 10.0f) * 0.5f) * H;   // ndc_y = -0.05
  int checked = 0;
  float max_err = 0, max_affine_gap = 0;
  for (int y = static_cast<int>(y_far) + 2; y < static_cast<int>(y_near) - 1; ++y) {
    const float ndc_y = 1.0f - 2.0f * (y + 0.5f) / H;
    const float d = 0.5f / -ndc_y;  // depth of the plane along this row's ray
    const float u_true = (d - 1.0f) / 9.0f;
    const float u_affine = (y_near - (y + 0.5f)) / (y_near - y_far);
    for (int x = 8; x < W - 8; x += 8) {
      const float got = u[y * W + x];
      REQUIRE(got >= 0.0f);
      max_err = std::max(max_err, std::fabs(got - u_true));
      max_affine_gap = std::max(max_affine_gap, std::fabs(u_affine - u_true));
      // v is world x along the ray: x = ndc_x * d for fov 90.
      const float ndc_x = 2.0f * (x + 0.5f) / W - 1.0f;
      CHECK(v[y * W + x] == doctest::Approx(ndc_x * d).epsilon(0.01));
      ++checked;
    }
    // Analytic derivatives vs central differences (one pixel apart) of the captured values.
    const int x = W / 2;
    if (y > static_cast<int>(y_far) + 3 && y < static_cast<int>(y_near) - 2) {
      const float fd = (u[(y + 1) * W + x] - u[(y - 1) * W + x]) * 0.5f;
      CHECK(dudy[y * W + x] == doctest::Approx(fd).epsilon(0.03));
      const float fdx = (v[y * W + x + 1] - v[y * W + x - 1]) * 0.5f;
      CHECK(dvdx[y * W + x] == doctest::Approx(fdx).epsilon(0.01));
    }
  }
  CHECK(checked > 100);
  CHECK(max_err < 3e-3f);  // residual is 28.4 vertex snapping, ~1/(32 px) of the u range per row
  CHECK(max_affine_gap > 0.1f);  // the affine answer is far off, so the test discriminates
}
