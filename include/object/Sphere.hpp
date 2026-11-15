#pragma once
#include <object/Object.hpp>

namespace SoftRasterizer {
class Sphere : public Object {
public:
  explicit Sphere(const glm::vec3 &center = glm::vec3(0.f),
                  const float radius = 1.f);

  glm::vec3 getCenter() const {
    return m_worldCenter;
  }

  Bounds3 getBounds() const override;
  Intersection getIntersect(const Ray &ray) const override;

  float getArea() const override {
    return 4 * Pi * m_worldRadius * m_worldRadius;
  }

  SurfaceSample sample(Sampler &rng) const override;
  void appendRasterTriangles(std::vector<RasterTriangle> &out) const override;

private:
  void transform() override;
  glm::vec2 getTexCoord(const glm::vec3 &worldPosition) const;

  /*Model-space sphere and its transformed world-space representation.*/
  glm::vec3 m_center, m_worldCenter;
  float m_radius, m_worldRadius;
};
} // namespace SoftRasterizer
