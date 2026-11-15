#include <TestHelpers.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(PathTracing, DielectricSamplesKeepFresnelCoefficientsAndProbabilities) {
  auto material = mat({.8f, .6f, .3f});
  material->type = MaterialType::Dielectric;
  material->ior = 1.5f;
  // Normal-incidence Fresnel: ((1 - 1.5) / (1 + 1.5))^2 = 0.04.
  constexpr float fresnel = .04f, etaSquared = 4.f / 9;
  unsigned reflected = 0, transmitted = 0;
  for (std::size_t i = 0; i < 4096; ++i) {
    SCOPED_TRACE("sample " + std::to_string(i));
    Sampler sampler(71, 0, i);
    const auto sample =
        material->sample({0, 0, -1}, {0, 0, 1}, true, {}, sampler);
    ASSERT_TRUE(sample.delta);
    expectNear(sample.fr, glm::vec3(0));
    expectNear(sample.solidAnglePdf, 0);
    ASSERT_TRUE(std::isfinite(sample.deltaEventProbability));
    ASSERT_GT(sample.deltaEventProbability, 0);
    if (sample.transmitted) {
      ++transmitted;
      expectNear(sample.direction, {0, 0, -1});
      expectNear(sample.deltaEventProbability, 1 - fresnel);
      expectNear(sample.deltaCoefficient,
                 material->Kd * ((1 - fresnel) * etaSquared));
      expectNear(sample.deltaCoefficient / sample.deltaEventProbability,
                 material->Kd * etaSquared);
    } else {
      ++reflected;
      expectNear(sample.direction, {0, 0, 1});
      expectNear(sample.deltaEventProbability, fresnel);
      expectNear(sample.deltaCoefficient, material->Kd * fresnel);
      expectNear(sample.deltaCoefficient / sample.deltaEventProbability,
                 material->Kd);
    }
  }
  EXPECT_GT(reflected, 0u);
  EXPECT_GT(transmitted, 0u);
  // The fixed streams must choose events with the reported probability.
  expectNear(float(reflected) / 4096, fresnel, .015f);
}

} // namespace
