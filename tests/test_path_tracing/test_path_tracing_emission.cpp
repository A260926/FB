#include <TestHelpers.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(PathTracing, EmissionDeterminismAndCameraNearPlane) {
  auto s = cameraScene();
  auto m = mat(glm::vec3(0));
  m->emission = {4, 2, 1};
  s->addGraphicObj(plane(-2, 10, m), "light");
  PathTracing pt(11, 7, 4);
  pt.configure(settings(4, 1));
  pt.addScene(s);
  pt.draw();
  for (auto p : pt.pixels()) {
    expectNear(p, {4, 2, 1});
  }
  auto first = pt.pixels();
  pt.draw();
  ASSERT_TRUE(first == pt.pixels()) << "PT not reproducible";
  auto clipped = cameraScene(60, .1f, 100, {.1, .2, .3});
  clipped->addGraphicObj(plane(-.01f, 10, m), "tooNear");
  PathTracing nearPT(3, 3, 4);
  nearPT.addScene(clipped);
  nearPT.draw();
  for (auto p : nearPT.pixels()) {
    expectNear(p, {.1, .2, .3});
  }
}

} // namespace
