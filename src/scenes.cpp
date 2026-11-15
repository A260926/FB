#include "softgpu/scenes.hpp"

#include <algorithm>
#include <random>

#include "softgpu/mesh.hpp"

namespace softgpu {

namespace {

Mat4 camera(const Renderer& r, Vec3 eye, Vec3 at) {
  const float aspect = static_cast<float>(r.fb().width) / static_cast<float>(r.fb().height);
  return Mat4::perspective(kPi / 3, aspect, 0.5f, 50.0f) * Mat4::look_at(eye, at, {0, 1, 0});
}

void draw_mesh(Renderer& r, const Mesh& m, const Mat4& proj_view, const Mat4& model, const LitFragment& fs,
               const DrawState& st = DrawState{}) {
  r.draw<5>(m.vertices.data(), m.vertices.size(), m.indices.data(), m.indices.size(), LitVertex{proj_view * model, model},
            fs, st);
}

void scene_cube(Renderer& r, const SceneOptions& o) {
  static const Texture tex = Texture::checkerboard(256, 256, 32, Vec4(0.95f, 0.85f, 0.35f, 1), Vec4(0.2f, 0.3f, 0.7f, 1));
  r.clear(Vec4(0.08f, 0.09f, 0.12f, 1));
  const Mat4 pv = camera(r, {0, 1.2f, 3.2f}, {0, 0, 0});
  const Mat4 model = Mat4::rotate_y(o.angle) * Mat4::rotate_x(o.angle * 0.5f);
  draw_mesh(r, make_cube(), pv, model, LitFragment{&tex, o.filter});
  // A floor under it so mip selection is visible (the checker recedes to the far plane).
  static const Texture floor = Texture::mip_debug(512);
  const Mesh q = make_quad({-12, -1.2f, 12}, {24, 0, 0}, {0, 0, -24});
  draw_mesh(r, q, pv, Mat4::identity(), LitFragment{&floor, o.filter});
  r.finish();
}

void scene_torus(Renderer& r, const SceneOptions& o) {
  r.clear(Vec4(0.08f, 0.09f, 0.12f, 1));
  const Mat4 pv = camera(r, {0, 2.0f, 4.2f}, {0, 0, 0});
  const Mat4 model = Mat4::rotate_y(o.angle) * Mat4::rotate_x(0.9f);
  const Mesh m = make_torus(96 * o.detail, 48 * o.detail, 1.2f, 0.5f);
  draw_mesh(r, m, pv, model, LitFragment{});
  r.finish();
}

void scene_sphere(Renderer& r, const SceneOptions& o) {
  static const Texture tex = Texture::checkerboard(512, 256, 32, Vec4(0.9f, 0.9f, 0.9f, 1), Vec4(0.8f, 0.2f, 0.2f, 1));
  r.clear(Vec4(0.08f, 0.09f, 0.12f, 1));
  const Mat4 pv = camera(r, {0, 1.0f, 3.6f}, {0, 0, 0});
  const Mat4 model = Mat4::rotate_y(o.angle) * Mat4::rotate_z(0.3f);
  const Mesh m = make_sphere(64 * o.detail, 128 * o.detail, 1.3f);
  draw_mesh(r, m, pv, model, LitFragment{&tex, o.filter});
  r.finish();
}

// Thousands of random triangles at random depths, all overlapping the middle of the screen,
// drawn in random order: the depth buffer has to sort them.
void scene_depth(Renderer& r, const SceneOptions& o) {
  r.clear(Vec4(0.02f, 0.02f, 0.03f, 1));
  std::mt19937 rng(o.seed);
  std::uniform_real_distribution<float> pos(-1.0f, 1.0f), z(-1.0f, 1.0f), size(0.05f, 0.5f), col(0.2f, 1.0f);
  Mesh m;
  for (int i = 0; i < o.count; ++i) {
    const Vec3 c = {pos(rng) * 1.5f, pos(rng) * 1.0f, z(rng) * 4.0f};
    const float s = size(rng);
    const Vec3 tint = {col(rng), col(rng), col(rng)};
    const uint32_t base = static_cast<uint32_t>(m.vertices.size());
    for (int k = 0; k < 3; ++k) {
      const Vec3 p = c + Vec3{pos(rng) * s, pos(rng) * s, pos(rng) * 0.1f};
      m.vertices.push_back({p, tint, {0, 0}});  // normal slot carries the flat color
    }
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2});
  }
  const Mat4 pv = camera(r, {0, 0, 7.0f}, {0, 0, 0});
  struct FlatVS {
    Mat4 mvp;
    VsOut<3> operator()(const Vertex& v) const {
      VsOut<3> out;
      out.pos = mvp * Vec4(v.pos, 1.0f);
      out.attr[0] = v.normal.x;
      out.attr[1] = v.normal.y;
      out.attr[2] = v.normal.z;
      return out;
    }
  };
  struct FlatFS {
    bool operator()(const Fragment<3>& f, Vec4& c) const {
      // Depth-tinted so the image reads as a depth field: nearer is brighter.
      const float k = 1.2f - f.z * 0.8f;
      c = Vec4(f.attr[0] * k, f.attr[1] * k, f.attr[2] * k, 1.0f);
      return true;
    }
  };
  DrawState st;
  st.cull = CullMode::None;
  r.draw<3>(m.vertices.data(), m.vertices.size(), m.indices.data(), m.indices.size(), FlatVS{pv}, FlatFS{}, st);
  r.finish();
}

