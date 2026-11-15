#pragma once
#include <array>
#include <cmath>
#include <gtest/gtest.h>
#include <memory>
#include <object/Mesh.hpp>
#include <render/PathTracing.hpp>
#include <string>
#include <utility>
#include <vector>

namespace SoftRasterizer::TestHelpers {

inline void expectNear(float actual, float expected, float tolerance = 1e-5f) {
  EXPECT_TRUE(std::isfinite(actual));
  EXPECT_TRUE(std::isfinite(expected));
  EXPECT_NEAR(actual, expected, tolerance);
}

inline void expectNear(glm::vec3 actual, glm::vec3 expected,
                       float tolerance = 1e-5f) {
  for (int i = 0; i < 3; ++i) {
    SCOPED_TRACE("vector component " + std::to_string(i));
    expectNear(actual[i], expected[i], tolerance);
  }
}

inline std::shared_ptr<Material> mat(glm::vec3 kd = glm::vec3(1)) {
  auto m = std::make_shared<Material>();
  m->Kd = kd;
  return m;
}

inline std::shared_ptr<Triangle> triangle(std::array<Vertex, 3> v,
                                          std::shared_ptr<Material> m = mat()) {
  return std::make_shared<Triangle>(v, std::move(m));
}

inline std::shared_ptr<Mesh>
plane(float z, float radius, std::shared_ptr<Material> m, bool front = true) {
  std::vector<Vertex> v{{{-radius, -radius, z}},
                        {{radius, -radius, z}},
                        {{radius, radius, z}},
                        {{-radius, radius, z}}};
  std::vector<glm::uvec3> f{{0, 1, 2}, {0, 2, 3}};
  if (!front) {
    for (auto &t : f) {
      std::swap(t.y, t.z);
    }
  }
  auto p = std::make_shared<Mesh>(v, f);
  p->setMaterial(std::move(m));
  return p;
}

inline std::shared_ptr<Scene> cameraScene(float fov = 60,
                                          float nearPlane = .05f,
                                          float farPlane = 100,
                                          glm::vec3 bg = glm::vec3(0)) {
  auto s = std::make_shared<Scene>("test", glm::vec3(0), glm::vec3(0, 0, -1),
                                   glm::vec3(0, 1, 0), bg);
  s->setProjectionMatrix(fov, nearPlane, farPlane);
  return s;
}

inline void unlit(const std::shared_ptr<Object> &o) {
  auto s = std::make_shared<Shader>();
  s->type = SHADERS_TYPE::TEXTURE;
  o->bindShader2Mesh(s);
}

inline PathTracingSettings settings(std::size_t spp = 16, unsigned depth = 2) {
  PathTracingSettings s;
  s.samplesPerPixel = spp;
  s.maxDepth = depth;
  s.rrDepth = 100;
  s.seed = 29;
  return s;
}

} // namespace SoftRasterizer::TestHelpers
