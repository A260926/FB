#include <TestHelpers.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(PathTracing, TotalInternalReflectionHasOneCertainEvent) {
  auto material = mat({.8f, .6f, .3f});
  material->type = MaterialType::Dielectric;
  material->ior = 1.5f;
  for (std::size_t i = 0; i < 32; ++i) {
    Sampler sampler(17, 0, i);
    // Glass to air: eta^2 * sin^2(theta) = 2.25 * 0.64 > 1.
    const auto sample =
        material->sample({.8f, 0, -.6f}, {0, 0, 1}, false, {}, sampler);
    EXPECT_TRUE(sample.delta);
    EXPECT_FALSE(sample.transmitted);
    expectNear(sample.direction, {.8f, 0, .6f});
    expectNear(sample.fr, glm::vec3(0));
    expectNear(sample.solidAnglePdf, 0);
    expectNear(sample.deltaEventProbability, 1);
    expectNear(sample.deltaCoefficient, material->Kd);
  }
}

} // namespace
