#pragma once
#include <object/Object.hpp>

namespace SoftRasterizer {
class Triangle : public Object {
public:
  explicit Triangle(
      const std::array<Vertex, 3> &vertices,
      std::shared_ptr<Material> material = std::make_shared<Material>());
  Bounds3 getBounds() const override;
  Intersection getIntersect(const Ray &ray) const override;

  float getArea() const override {
    return m_area;
  }

  SurfaceSample sample(Sampler &rng) const override;

  void
  appendRasterTriangles(std::vector<RasterTriangle> &stream) const override;

private:
  /*Original vertices remain in model space.*/
  std::array<Vertex, 3> m_sourceVertices;
  /*World-space vertices are shared by rasterization and ray queries.*/
  std::array<Vertex, 3> m_vertices;
  glm::vec3 m_faceNormal{0, 1, 0};
  float m_area = 0;
  void transform() override;
};
} // namespace SoftRasterizer
