#include <TestHelpers.hpp>
#include <render/Rasterizer.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(Rasterizer, TriangleCrossingCameraPlaneIsSkipped) {
  auto scene = cameraScene(75, .1f, 4);
  scene->addGraphicObj(triangle({Vertex({0, .8f, .4f}), Vertex({-2, -.8f, -2}),
                                 Vertex({2, -.8f, -2})}),
                       "camera plane crossing");

  TraditionalRasterizer rasterizer(71, 53);
  rasterizer.addScene(scene);
  rasterizer.draw();
  for (float depth : rasterizer.depth()) {
    ASSERT_FALSE(std::isfinite(depth))
        << "triangle crossing clip.w == 0 was projected";
  }
}

} // namespace
