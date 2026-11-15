#include <TestHelpers.hpp>

using namespace SoftRasterizer;
using namespace SoftRasterizer::TestHelpers;

namespace {

TEST(PathTracing, DiffuseIndirectRadianceIncludesVertexColor) {
  const glm::vec3 background(.4f, .8f, 1.5f), vertexColor(.25f, .5f, .75f);
  auto scene = cameraScene(30, .05f, 100, background);
  auto material = mat({.8f, .6f, .4f});
  std::vector<Vertex> vertices{{{-10, -10, -2}, {0, 0, 1}},
                               {{10, -10, -2}, {0, 0, 1}},
                               {{10, 10, -2}, {0, 0, 1}},
                               {{-10, 10, -2}, {0, 0, 1}}};
  for (auto &vertex : vertices) {
    vertex.color = vertexColor;
  }
  auto receiver = std::make_shared<Mesh>(
      vertices, std::vector<glm::uvec3>{{0, 1, 2}, {0, 2, 3}});
  receiver->setMaterial(material);
  scene->addGraphicObj(receiver, "receiver");
  PathTracing pt(3, 2);
  auto config = settings(16, 1); // rrDepth=100 keeps roulette disabled here.
  pt.configure(config);
  pt.addScene(scene);
  pt.draw();
  for (const auto pixel : pt.pixels()) {
    expectNear(pixel, glm::vec3(0)); // No light until the next ray sees Li.
  }

  config.maxDepth = 2;
  pt.configure(config);
  pt.draw();
  const auto expected = material->Kd * vertexColor * background;
  for (const auto pixel : pt.pixels()) {
    expectNear(pixel, expected);
  }
}

} // namespace
