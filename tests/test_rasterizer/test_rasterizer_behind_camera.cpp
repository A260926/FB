#include <TestHelpers.hpp>
#include <render/Rasterizer.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(Rasterizer, GeometryBehindCameraDoesNotReachFramebuffer) {
  auto scene = cameraScene();
  auto behindCamera = plane(1, 10, mat());
  unlit(behindCamera);
  scene->addGraphicObj(behindCamera, "behind");

  TraditionalRasterizer rasterizer(71, 53);
  rasterizer.addScene(scene);
  rasterizer.draw();

  for (float depth : rasterizer.depth()) {
    ASSERT_FALSE(std::isfinite(depth)) << "behind-camera geometry rendered";
  }
}

} // namespace
