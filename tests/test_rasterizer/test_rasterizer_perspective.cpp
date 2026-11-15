#include "RasterizerTestScenes.hpp"
#include <render/Rasterizer.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::RasterizerTestScenes;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(Rasterizer, PerspectiveCorrectInterpolationMatchesRayIntersection) {
  auto [scene, triangle] = slantedTexturedTriangle();
  TraditionalRasterizer rasterizer(101, 79);
  rasterizer.addScene(scene);
  rasterizer.draw();

  int checkedPixels = 0;
  for (std::size_t y = 0; y < 79; ++y) {
    for (std::size_t x = 0; x < 101; ++x) {
      const std::size_t pixel = y * 101 + x;
      if (!std::isfinite(rasterizer.depth()[pixel])) {
        continue;
      }
      auto ray = scene->getCamera().generateRay(x + .5f, y + .5f, 101, 79);
      auto hit = triangle->getIntersect(ray);
      if (!hit.intersected) {
        continue;
      }
      expectNear(rasterizer.pixels()[pixel], {hit.uv.x, hit.uv.y, 0}, .001f);
      const auto clip = scene->getCamera().projectionMatrix(101.f / 79) *
                        scene->getCamera().view() * glm::vec4(hit.coords, 1);
      expectNear(rasterizer.depth()[pixel], (clip.z / clip.w) * .5f + .5f,
                 .00002f);
      ++checkedPixels;
    }
  }
  ASSERT_GT(checkedPixels, 500) << "insufficient perspective samples";
}

} // namespace
