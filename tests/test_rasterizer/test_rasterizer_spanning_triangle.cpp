#include <TestHelpers.hpp>
#include <render/Rasterizer.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

std::shared_ptr<Scene> viewportSpanningScene() {
  auto scene = cameraScene(75, .1f, 4);
  auto largeTriangle = triangle(
      {Vertex({-10, -10, -1}), Vertex({10, -10, -1}), Vertex({0, 10, -1})});
  unlit(largeTriangle);
  scene->addGraphicObj(largeTriangle, "spanning");
  return scene;
}

TEST(Rasterizer, ViewportSpanningTriangleIsNotFrustumCulled) {
  auto scene = viewportSpanningScene();
  for (bool simd : {false, true}) {
    SCOPED_TRACE(simd ? "SIMD" : "scalar");
    TraditionalRasterizer rasterizer(71, 53);
    rasterizer.setSIMD(simd);
    rasterizer.addScene(scene);
    rasterizer.draw();

    for (float depth : rasterizer.depth()) {
      ASSERT_TRUE(std::isfinite(depth))
          << "partially overlapping triangle was culled";
    }
  }
}

} // namespace
