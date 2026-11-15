#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "softgpu/mesh.hpp"
#include "softgpu/renderer.hpp"
#include "softgpu/texture.hpp"

namespace softgpu {

struct SceneOptions {
  std::string name = "cube";
  float angle = 0.6f;                // rotation of the model, radians
  int detail = 1;                    // mesh density multiplier for torus / sphere
  int count = 2000;                  // triangles in the depth stress scene; count/250 layers in fill
  Filter filter = Filter::Trilinear;
  unsigned seed = 7;
};

// Lambert-lit shaders shared by the demos, the bench and the golden tests.
struct LitVertex {
  Mat4 mvp;
  Mat4 model;  // rotation + uniform scale only, so normals go through the 3x3
  VsOut<5> operator()(const Vertex& v) const {
    VsOut<5> o;
    o.pos = mvp * Vec4(v.pos, 1.0f);
    const Vec3 n = model.transform_dir(v.normal);
    o.attr[0] = n.x;
    o.attr[1] = n.y;
    o.attr[2] = n.z;
    o.attr[3] = v.uv.x;
    o.attr[4] = v.uv.y;
    return o;
  }
};

struct LitFragment {
  const Texture* tex = nullptr;  // null -> albedo from the normal
  Filter filter = Filter::Trilinear;
  Vec3 light = normalize({0.4f, 0.8f, 0.6f});
  bool operator()(const Fragment<5>& f, Vec4& c) const {
    const Vec3 n = normalize({f.attr[0], f.attr[1], f.attr[2]});
    const float diffuse = std::max(0.0f, dot(n, light));
    Vec3 albedo = {0.5f + 0.5f * n.x, 0.5f + 0.5f * n.y, 0.5f + 0.5f * n.z};
    if (tex) {
      const Vec4 t = tex->sample_grad({f.attr[3], f.attr[4]}, {f.ddx[3], f.ddx[4]}, {f.ddy[3], f.ddy[4]}, filter);
      albedo = t.xyz();
    }
    const Vec3 shade = albedo * (0.15f + 0.85f * diffuse);
    c = Vec4(shade, 1.0f);
    return true;
  }
};

std::vector<std::string> scene_names();
// Draws the named scene into r (clear + draws + finish). Returns false for an unknown name.
bool render_scene(Renderer& r, const SceneOptions& opt);

}  // namespace softgpu
