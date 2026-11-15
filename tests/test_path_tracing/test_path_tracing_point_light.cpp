#include <TestHelpers.hpp>
#include <object/Sphere.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(PathTracing, PointLightAndVisibilityMatchReference) {
  auto s = cameraScene(30);
  auto m = mat(glm::vec3(.5f));
  std::array<Vertex, 3> receiver{Vertex({-10, -10, -2}), Vertex({30, -10, -2}),
                                 Vertex({-10, 30, -2})};
  const glm::vec3 vertexTint(1, .5f, .25f);
  for (auto &v : receiver) {
    v.color = vertexTint;
  }
  s->addGraphicObj(triangle(receiver, m), "receiver");
  const glm::vec3 lightPosition(2, 0, 0), intensity(8);
  s->addLight("point",
              std::make_shared<light_struct>(lightPosition, intensity));
  PathTracing pt(1, 1, 4096);
  auto config = settings(4096, 1);
  pt.configure(config);
  pt.addScene(s);
  pt.draw();
  glm::dvec3 expected(0);
  for (std::size_t i = 0; i < config.samplesPerPixel; ++i) {
    Sampler rng(config.seed, 0, i);
    float dx = rng.next(), dy = rng.next();
    auto ray = s->getCamera().generateRay(dx, dy, 1, 1);
    auto p = ray.at(-2 / ray.direction.z), d = lightPosition - p;
    float r2 = glm::dot(d, d);
    expected += glm::dvec3(vertexTint * glm::vec3(.5f) * (1 / Pi) * intensity *
                           (d.z / std::sqrt(r2)) / r2);
  }
  expectNear(pt.pixels()[0],
             glm::vec3(expected / double(config.samplesPerPixel)), .00001f);
  auto blocker = std::make_shared<Sphere>(glm::vec3(1, 0, -1), .65f);
  s->addGraphicObj(blocker, "blocker");
  pt.draw();
  ASSERT_TRUE(maxComponent(pt.pixels()[0]) < .02f)
      << "point light shadow missing";
}

} // namespace
