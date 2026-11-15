#include <object/Triangle.hpp>

SoftRasterizer::Triangle::Triangle(const std::array<Vertex, 3> &vertices,
                                   std::shared_ptr<Material> material)
    : m_sourceVertices(vertices) {
  if (!material) {
    throw std::invalid_argument("null material");
  }
  for (const auto &vertex : vertices) {
    if (!finite(vertex.position) || !finite(vertex.normal) ||
        !finite(vertex.color) || !std::isfinite(vertex.texCoord.x) ||
        !std::isfinite(vertex.texCoord.y)) {
      throw std::invalid_argument("invalid triangle vertex");
    }
  }
  m_material = std::move(material);
  transform();
}

void SoftRasterizer::Triangle::transform() {
  /*Transform positions and normals into world space, without camera
   * projection.*/
  const auto normalMatrix = glm::transpose(glm::inverse(glm::mat3(m_model)));
  for (int i = 0; i < 3; ++i) {
    m_vertices[i] = m_sourceVertices[i];
    m_vertices[i].position =
        glm::vec3(m_model * glm::vec4(m_sourceVertices[i].position, 1));
    if (!finite(m_vertices[i].position)) {
      throw std::invalid_argument("triangle transform overflow");
    }
  }
  auto cross = glm::cross(m_vertices[1].position - m_vertices[0].position,
                          m_vertices[2].position - m_vertices[0].position);
  m_area = 0.5f * glm::length(cross);
  if (!std::isfinite(m_area)) {
    throw std::invalid_argument("triangle area overflow");
  }
  m_faceNormal = unit(cross);
  for (int i = 0; i < 3; ++i) {
    m_vertices[i].normal =
        unit(normalMatrix * m_sourceVertices[i].normal, m_faceNormal);
    if (glm::dot(m_vertices[i].normal, m_faceNormal) < 0) {
      m_vertices[i].normal = -m_vertices[i].normal;
    }
  }
}

SoftRasterizer::Bounds3 SoftRasterizer::Triangle::getBounds() const {
  Bounds3 bounds;
  for (const auto &vertex : m_vertices) {
    bounds.expand(vertex.position);
  }
  return bounds;
}

SoftRasterizer::Intersection
SoftRasterizer::Triangle::getIntersect(const Ray &ray) const {
  if (m_area <= 0 || !finite(ray.origin) || !finite(ray.direction)) {
    return {};
  }
  /*Moller-Trumbore intersection; barycentric u/v also interpolate attributes.*/
  const auto e1 =
      glm::dvec3(m_vertices[1].position) - glm::dvec3(m_vertices[0].position);
  const auto e2 =
      glm::dvec3(m_vertices[2].position) - glm::dvec3(m_vertices[0].position);
  const auto p = glm::cross(glm::dvec3(ray.direction), e2);
  const double det = glm::dot(e1, p);
  if (std::abs(det) < 1e-14) {
    return {};
  }
  const auto originOffset =
      glm::dvec3(ray.origin) - glm::dvec3(m_vertices[0].position);
  const double u = glm::dot(originOffset, p) / det;
  if (u < 0 || u > 1) {
    return {};
  }
  const auto q = glm::cross(originOffset, e1);
  const double v = glm::dot(glm::dvec3(ray.direction), q) / det;
  if (v < 0 || u + v > 1) {
    return {};
  }
  const double intersectTime = glm::dot(e2, q) / det;
  if (intersectTime <= ray.tMin || intersectTime >= ray.tMax ||
      !std::isfinite(intersectTime)) {
    return {};
  }
  Intersection intersection;
  intersection.intersected = true;
  intersection.intersect_time = float(intersectTime);
  intersection.coords = ray.at(float(intersectTime));
  intersection.frontFace = glm::dot(ray.direction, m_faceNormal) < 0;
  intersection.geometricNormal = m_faceNormal;
  intersection.normal = unit(float(1 - u - v) * m_vertices[0].normal +
                                 float(u) * m_vertices[1].normal +
                                 float(v) * m_vertices[2].normal,
                             m_faceNormal);
  if (!intersection.frontFace) {
    intersection.normal = -intersection.normal;
  }
  if (glm::dot(ray.direction, intersection.normal) >= 0) {
    intersection.normal = intersection.frontFace ? m_faceNormal : -m_faceNormal;
  }
  intersection.uv = float(1 - u - v) * m_vertices[0].texCoord +
                    float(u) * m_vertices[1].texCoord +
                    float(v) * m_vertices[2].texCoord;
  intersection.color = glm::clamp(float(1 - u - v) * m_vertices[0].color +
                                      float(u) * m_vertices[1].color +
                                      float(v) * m_vertices[2].color,
                                  glm::vec3(0), glm::vec3(1));
  intersection.material = m_material;
  intersection.obj = this;
  return intersection;
}

SoftRasterizer::SurfaceSample
SoftRasterizer::Triangle::sample(Sampler &rng) const {
  if (m_area <= 0) {
    return {};
  }
  const float sqrtSample = std::sqrt(rng.next()), sampleY = rng.next();
  const glm::vec3 barycentric(1 - sqrtSample, sqrtSample * (1 - sampleY),
                              sqrtSample * sampleY);
  return {barycentric.x * m_vertices[0].position +
              barycentric.y * m_vertices[1].position +
              barycentric.z * m_vertices[2].position,
          m_faceNormal,
          barycentric.x * m_vertices[0].texCoord +
              barycentric.y * m_vertices[1].texCoord +
              barycentric.z * m_vertices[2].texCoord,
          m_material, 1 / m_area};
}

void SoftRasterizer::Triangle::appendRasterTriangles(
    std::vector<RasterTriangle> &stream) const {
  if (m_area > 0) {
    stream.push_back({m_vertices, m_material, m_shader});
  }
}
