#include <TestHelpers.hpp>
#include <render/Rasterizer.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

std::shared_ptr<Scene> viewportSpanningWireframeScene() {
  auto scene = cameraScene(75, .1f, 4);
  auto largeTriangle = triangle(
      {Vertex({-10, -10, -1}), Vertex({10, -10, -1}), Vertex({0, 10, -1})});
  unlit(largeTriangle);
  scene->addGraphicObj(largeTriangle, "spanning");
  return scene;
}

TEST(Rasterizer, ViewportSpanningWireframeAddsNoClippingEdges) {
  auto scene = viewportSpanningWireframeScene();
  for (bool simd : {false, true}) {
    SCOPED_TRACE(simd ? "SIMD" : "scalar");
    TraditionalRasterizer rasterizer(71, 53);
    rasterizer.setSIMD(simd);
    rasterizer.addScene(scene);
    rasterizer.draw(Primitive::LINES);

    for (float depth : rasterizer.depth()) {
      ASSERT_FALSE(std::isfinite(depth))
          << "wireframe gained an artificial clipping edge";
    }
  }
}

} // namespace
