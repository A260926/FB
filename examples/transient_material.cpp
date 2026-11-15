// Transient material demo: geometry writes an HDR material attachment in subpass 0, later
// subpasses read it and derive new attachments, and dead attachments give their physical
// slots back before the final shaded resolve. Five logical attachments compile to two
// physical RGBA32F slots. All geometry is generated in memory; output is a P6 PPM at the
// path given on the command line.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "softgpu/image.hpp"
#include "softgpu/renderer.hpp"

using namespace softgpu;

namespace {

struct Vertex {
  Vec4 p;
  float heat;
};

struct VS {
  VsOut<1> operator()(const Vertex& v) const {
    VsOut<1> o;
    o.pos = v.p;
    o.attr[0] = v.heat;
    return o;
  }
};

// Logical attachment ids.
constexpr int kMaterial = 0;  // sp0 out, sp1 in   : interval [0,1]
constexpr int kAux = 1;       // sp0 out only      : interval [0,0] -> slot reused from sp1
constexpr int kDerived = 2;   // sp1 out, sp2 in   : interval [1,2] (reuses kAux's slot)
constexpr int kLight = 3;     // sp2 out, sp3 in   : interval [2,3] (reuses kMaterial's slot)
constexpr int kFinal = 4;     // sp3 out, final    : interval [3,3] (reuses kDerived's slot)

struct MaterialFS {
  bool operator()(const Fragment<1>& f, PassFragmentIO& io) const {
    // Out-of-gamut material: a negative channel and a value above 1 must survive to sp1.
    io.out(0) = Vec4(-0.8f, 1.6f, f.attr[0], 1.0f);  // kMaterial
    io.out(1) = Vec4(f.attr[0] * 2.0f, 0.25f, 0.25f, 1.0f);  // kAux, dead after this subpass
    return true;
  }
};

struct DeriveFS {
  bool operator()(const Fragment<1>& f, PassFragmentIO& io) const {
    const Vec4 m = io.in(0);  // kMaterial, still full-float (negative and HDR intact)
    if ((f.x * 7 + f.y * 13) % 11 == 0) return false;  // periodic discard
    io.out(0) = Vec4(m.x + 1.0f, m.y * 2.0f, m.z, 1.0f);  // kDerived into kAux's old slot
    return true;
  }
};

struct LightFS {
  bool operator()(const Fragment<1>&, PassFragmentIO& io) const {
    const Vec4 d = io.in(0);  // kDerived
    const float lambert = 0.35f + 0.65f * (0.5f + 0.5f * d.z);
    io.out(0) = Vec4(d.x * lambert, d.y * lambert, d.z * lambert, 1.0f);  // kLight
    return true;
  }
};

struct FinalFS {
  bool operator()(const Fragment<1>&, PassFragmentIO& io) const {
    io.out(0) = io.in(0);  // resolve kLight into kFinal; to_u8 clamps at writeback
    return true;
  }
};

DrawState no_cull() {
  DrawState st;
  st.cull = CullMode::None;
  return st;
}

// Resolve subpasses re-walk the same geometry to read back materials, so the depth test is
// off: an equal-depth resubmit must not fail the strict z < depth test.
DrawState resolve_state() {
  DrawState st;
  st.cull = CullMode::None;
  st.depth_test = false;
  return st;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: transient_material out.ppm\n");
    return 1;
  }

  RenderPassPlan desc;
  desc.physical_slots = 2;
  desc.clear_values = {
      {kMaterial, Vec4(0.05f, 0.05f, 0.08f, 1.0f)},
      {kAux, Vec4(0.0f, 0.0f, 0.0f, 1.0f)},
      {kDerived, Vec4(0.1f, 0.1f, 0.1f, 1.0f)},
      {kLight, Vec4(0.02f, 0.03f, 0.05f, 1.0f)},
      {kFinal, Vec4(0.01f, 0.01f, 0.02f, 1.0f)},
  };
  desc.subpasses = {
      {{}, {kMaterial, kAux}},  // sp0: geometry writes material plus a short-lived auxiliary
      {{kMaterial}, {kDerived}},
      {{kDerived}, {kLight}},
      {{kLight}, {kFinal}},
  };
  desc.final_output = kFinal;
  const auto plan = compile_render_pass(desc);

  // In-memory scene: two overlapping triangles at different depths.
  const std::vector<Vertex> verts = {
      {{-0.95f, -0.9f, 0.3f, 1.0f}, 0.2f},
      {{0.95f, -0.9f, 0.3f, 1.0f}, 0.8f},
      {{0.0f, 0.95f, 0.3f, 1.0f}, 0.5f},
      {{-0.7f, -0.95f, 0.7f, 1.0f}, 0.9f},
      {{0.7f, -0.95f, 0.7f, 1.0f}, 0.1f},
      {{0.0f, 0.2f, 0.7f, 1.0f}, 0.4f},
  };
  const uint32_t idx[6] = {0, 1, 2, 3, 4, 5};

  Renderer r(64, 48, Config{Mode::Tiled, 3, 16});
  r.clear(Vec4(0, 0, 0, 1), 1.0f);
  r.begin_pass(*plan);
  r.subpass_draw<1>(verts.data(), verts.size(), idx, 6, VS{}, MaterialFS{}, no_cull());
  r.next_subpass();
  r.subpass_draw<1>(verts.data(), verts.size(), idx, 6, VS{}, DeriveFS{}, resolve_state());
  r.next_subpass();
  r.subpass_draw<1>(verts.data(), verts.size(), idx, 6, VS{}, LightFS{}, resolve_state());
  r.next_subpass();
  r.subpass_draw<1>(verts.data(), verts.size(), idx, 6, VS{}, FinalFS{}, resolve_state());
  r.end_pass();

  if (!write_ppm(argv[1], r.fb().color)) {
    std::fprintf(stderr, "cannot write %s\n", argv[1]);
    return 1;
  }
  std::printf("wrote %s: attachments=%d min_slots=%d peak_pixels=%zu\n", argv[1],
              static_cast<int>(desc.clear_values.size()), plan->info.min_slots, r.transient_peak_pixels());
  return 0;
}
