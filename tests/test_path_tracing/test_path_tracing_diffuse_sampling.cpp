#include <TestHelpers.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(PathTracing, DiffuseSampleReportsBrdfAndSolidAnglePdf) {
  auto material = mat({.8f, .45f, .2f});
  const std::array<glm::vec3, 3> normals{glm::vec3(0, 0, 1), unit({1, 2, 3}),
                                         unit({-.4f, -1, .2f})};
  for (std::size_t n = 0; n < normals.size(); ++n) {
    const auto normal = normals[n];
    for (std::size_t i = 0; i < 256; ++i) {
      SCOPED_TRACE("normal " + std::to_string(n) + ", sample " +
                   std::to_string(i));
      Sampler sampler(47, n, i);
      const auto sample = material->sample(-normal, normal, true, {}, sampler);
      ASSERT_TRUE(finite(sample.direction));
      expectNear(glm::length(sample.direction), 1);
      EXPECT_FALSE(sample.delta);
      EXPECT_FALSE(sample.transmitted);
      expectNear(sample.fr, material->Kd / Pi);
      expectNear(sample.deltaCoefficient, glm::vec3(0));
      expectNear(sample.deltaEventProbability, 0);
      const float cosine = glm::dot(normal, sample.direction);
      ASSERT_GT(cosine, 0);
      ASSERT_GT(sample.solidAnglePdf, 0);
      ASSERT_TRUE(std::isfinite(sample.solidAnglePdf));
      expectNear(sample.solidAnglePdf, cosine / Pi);
      expectNear(sample.fr * cosine / sample.solidAnglePdf, material->Kd);
    }
  }
}

} // namespace
