#include <object/Mesh.hpp>

SoftRasterizer::Mesh::Mesh(
    std::vector<Vertex> vertices, std::vector<glm::uvec3> faces,
    std::vector<std::shared_ptr<Material>> faceMaterials) {
  if (!faceMaterials.empty() && faceMaterials.size() != faces.size()) {
    throw std::invalid_argument("material/face count mismatch");
  }
  if (!faceMaterials.empty()) {
    if (!faceMaterials.front()) {
      throw std::invalid_argument("null face material");
    }
    m_material = faceMaterials.front();
  }
  m_triangles.reserve(faces.size());
  for (std::size_t i = 0; i < faces.size(); ++i) {
    auto face = faces[i];
    if (face.x >= vertices.size() || face.y >= vertices.size() ||
        face.z >= vertices.size()) {
      throw std::invalid_argument("invalid face index");
    }
    auto faceMaterial = faceMaterials.empty() ? m_material : faceMaterials[i];
    m_triangles.push_back(std::make_shared<Triangle>(
        std::array<Vertex, 3>{vertices[face.x], vertices[face.y],
                              vertices[face.z]},
        faceMaterial));
  }
  transform();
}

void SoftRasterizer::Mesh::transform() {
  /*Update world-space triangles, bounding box and sampling area together.*/
  Bounds3 newBounds;
  float newArea = 0;
  for (auto &triangle : m_triangles) {
    triangle->setModelMatrix(m_model);
    newBounds.expand(triangle->getBounds());
    newArea += triangle->getArea();
  }
  if (!std::isfinite(newArea)) {
    throw std::invalid_argument("mesh area overflow");
  }
  m_boundingBox = newBounds;
  m_area = newArea;
}

void SoftRasterizer::Mesh::setMaterial(std::shared_ptr<Material> material) {
  SoftRasterizer::Object::setMaterial(material);
  for (auto &triangle : m_triangles) {
    triangle->setMaterial(material);
  }
}

void SoftRasterizer::Mesh::bindShader2Mesh(std::shared_ptr<Shader> shader) {
  SoftRasterizer::Object::bindShader2Mesh(shader);
  for (auto &triangle : m_triangles) {
    triangle->bindShader2Mesh(shader);
  }
}

SoftRasterizer::Intersection
SoftRasterizer::Mesh::getIntersect(const Ray &ray) const {
  Intersection nearest;
  for (const auto &triangle : m_triangles) {
    Ray limited = ray;
    limited.tMax = std::min(ray.tMax, nearest.intersect_time);
    auto intersection = triangle->getIntersect(limited);
    if (intersection.intersected) {
      nearest = intersection;
    }
  }
  return nearest;
}

SoftRasterizer::SurfaceSample SoftRasterizer::Mesh::sample(Sampler &rng) const {
  if (m_area <= 0) {
    return {};
  }
  const float target =
      std::min(rng.next() * m_area, std::nextafter(m_area, 0.f));
  float accumulated = 0;
  for (const auto &triangle : m_triangles) {
    accumulated += triangle->getArea();
    if (target < accumulated) {
      auto surfaceSample = triangle->sample(rng);
      surfaceSample.pdfArea = 1 / m_area;
      return surfaceSample;
    }
  }
  return {};
}

void SoftRasterizer::Mesh::appendRasterTriangles(
    std::vector<RasterTriangle> &out) const {
  for (const auto &triangle : m_triangles) {
    triangle->appendRasterTriangles(out);
  }
}

void SoftRasterizer::Mesh::appendPrimitives(
    std::vector<std::shared_ptr<Object>> &out) {
  for (auto &triangle : m_triangles) {
    if (triangle->getArea() > 0) {
      out.push_back(triangle);
    }
  }
}
