#pragma once
#include <TestHelpers.hpp>

namespace SoftRasterizer::RasterizerTestScenes {

struct SlantedTriangleScene {
  std::shared_ptr<Scene> scene;
  std::shared_ptr<Triangle> triangle;
};

inline SlantedTriangleScene slantedTexturedTriangle() {
  auto texture = std::make_shared<TextureLoader>(
      2, 2, std::vector<glm::vec3>{{0, 1, 0}, {1, 1, 0}, {0, 0, 0}, {1, 0, 0}});
  auto material = TestHelpers::mat();
  material->texture = texture;
  auto triangle = TestHelpers::triangle({Vertex({-1, -1, -1}, {}, {0, 0}),
                                         Vertex({1, -1, -4}, {}, {1, 0}),
                                         Vertex({0, 1, -2}, {}, {.5f, 1})},
                                        material);
  TestHelpers::unlit(triangle);
  auto scene = TestHelpers::cameraScene(70);
  scene->addGraphicObj(triangle, "slanted");
  return {scene, triangle};
}

inline std::shared_ptr<Scene> sharedEdgeScene(bool reverseOrder) {
  auto scene = TestHelpers::cameraScene(90);
  constexpr float aspect = 63.f / 47;
  Vertex bottomLeft({-aspect, -1, -1});
  Vertex bottomRight({aspect, -1, -1});
  Vertex topRight({aspect, 1, -1});
  Vertex topLeft({-aspect, 1, -1});
  auto red = TestHelpers::triangle({bottomLeft, bottomRight, topRight},
                                   TestHelpers::mat({1, 0, 0}));
  auto green = TestHelpers::triangle({bottomLeft, topRight, topLeft},
                                     TestHelpers::mat({0, 1, 0}));
  TestHelpers::unlit(red);
  TestHelpers::unlit(green);
  scene->addGraphicObj(red, reverseOrder ? "b" : "a");
  scene->addGraphicObj(green, reverseOrder ? "a" : "b");
  return scene;
}

} // namespace SoftRasterizer::RasterizerTestScenes
