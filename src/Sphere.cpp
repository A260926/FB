#include <object/Sphere.hpp>

SoftRasterizer::Sphere::Sphere(const glm::vec3 &center, float radius)
    : m_center(center), m_worldCenter(center), m_radius(radius),
      m_worldRadius(radius) {
  if (!finite(center) || !std::isfinite(radius) || radius <= 0) {
    throw std::invalid_argument("invalid sphere");
  }
  transform();
}

void SoftRasterizer::Sphere::transform() {
  /*A sphere stays spherical under translation, rotation and uniform scale.*/
  const glm::mat3 linearTransform(m_model);
  const float sx = glm::length(linearTransform[0]),
              sy = glm::length(linearTransform[1]),
              sz = glm::length(linearTransform[2]);
  const float tolerance = 1e-4f * sx;
  if (std::abs(sx - sy) > tolerance || std::abs(sx - sz) > tolerance ||
      std::abs(glm::dot(linearTransform[0], linearTransform[1])) >
          1e-4f * sx * sy ||
      std::abs(glm::dot(linearTransform[0], linearTransform[2])) >
          1e-4f * sx * sz ||
      std::abs(glm::dot(linearTransform[1], linearTransform[2])) >
          1e-4f * sy * sz) {
    throw std::invalid_argument("Sphere requires uniform scaling without "
                                "shear; use a mesh for ellipsoids");
  }
  m_worldCenter = glm::vec3(m_model * glm::vec4(m_center, 1));
  m_worldRadius = m_radius * sx;
  if (!finite(m_worldCenter) || !std::isfinite(m_worldRadius) ||
      !finite(glm::abs(m_worldCenter) + glm::vec3(m_worldRadius)) ||
      !std::isfinite(4 * Pi * m_worldRadius * m_worldRadius)) {
    throw std::invalid_argument("sphere transform overflow");
  }
}

SoftRasterizer::Intersection
SoftRasterizer::Sphere::getIntersect(const Ray &ray) const {
  if (!finite(ray.origin) || !finite(ray.direction)) {
    return {};
  }
  auto originOffset = glm::dvec3(ray.origin) - glm::dvec3(m_worldCenter),
       rayDirection = glm::dvec3(ray.direction);
  const double coefficientA = glm::dot(rayDirection, rayDirection),
               coefficientB = 2 * glm::dot(rayDirection, originOffset),
               coefficientC = glm::dot(originOffset, originOffset) -
                              double(m_worldRadius) * m_worldRadius;
  const double discriminant =
      coefficientB * coefficientB - 4 * coefficientA * coefficientC;
  if (coefficientA <= 0 || discriminant < 0) {
    return {};
  }
  /*Use the stable quadratic formula, then select a positive root in the ray
   * interval.*/
  double t0, t1;
  if (discriminant == 0) {
    t0 = t1 = -coefficientB / (2 * coefficientA);
  } else {
    const double q =
        -0.5 *
        (coefficientB + std::copysign(std::sqrt(discriminant), coefficientB));
    t0 = q / coefficientA;
    t1 = coefficientC / q;
    if (t0 > t1) {
      std::swap(t0, t1);
    }
  }
  double intersectTime = t0 > ray.tMin ? t0 : t1;
  if (intersectTime <= ray.tMin || intersectTime >= ray.tMax ||
      !std::isfinite(intersectTime)) {
    return {};
  }
  Intersection intersection;
  intersection.intersected = true;
  intersection.intersect_time = float(intersectTime);
  intersection.coords = ray.at(float(intersectTime));
  intersection.geometricNormal = unit(intersection.coords - m_worldCenter);
  intersection.frontFace =
      glm::dot(rayDirection, glm::dvec3(intersection.geometricNormal)) < 0;
  intersection.normal = intersection.frontFace ? intersection.geometricNormal
                                               : -intersection.geometricNormal;
  intersection.uv = getTexCoord(intersection.coords);
  intersection.obj = this;
  intersection.material = m_material;
  return intersection;
}

SoftRasterizer::SurfaceSample
SoftRasterizer::Sphere::sample(Sampler &rng) const {
  auto normal = rng.sphere();
  auto position = m_worldCenter + m_worldRadius * normal;
  return {position, normal, getTexCoord(position), m_material, 1 / getArea()};
}

glm::vec2
SoftRasterizer::Sphere::getTexCoord(const glm::vec3 &worldPosition) const {
  // Texture coordinates remain attached to the object when it rotates.
  auto objectNormal =
      unit(glm::vec3(glm::inverse(m_model) * glm::vec4(worldPosition, 1)) -
           m_center);
  return {0.5f + std::atan2(objectNormal.z, objectNormal.x) / (2 * Pi),
          0.5f + std::asin(std::clamp(objectNormal.y, -1.f, 1.f)) / Pi};
}

void SoftRasterizer::Sphere::appendRasterTriangles(
    std::vector<RasterTriangle> &stream) const {
  /*Rasterization uses a tessellated sphere; ray tracing keeps analytic
   * intersection.*/
  constexpr int rings = 24, slices = 48;
  const auto normalMatrix = glm::transpose(glm::inverse(glm::mat3(m_model)));
  auto makeVertex = [&](int y, int x) {
    float u = float(x) / slices, v = float(y) / rings;
    float latitude = Pi * (v - 0.5f), longitude = 2 * Pi * (u - 0.5f);
    glm::vec3 normal(std::cos(latitude) * std::cos(longitude),
                     std::sin(latitude),
                     std::cos(latitude) * std::sin(longitude));
    Vertex vertex;
    vertex.position =
        glm::vec3(m_model * glm::vec4(m_center + m_radius * normal, 1));
    vertex.normal = unit(normalMatrix * normal);
    vertex.texCoord = {u, v};
    return vertex;
  };
  auto appendTriangle = [&](std::array<Vertex, 3> vertices) {
    auto cross = glm::cross(vertices[1].position - vertices[0].position,
                            vertices[2].position - vertices[0].position);
    if (glm::dot(cross, cross) < 1e-16f) {
      return;
    }
    if (glm::dot(cross, vertices[0].normal) < 0) {
      std::swap(vertices[1], vertices[2]);
    }
    stream.push_back({vertices, m_material, m_shader});
  };
  for (int y = 0; y < rings; ++y) {
    for (int x = 0; x < slices; ++x) {
      auto vertexA = makeVertex(y, x), vertexB = makeVertex(y, x + 1),
           vertexC = makeVertex(y + 1, x), vertexD = makeVertex(y + 1, x + 1);
      appendTriangle({vertexA, vertexB, vertexC});
      appendTriangle({vertexB, vertexD, vertexC});
    }
  }
}

SoftRasterizer::Bounds3 SoftRasterizer::Sphere::getBounds() const {
  return {m_worldCenter - glm::vec3(m_worldRadius),
          m_worldCenter + glm::vec3(m_worldRadius)};
}
