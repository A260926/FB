#pragma once
#include <glm/glm.hpp>
#include <limits>

namespace SoftRasterizer {
struct Ray {
  glm::vec3 origin, direction;
  float tMin = 0.f, tMax = std::numeric_limits<float>::infinity();

  Ray(const glm::vec3 &o, const glm::vec3 &d, float nearT = 0.f,
      float farT = std::numeric_limits<float>::infinity())
      : origin(o), direction(d), tMin(nearT), tMax(farT) {}

  glm::vec3 at(float t) const {
    return origin + t * direction;
  }
};
} // namespace SoftRasterizer
