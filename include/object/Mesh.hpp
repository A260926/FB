#pragma once
#include <object/Triangle.hpp>

namespace SoftRasterizer {
class Mesh : public Object {
public:
  Mesh(std::vector<Vertex> vertices, std::vector<glm::uvec3> faces,
       std::vector<std::shared_ptr<Material>> materials = {});
  void setMaterial(std::shared_ptr<Material> material) override;
  void bindShader2Mesh(std::shared_ptr<Shader> shader) override;

  Bounds3 getBounds() const override {
    return m_boundingBox;
  }

  Intersection getIntersect(const Ray &ray) const override;

  float getArea() const override {
    return m_area;
  }

  SurfaceSample sample(Sampler &rng) const override;
  void appendRasterTriangles(std::vector<RasterTriangle> &out) const override;
  void appendPrimitives(std::vector<std::shared_ptr<Object>> &out) override;

  std::size_t triangleCount() const {
    return m_triangles.size();
  }

private:
  std::vector<std::shared_ptr<Triangle>> m_triangles;
  Bounds3 m_boundingBox;
  float m_area = 0;
  void transform() override;
};
} // namespace SoftRasterizer