// Textured floor with translucent quads sorted back to front and blended over it.
void scene_blend(Renderer& r, const SceneOptions& o) {
  static const Texture tex = Texture::checkerboard(256, 256, 32, Vec4(0.85f, 0.85f, 0.85f, 1), Vec4(0.3f, 0.3f, 0.35f, 1));
  r.clear(Vec4(0.08f, 0.09f, 0.12f, 1));
  const Mat4 pv = camera(r, {0, 2.5f, 5.0f}, {0, 0.3f, 0});
  draw_mesh(r, make_quad({-3, -0.5f, 3}, {6, 0, 0}, {0, 0, -6}), pv, Mat4::identity(), LitFragment{&tex, o.filter});
  struct TintVS {
    Mat4 mvp;
    VsOut<0> operator()(const Vertex& v) const {
      VsOut<0> out;
      out.pos = mvp * Vec4(v.pos, 1.0f);
      return out;
    }
  };
  struct TintFS {
    Vec4 color;
    bool operator()(const Fragment<0>&, Vec4& c) const {
      c = color;
      return true;
    }
  };
  DrawState st;
  st.cull = CullMode::None;
  st.blend = true;
  st.depth_write = false;
  const Vec4 colors[] = {{1, 0.2f, 0.2f, 0.5f}, {0.2f, 1, 0.2f, 0.5f}, {0.2f, 0.4f, 1, 0.5f}, {1, 1, 0.2f, 0.4f}};
  for (int i = 0; i < 4; ++i) {  // back to front along -z
    const float zq = -1.5f + i * 1.0f;
    const Mesh q = make_quad({-1.2f + 0.4f * i, -0.5f, zq}, {2.0f, 0, 0}, {0, 1.6f, 0});
    r.draw<0>(q.vertices.data(), q.vertices.size(), q.indices.data(), q.indices.size(),
              TintVS{pv * Mat4::rotate_y(o.angle * 0.2f)}, TintFS{colors[i]}, st);
  }
  r.finish();
}

// Fill-rate scene: `count` full-screen textured layers with the depth test off, so every
// fragment of every layer is shaded and blended over the previous one.
void scene_fill(Renderer& r, const SceneOptions& o) {
  static const Texture tex = Texture::checkerboard(1024, 1024, 64, Vec4(0.9f, 0.6f, 0.2f, 1), Vec4(0.2f, 0.5f, 0.9f, 1));
  r.clear(Vec4(0.08f, 0.09f, 0.12f, 1));
  DrawState st;
  st.cull = CullMode::None;
  st.depth_test = false;
  st.depth_write = false;
  const int layers = std::max(1, o.count / 250);
  for (int i = 0; i < layers; ++i) {
    const float s = 1.0f + 0.1f * i;
    const Mesh q = make_quad({-s, -s, 0}, {2 * s, 0, 0}, {0, 2 * s, 0});
    const Mat4 model = Mat4::rotate_z(o.angle * 0.1f * i) * Mat4::scale({1, 1, 1});
    draw_mesh(r, q, Mat4::identity(), model, LitFragment{&tex, o.filter}, st);
  }
  r.finish();
}

}  // namespace

std::vector<std::string> scene_names() { return {"cube", "torus", "sphere", "depth", "blend", "fill"}; }

bool render_scene(Renderer& r, const SceneOptions& o) {
  if (o.name == "cube") scene_cube(r, o);
  else if (o.name == "torus") scene_torus(r, o);
  else if (o.name == "sphere") scene_sphere(r, o);
  else if (o.name == "depth") scene_depth(r, o);
  else if (o.name == "blend") scene_blend(r, o);
  else if (o.name == "fill") scene_fill(r, o);
  else return false;
  return true;
}

}  // namespace softgpu
