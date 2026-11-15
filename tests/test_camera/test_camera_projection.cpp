#include <TestHelpers.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(Camera, ProjectionMatchesPrimaryRays) {
  Camera c;
  expectNear(c.generateRay(160, 80, 320, 160).direction, {0, 0, -1});
  c.lookAt({3, 2, 4}, {-1, 0, -2}, {0, 1, .2f});
  c.projection(67, .2f, 30);
  for (auto p : std::vector<glm::vec2>{
           {.5, .5}, {90.3, 37.7}, {319.5, 159.5}, {160, 80}}) {
    auto ray = c.generateRay(p.x, p.y, 320, 160);
    auto point = ray.at(3);
    auto clip = c.projectionMatrix(2) * c.view() * glm::vec4(point, 1);
    auto ndc = glm::vec3(clip) / clip.w;
    expectNear((ndc.x * .5f + .5f) * 320, p.x, .0002f);
    expectNear((.5f - ndc.y * .5f) * 160, p.y, .0002f);
    auto nearPoint = c.view() * glm::vec4(ray.at(ray.tMin), 1);
    expectNear(nearPoint.z, -.2f, .00001f);
  }
  expectNear(glm::determinant(glm::mat3(c.view())), 1);
  EXPECT_THROW(([&] { c.projection(180, 1, 10); })(), std::exception);
  EXPECT_THROW(([&] { c.lookAt({0, 0, 0}, {0, 0, 0}, {0, 1, 0}); })(),
               std::exception);
}

} // namespace
