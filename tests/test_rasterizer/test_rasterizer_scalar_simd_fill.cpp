#include "RasterizerTestScenes.hpp"
#include <render/Rasterizer.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::RasterizerTestScenes;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(Rasterizer, FilledTriangleMatchesBetweenScalarAndSIMD) {
  auto [scene, triangle] = slantedTexturedTriangle();
  TraditionalRasterizer scalar(101, 79), simd(101, 79);
  scalar.setSIMD(false);
  scalar.addScene(scene);
  simd.addScene(scene);
  scalar.draw();
  simd.draw();

  for (std::size_t pixel = 0; pixel < scalar.pixels().size(); ++pixel) {
    ASSERT_EQ(std::isfinite(scalar.depth()[pixel]),
              std::isfinite(simd.depth()[pixel]))
        << "coverage differs at pixel " << pixel;
    if (!std::isfinite(scalar.depth()[pixel])) {
      continue;
    }
    expectNear(scalar.depth()[pixel], simd.depth()[pixel], 1e-6f);
    expectNear(scalar.pixels()[pixel], simd.pixels()[pixel], 2e-6f);
  }
}

} // namespace
