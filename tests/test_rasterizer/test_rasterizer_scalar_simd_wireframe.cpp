#include "RasterizerTestScenes.hpp"
#include <render/Rasterizer.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::RasterizerTestScenes;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(Rasterizer, WireframeMatchesBetweenScalarAndSIMD) {
  auto [scene, triangle] = slantedTexturedTriangle();
  TraditionalRasterizer scalar(101, 79), simd(101, 79);
  scalar.setSIMD(false);
  scalar.addScene(scene);
  simd.addScene(scene);
  scalar.draw(Primitive::LINES);
  simd.draw(Primitive::LINES);

  for (std::size_t pixel = 0; pixel < scalar.pixels().size(); ++pixel) {
    ASSERT_EQ(std::isfinite(scalar.depth()[pixel]),
              std::isfinite(simd.depth()[pixel]))
        << "wireframe coverage differs at pixel " << pixel;
    expectNear(scalar.pixels()[pixel], simd.pixels()[pixel], 2e-6f);
    if (std::isfinite(scalar.depth()[pixel])) {
      expectNear(scalar.depth()[pixel], simd.depth()[pixel], 1e-6f);
    }
  }
}

} // namespace
