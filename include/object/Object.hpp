#pragma once
#include <array>
#include <bvh/Bounds3.hpp>
#include <ray/Intersection.hpp>
#include <shader/Shader.hpp>
#include <vector>

namespace SoftRasterizer {
struct Vertex {
  glm::vec3 position{0}, normal{0};
  glm::vec2 texCoord{0};
  glm::vec3 color{1};
  Vertex() = default;
  Vertex(const glm::vec3 &position, const glm::vec3 &normal = glm::vec3(0.f),
         const glm::vec2 &texCoord = glm::vec2(0.f));
};

struct RasterTriangle {
  std::array<Vertex, 3> vertices;
  std::shared_ptr<Material> material;
  std::shared_ptr<Shader> shader;
};

class Object : public std::enable_shared_from_this<Object> {
public:
  virtual ~Object() = default;
  void updateModelMatrix(const glm::vec3 &axis, float degrees,
                         const glm::vec3 &translation, const glm::vec3 &scale);
  void setModelMatrix(const glm::mat4 &modelMatrix);

  const glm::mat4 &getModelMatrix() const {
    return m_model;
  }

  std::uint64_t revision() const {
    return m_revision;
  }

  virtual void setMaterial(std::shared_ptr<Material> material);

  const std::shared_ptr<Material> &getMaterial() const {
    return m_material;
  }

  virtual void bindShader2Mesh(std::shared_ptr<Shader> shader);
  virtual Bounds3 getBounds() const = 0;
  virtual Intersection getIntersect(const Ray &ray) const = 0;
  virtual float getArea() const = 0;
  virtual SurfaceSample sample(Sampler &rng) const = 0;
  virtual void
  appendRasterTriangles(std::vector<RasterTriangle> &out) const = 0;

  virtual void appendPrimitives(std::vector<std::shared_ptr<Object>> &out) {
    out.push_back(shared_from_this());
  }

protected:
  virtual void transform() = 0;

  /*Model-space source data is preserved by derived objects.*/
  glm::mat4 m_model{1};
  std::shared_ptr<Material> m_material = std::make_shared<Material>();
  std::shared_ptr<Shader> m_shader;
  std::uint64_t m_revision = 1;
};
} // namespace SoftRasterizer
