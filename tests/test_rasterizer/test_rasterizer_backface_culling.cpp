#include <TestHelpers.hpp>
#include <render/Rasterizer.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(Rasterizer, BackfaceIsRemovedWhenCullingIsEnabled) {
  auto scene = cameraScene();
  auto backFacingPlane = plane(-1, 10, mat(), false);
  unlit(backFacingPlane);
  scene->addGraphicObj(backFacingPlane, "back");

  TraditionalRasterizer rasterizer(31, 29);
  rasterizer.addScene(scene);
  rasterizer.setBackfaceCulling(true);
  rasterizer.draw();

  for (float depth : rasterizer.depth()) {
    ASSERT_FALSE(std::isfinite(depth)) << "backface rendered while culling";
  }
}

} // namespace
