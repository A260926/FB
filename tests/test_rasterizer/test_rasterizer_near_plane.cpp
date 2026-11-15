#include <TestHelpers.hpp>
#include <render/Rasterizer.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(Rasterizer, NearPlaneOverlapKeepsVisiblePixelsAndValidDepth) {
  auto scene = cameraScene(75, .1f, 4);
  auto nearTriangle = triangle(
      {Vertex({0, .8f, -.02f}), Vertex({-2, -.8f, -2}), Vertex({2, -.8f, -2})});
  unlit(nearTriangle);
  scene->addGraphicObj(nearTriangle, "near");

  TraditionalRasterizer rasterizer(71, 53);
  rasterizer.addScene(scene);
  rasterizer.draw();

  int coveredPixels = 0;
  for (float depth : rasterizer.depth()) {
    if (!std::isfinite(depth)) {
      continue;
    }
    ASSERT_GE(depth, -1e-6f) << "depth before the near clip range";
    ASSERT_LE(depth, 1 + 1e-6f) << "depth after the far clip range";
    ++coveredPixels;
  }
  ASSERT_GT(coveredPixels, 200) << "near-plane overlap lost visible pixels";
}

} // namespace
