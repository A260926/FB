#include <TestHelpers.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(PathTracing, MirrorSampleSeparatesContinuousAndDiscreteMeasures) {
  auto material = mat({.8f, .6f, .3f});
  material->type = MaterialType::Mirror;
  Sampler sampler(11);
  const auto sample =
      material->sample({.6f, 0, -.8f}, {0, 0, 1}, true, {}, sampler);
  EXPECT_TRUE(sample.delta);
  EXPECT_FALSE(sample.transmitted);
  expectNear(sample.direction, {.6f, 0, .8f});
  // The ideal mirror has no continuous BRDF density. Its response is stored
  // separately as a discrete delta coefficient.
  expectNear(sample.fr, glm::vec3(0));
  expectNear(sample.solidAnglePdf, 0);
  expectNear(sample.deltaEventProbability, 1);
  expectNear(sample.deltaCoefficient, material->Kd);
  expectNear(sample.deltaCoefficient / sample.deltaEventProbability,
             material->Kd);
}

} // namespace
