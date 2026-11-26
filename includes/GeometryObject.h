#pragma once

class Material;

#include <vector>
#include "RGBColor.h"
#include "Point2D.h"
#include "Point3D.h"
#include "Triangle.h"
#include "Material.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

class GeometryObject {
private:
	Material *m_material;
  std::vector<Point3D> m_vertices;
  std::vector<RGBColor> m_colors;
  std::vector<Vector2D> m_texture_coords;
  std::vector<uint32_t> m_indices;

  // Explicit identity: a default-constructed glm::mat4 is left
  // uninitialized by newer GLM versions (no GLM_FORCE_CTOR_INIT), so relying
  // on implicit identity would corrupt the model transform.
  glm::mat4 m_model = glm::mat4(1.0f);
  glm::mat4 m_rotation = glm::mat4(1.0f);
  glm::mat4 m_translation = glm::mat4(1.0f);
public:
	GeometryObject();
	GeometryObject(Material* material, const std::vector<Point3D>& vertices, const std::vector<RGBColor>& colors, const std::vector<Vector2D>& texture_coords, const std::vector<uint32_t>& indices, const Point3D& center = Point3D(0,0,0));
	~GeometryObject();

  // Rotation using Euler angles
  void rotate_euler(const float roll, const float pitch, const float yaw);
  // Rotation using Quaternions (Axis-angle representation)
  void rotate_quat(const float angle_degrees, const Vector3D& axis);

  void translate(const Vector3D translation);

  const std::vector<Triangle3D> triangles() ;
	const Material * material() const;

private:
	const Vertex3D build_vertex(const std::vector<Point3D>& vertices, const std::vector<RGBColor>& colors, const std::vector<Vector2D>& texture_coords, const uint32_t vertex_index) const;
  const Point3D model_transform(const Point3D& p) const;
};

