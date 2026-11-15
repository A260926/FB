#include "softgpu/mesh.hpp"

#include <cmath>

namespace softgpu {

Mesh make_quad(Vec3 o, Vec3 du, Vec3 dv) {
  Mesh m;
  const Vec3 n = normalize(cross(du, dv));
  m.vertices = {{o, n, {0, 0}}, {o + du, n, {1, 0}}, {o + du + dv, n, {1, 1}}, {o + dv, n, {0, 1}}};
  m.indices = {0, 1, 2, 0, 2, 3};
  return m;
}

Mesh make_cube() {
  Mesh m;
  const Vec3 n[6] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
  for (int f = 0; f < 6; ++f) {
    // Tangent frame per face so that (u, v) map onto the face counter-clockwise from outside.
    const Vec3 up = std::fabs(n[f].y) > 0.5f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    const Vec3 t = normalize(cross(up, n[f]));
    const Vec3 b = cross(n[f], t);
    const Vec3 c = n[f] * 0.5f;
    const uint32_t base = static_cast<uint32_t>(m.vertices.size());
    m.vertices.push_back({c - t * 0.5f - b * 0.5f, n[f], {0, 0}});
    m.vertices.push_back({c + t * 0.5f - b * 0.5f, n[f], {1, 0}});
    m.vertices.push_back({c + t * 0.5f + b * 0.5f, n[f], {1, 1}});
    m.vertices.push_back({c - t * 0.5f + b * 0.5f, n[f], {0, 1}});
    for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
  }
  return m;
}

Mesh make_torus(int rings, int segments, float major, float minor) {
  Mesh m;
  for (int i = 0; i <= rings; ++i) {
    const float u = static_cast<float>(i) / rings, a = u * 2 * kPi;
    const float ca = std::cos(a), sa = std::sin(a);
    for (int j = 0; j <= segments; ++j) {
      const float v = static_cast<float>(j) / segments, b = v * 2 * kPi;
      const float cb = std::cos(b), sb = std::sin(b);
      const Vec3 normal = {ca * cb, sb, sa * cb};
      const Vec3 pos = {(major + minor * cb) * ca, minor * sb, (major + minor * cb) * sa};
      m.vertices.push_back({pos, normal, {u * 8, v * 2}});
    }
  }
  const int stride = segments + 1;
  for (int i = 0; i < rings; ++i)
    for (int j = 0; j < segments; ++j) {
      const uint32_t a = static_cast<uint32_t>(i * stride + j), b = a + 1;
      const uint32_t c = a + static_cast<uint32_t>(stride), d = c + 1;
      m.indices.insert(m.indices.end(), {a, b, c, b, d, c});
    }
  return m;
}

Mesh make_sphere(int stacks, int slices, float radius) {
  Mesh m;
  for (int i = 0; i <= stacks; ++i) {
    const float v = static_cast<float>(i) / stacks, phi = v * kPi;
    for (int j = 0; j <= slices; ++j) {
      const float u = static_cast<float>(j) / slices, theta = u * 2 * kPi;
      const Vec3 n = {std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
      m.vertices.push_back({n * radius, n, {u, v}});
    }
  }
  const int stride = slices + 1;
  for (int i = 0; i < stacks; ++i)
    for (int j = 0; j < slices; ++j) {
      const uint32_t a = static_cast<uint32_t>(i * stride + j), b = a + 1;
      const uint32_t c = a + static_cast<uint32_t>(stride), d = c + 1;
      if (i > 0) m.indices.insert(m.indices.end(), {a, b, c});           // row 0 is the pole
      if (i + 1 < stacks) m.indices.insert(m.indices.end(), {b, d, c});  // last row is the other pole
    }
  return m;
}

}  // namespace softgpu
