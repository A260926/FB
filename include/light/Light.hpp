#pragma once
#include <glm/glm.hpp>

namespace SoftRasterizer {
struct light_struct {
  glm::vec3 position{0}, intensity{0};
  light_struct() = default;

  light_struct(const glm::vec3 &p, const glm::vec3 &i)
      : position(p), intensity(i) {}
};
} // namespace SoftRasterizer
