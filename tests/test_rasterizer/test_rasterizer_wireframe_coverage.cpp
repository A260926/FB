#include "RasterizerTestScenes.hpp"
#include <render/Rasterizer.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::RasterizerTestScenes;

namespace {

TEST(Rasterizer, WireframeCoversOnlyTriangleEdges) {
  auto [scene, triangle] = slantedTexturedTriangle();
  TraditionalRasterizer rasterizer(101, 79);
  rasterizer.addScene(scene);
  rasterizer.draw();

  int filledPixels = 0;
  for (float depth : rasterizer.depth()) {
    filledPixels += std::isfinite(depth);
  }

  rasterizer.draw(Primitive::LINES);
  int wireframePixels = 0;
  for (float depth : rasterizer.depth()) {
    wireframePixels += std::isfinite(depth);
  }

  ASSERT_GT(wireframePixels, 10) << "wireframe drew too few edge pixels";
  ASSERT_LT(wireframePixels, filledPixels)
      << "wireframe unexpectedly filled the triangle interior";
}

} // namespace
