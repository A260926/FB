#include <TestHelpers.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(PathTracing, EqualIorHasOneCertainTransmissionEvent) {
  auto material = mat({.8f, .6f, .3f});
  material->type = MaterialType::Dielectric;
  material->ior = 1;
  for (const bool frontFace : {false, true}) {
    Sampler sampler(23);
    const auto sample =
        material->sample({.6f, 0, -.8f}, {0, 0, 1}, frontFace, {}, sampler);
    EXPECT_TRUE(sample.delta);
    EXPECT_TRUE(sample.transmitted);
    expectNear(sample.direction, {.6f, 0, -.8f});
    expectNear(sample.fr, glm::vec3(0));
    expectNear(sample.solidAnglePdf, 0);
    expectNear(sample.deltaEventProbability, 1);
    expectNear(sample.deltaCoefficient, material->Kd);
    expectNear(sample.deltaCoefficient / sample.deltaEventProbability,
               material->Kd);
  }
}

} // namespace
