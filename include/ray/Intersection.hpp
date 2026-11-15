#pragma once
#include <memory>
#include <ray/Ray.hpp>

namespace SoftRasterizer {
class Object;
struct Material;

struct Intersection {
  bool intersected = false, frontFace = true;
  float intersect_time = std::numeric_limits<float>::infinity();
  glm::vec3 coords{0}, normal{0, 1, 0}, geometricNormal{0, 1, 0};
  glm::vec2 uv{0};
  glm::vec3 color{1};
  std::shared_ptr<Material> material;
  const Object *obj = nullptr;
};

struct SurfaceSample {
  glm::vec3 position{0}, normal{0, 1, 0};
  glm::vec2 uv{0};
  std::shared_ptr<Material> material;
  float pdfArea = 0;
};
} // namespace SoftRasterizer
