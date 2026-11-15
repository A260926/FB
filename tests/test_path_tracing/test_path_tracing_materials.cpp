#include <TestHelpers.hpp>
#include <object/Sphere.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(PathTracing, MirrorAndDielectricFresnelTransport) {
  auto mirror = mat(glm::vec3(.8f));
  mirror->type = MaterialType::Mirror;
  auto s = cameraScene(10, .05f, 100, {.4, .2, .1});
  s->addGraphicObj(plane(-1, 10, mirror), "mirror");
  PathTracing pt(3, 3);
  pt.configure(settings(64, 3));
  pt.addScene(s);
  pt.draw();
  for (auto p : pt.pixels()) {
    expectNear(p, {.32, .16, .08});
  }
  auto glassScene = cameraScene(.01f);
  auto glass = std::make_shared<Sphere>(glm::vec3(0, 0, -2), .5f);
  auto g = mat();
  g->type = MaterialType::Dielectric;
  glass->setMaterial(g);
  glassScene->addGraphicObj(glass, "glass");
  auto light = mat(glm::vec3(0));
  light->emission = glm::vec3(3);
  glassScene->addGraphicObj(plane(-4, 10, light), "light");
  PathTracing transmission(1, 1);
  transmission.configure(settings(32768, 32));
  transmission.addScene(glassScene);
  transmission.draw();
  const float f = dielectricFresnel(1, 1, 1.5f);
  expectNear(transmission.pixels()[0], glm::vec3(3 * (1 - f) / (1 + f)), .035f);
  expectNear(dielectricFresnel(.1f, 1.5f, 1), 1); // total internal reflection
}

} // namespace
