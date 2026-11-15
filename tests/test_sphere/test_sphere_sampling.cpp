#include <TestHelpers.hpp>
#include <object/Sphere.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(Sphere, RootsScaleUVAndUniformAreaSampling) {
  Sphere s({2, 0, -3}, 1);
  expectNear(s.getBounds().min, {1, -1, -4});
  auto tangent = s.getIntersect(Ray({3, 0, 0}, {0, 0, -1}));
  ASSERT_TRUE(tangent.intersected) << "tangent missed";
  expectNear(tangent.intersect_time, 3);
  auto inside = s.getIntersect(Ray({2, 0, -3}, {1, 0, 0}));
  ASSERT_TRUE(inside.intersected && !inside.frontFace) << "inside sphere hit wrong";
  expectNear(inside.intersect_time, 1);
  ASSERT_TRUE(!s.getIntersect(Ray({2, 0, 0}, {0, 0, 1})).intersected) << "negative root accepted";
  s.updateModelMatrix({0, 1, 0}, 0, {0, 0, 0}, {2, 2, 2});
  expectNear(s.getArea(), 16 * Pi);
  auto before = s.getBounds();
  EXPECT_THROW(([&] { s.updateModelMatrix({0, 1, 0}, 0, {0, 0, 0}, {2, 1, 1}); })(), std::exception);
  expectNear(s.getBounds().min, before.min);
  Sampler rng(17);
  glm::dvec3 mean(0), second(0);
  for (int i = 0; i < 100000; ++i) {
    auto sample = s.sample(rng);
    auto n = sample.normal;
    mean += glm::dvec3(n);
    second += glm::dvec3(n * n);
    expectNear(sample.pdfArea, 1 / (16 * Pi));
    expectNear(glm::length(sample.position - s.getCenter()), 2, .00001f);
  }
  expectNear(glm::vec3(mean / 100000.), glm::vec3(0), .007f);
  expectNear(glm::vec3(second / 100000.), glm::vec3(1.f / 3), .005f);

  // Known model-space directions keep their UVs after rotation and translation.
  s.updateModelMatrix({0, 1, 0}, 90, {1, 2, 3}, {2, 2, 2});
  for (const auto &direction : std::array<glm::vec3, 2>{{{1, 0, 0}, {0, 0, 1}}}) {
    auto position = glm::vec3(s.getModelMatrix() * glm::vec4(glm::vec3(2, 0, -3) + direction, 1));
    auto normal = unit(position - s.getCenter());
    auto hit = s.getIntersect(Ray(position + 3.f * normal, -normal));
    ASSERT_TRUE(hit.intersected) << "rotated textured sphere missed";
    expectNear(hit.uv.x, direction.x == 1 ? .5f : .75f, .0001f);
    expectNear(hit.uv.y, .5f, .0001f);
  }
  for (int i = 0; i < 32; ++i) {
    auto sample = s.sample(rng);
    auto hit = s.getIntersect(Ray(sample.position + 3.f * sample.normal, -sample.normal));
    ASSERT_TRUE(hit.intersected) << "sampled sphere point missed";
    expectNear(hit.uv.x, sample.uv.x, .0001f);
    expectNear(hit.uv.y, sample.uv.y, .0001f);
  }
}

} // namespace
