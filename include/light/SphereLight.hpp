#pragma once
#include <object/Sphere.hpp>

namespace SoftRasterizer {
class SphereLight : public Sphere {
public:
  SphereLight(const glm::vec3 &p = glm::vec3(0),
              const glm::vec3 &radiance = glm::vec3(1), float radius = 1)
      : Sphere(p, radius) {
    m_material->emission = radiance;
  }
};
} // namespace SoftRasterizer
