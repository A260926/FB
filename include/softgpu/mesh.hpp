#pragma once
#include <cstdint>
#include <vector>

#include "softgpu/math.hpp"

namespace softgpu {

struct Vertex {
  Vec3 pos;
  Vec3 normal;
  Vec2 uv;
};

// Indexed triangle list, counter-clockwise seen from outside (front faces).
struct Mesh {
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  size_t triangles() const { return indices.size() / 3; }
};

Mesh make_cube();  // unit cube centred at the origin, 24 vertices with per-face normals and uvs
Mesh make_torus(int rings, int segments, float major, float minor);
Mesh make_sphere(int stacks, int slices, float radius);
Mesh make_quad(Vec3 origin, Vec3 du, Vec3 dv);  // one-sided quad, normal = du x dv

}  // namespace softgpu
