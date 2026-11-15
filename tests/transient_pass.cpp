// Transient render-pass tests: plan validation and interval-based slot aliasing, then real
// tile-local draws exercising HDR intermediates, discard, shared depth, mixed shaders and
// byte-identical output across thread counts, tile sizes and edge tiles.
#include "task_support/check.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <vector>

#include "softgpu/renderer.hpp"

using namespace softgpu;

namespace {

// --- Test geometry: clip-space vertices with a scalar/vector varying, generated in memory. ---
struct V1 {
  Vec4 p;
  float a;
};
struct V2 {
  Vec4 p;
  float a, b;
};

struct VS1 {
  VsOut<1> operator()(const V1& v) const {
    VsOut<1> o;
    o.pos = v.p;
    o.attr[0] = v.a;
    return o;
  }
};
struct VS2 {
  VsOut<2> operator()(const V2& v) const {
    VsOut<2> o;
    o.pos = v.p;
    o.attr[0] = v.a;
    o.attr[1] = v.b;
    return o;
  }
};

// A big triangle covering most of the framebuffer and a small one in the bottom-left corner
// so edge tiles are exercised on framebuffers whose dimensions do not divide by tile size.
std::vector<V1> big_tri(float z, float a) {
  return {{{-0.9f, -0.9f, z, 1.0f}, a}, {{0.9f, -0.9f, z, 1.0f}, a}, {{0.0f, 0.9f, z, 1.0f}, a}};
}
std::vector<V1> corner_tri(float z, float a) {
  return {{{-1.0f, -1.0f, z, 1.0f}, a}, {{-0.4f, -1.0f, z, 1.0f}, a}, {{-1.0f, -0.4f, z, 1.0f}, a}};
}
const uint32_t kTriIdx[3] = {0, 1, 2};

DrawState no_cull() {
  DrawState st;
  st.cull = CullMode::None;
  return st;
}

// Later subpasses re-draw the same surfaces to read back materials: like a deferred resolve
// they keep cull/write settings but switch the depth test off, so an equal-depth resubmit is
// not rejected by the strict z < depth rule.
DrawState resolve_state() {
  DrawState st;
  st.cull = CullMode::None;
  st.depth_test = false;
  return st;
}

bool expect_invalid(const std::function<void()>& fn) {
  try {
    fn();
  } catch (const std::invalid_argument&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

bool expect_logic(const std::function<void()>& fn) {
  try {
    fn();
  } catch (const std::logic_error&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

RenderPassPlan make_desc() {
  RenderPassPlan d;
  d.clear_values = {{0, Vec4(0.1f, 0.2f, 0.3f, 1.0f)}, {1, Vec4(0.0f, 0.0f, 0.0f, 1.0f)}};
  d.subpasses = {{{}, {0}}, {{0}, {1}}};
  d.final_output = 1;
  d.physical_slots = 2;
  return d;
}

// ---------------- planning group ----------------

int plan_valid_compiles() {
  RenderPassPlan d = make_desc();
  auto plan = compile_render_pass(d);
  TASK_CHECK(plan->info.compiled);
  TASK_CHECK(plan->info.min_slots == 2);
  TASK_CHECK(plan->info.slot_for.at(0) == 0);
  TASK_CHECK(plan->info.slot_for.at(1) == 1);
  TASK_CHECK(plan->info.first_output.at(0) == 0);
  TASK_CHECK(plan->info.first_output.at(1) == 1);
  return 0;
}

int plan_undeclared_reference() {
  RenderPassPlan d = make_desc();
  d.subpasses[0].outputs.push_back(7);  // no clear value for id 7
  TASK_CHECK(expect_invalid([&] { compile_render_pass(d); }));
  RenderPassPlan d2 = make_desc();
  d2.subpasses[1].inputs.push_back(7);
  TASK_CHECK(expect_invalid([&] { compile_render_pass(d2); }));
  return 0;
}

int plan_feedback_loop() {
  RenderPassPlan d = make_desc();
  d.subpasses[0].inputs.push_back(0);  // id 0 is also the output of subpass 0
  TASK_CHECK(expect_invalid([&] { compile_render_pass(d); }));
  return 0;
}

int plan_read_undefined() {
  RenderPassPlan d = make_desc();
  d.clear_values[2] = Vec4(0, 0, 0, 1);
  d.subpasses[0].inputs.push_back(2);  // never output before subpass 0
  TASK_CHECK(expect_invalid([&] { compile_render_pass(d); }));
  return 0;
}

int plan_duplicate_ids() {
  RenderPassPlan d = make_desc();
  d.subpasses[0].outputs.push_back(0);
  TASK_CHECK(expect_invalid([&] { compile_render_pass(d); }));
  RenderPassPlan d2 = make_desc();
  d2.subpasses[1].inputs.push_back(0);
  TASK_CHECK(expect_invalid([&] { compile_render_pass(d2); }));
  return 0;
}

int plan_final_undefined() {
  RenderPassPlan d = make_desc();
  d.final_output = 9;  // declared nowhere
  TASK_CHECK(expect_invalid([&] { compile_render_pass(d); }));
  RenderPassPlan d2 = make_desc();
  d2.clear_values[3] = Vec4(0, 0, 0, 1);
  d2.final_output = 3;  // declared with a clear value but never produced
  TASK_CHECK(expect_invalid([&] { compile_render_pass(d2); }));
  return 0;
}

int plan_slot_budget() {
  // Two simultaneously live attachments need two slots: one must be refused.
  RenderPassPlan d = make_desc();
  d.physical_slots = 1;
  TASK_CHECK(expect_invalid([&] { compile_render_pass(d); }));
  // The minimum is computed from liveness, not the attachment count: six attachments whose
  // intervals stagger so only three are ever live together fit a budget of three.
  RenderPassPlan six;
  six.physical_slots = 3;
  six.clear_values = {{0, {}}, {1, {}}, {2, {}}, {3, {}}, {4, {}}, {5, {}}};
  six.subpasses = {{{}, {0, 1}}, {{0}, {2}}, {{1, 2}, {3}}, {{3}, {4}}, {{4}, {5}}};
  six.final_output = 5;
  auto plan = compile_render_pass(six);
  TASK_CHECK(plan->info.min_slots == 3);
  TASK_CHECK(expect_invalid([&] {
    auto copy = six;
    copy.physical_slots = 2;
    compile_render_pass(copy);
  }));
  return 0;
}

int plan_empty_subpass_list() {
  RenderPassPlan d = make_desc();
  d.subpasses.clear();
  TASK_CHECK(expect_invalid([&] { compile_render_pass(d); }));
  return 0;
}

int plan_unused_attachment_uses_no_slot() {
  RenderPassPlan d = make_desc();
  d.clear_values[42] = Vec4(1, 2, 3, 1);  // declared, never referenced
  auto plan = compile_render_pass(d);
  TASK_CHECK(plan->info.slot_for.count(42) == 0);
  TASK_CHECK(plan->info.min_slots == 2);
  return 0;
}

int plan_bad_then_good_leaves_renderer_clean() {
  Renderer r(48, 40, Config{Mode::Tiled, 2, 16});
  r.clear(Vec4(0, 0, 0, 1));
  RenderPassPlan bad = make_desc();
  bad.physical_slots = 1;
  TASK_CHECK(expect_invalid([&] { compile_render_pass(bad); }));
  // No half-pass: a normal pass still runs and produces the material flow below.
  auto plan = compile_render_pass(make_desc());
  struct MatFS {
    bool operator()(const Fragment<1>& f, PassFragmentIO& io) const {
      io.out(0) = Vec4(f.attr[0], 0.0f, 0.0f, 1.0f);
      return true;
    }
  };
  struct FinalFS {
    bool operator()(const Fragment<1>&, PassFragmentIO& io) const {
      io.out(0) = io.in(0);
      return true;
    }
  };
  r.begin_pass(*plan);
  auto tri = big_tri(0.2f, 0.75f);
  r.subpass_draw<1>(tri.data(), 3, kTriIdx, 3, VS1{}, MatFS{}, no_cull());
  r.next_subpass();
  r.subpass_draw<1>(tri.data(), 3, kTriIdx, 3, VS1{}, FinalFS{}, resolve_state());
  r.end_pass();
  int shaded = 0;
  for (int y = 0; y < 40; ++y)
    for (int x = 0; x < 48; ++x) {
      const uint8_t* p = r.fb().color.at(x, y);
      if (p[0] == to_u8(0.75f)) ++shaded;
    }
  TASK_CHECK(shaded > 100);
  return 0;
}

int plan_ordering_errors() {
  Renderer r(48, 40, Config{Mode::Tiled, 2, 16});
  r.clear(Vec4(0, 0, 0, 1));
  auto plan = compile_render_pass(make_desc());
  // end / next / subpass_draw outside an active pass.
  TASK_CHECK(expect_logic([&] { r.end_pass(); }));
  TASK_CHECK(expect_logic([&] { r.next_subpass(); }));
  auto tri = big_tri(0.2f, 0.5f);
  TASK_CHECK(expect_logic([&] {
    r.subpass_draw<1>(tri.data(), 3, kTriIdx, 3, VS1{},
                      [](const Fragment<1>&, PassFragmentIO& io) {
                        io.out(0) = Vec4(1, 1, 1, 1);
                        return true;
                      },
                      no_cull());
  }));
  r.begin_pass(*plan);
  // No nesting, no ordinary framebuffer commands while the pass owns the renderer.
  TASK_CHECK(expect_logic([&] { r.begin_pass(*plan); }));
  TASK_CHECK(expect_logic([&] { r.clear(Vec4(0, 0, 0, 1)); }));
  TASK_CHECK(expect_logic([&] { r.finish(); }));
  TASK_CHECK(expect_logic([&] {
    r.draw<1>(tri.data(), 3, kTriIdx, 3, VS1{},
              [](const Fragment<1>&, Vec4& c) {
                c = Vec4(1, 1, 1, 1);
                return true;
              },
              no_cull());
  }));
  r.next_subpass();
  TASK_CHECK(expect_logic([&] { r.next_subpass(); }));  // past the last subpass
  r.end_pass();
  TASK_CHECK(expect_logic([&] { r.end_pass(); }));
  // Ordinary tiled draws must be finished before another pass begins.
  r.draw<1>(tri.data(), 3, kTriIdx, 3, VS1{},
            [](const Fragment<1>&, Vec4& c) {
              c = Vec4(1, 1, 1, 1);
              return true;
            },
            no_cull());
  TASK_CHECK(expect_logic([&] { r.begin_pass(*plan); }));
  r.finish();
  r.begin_pass(*plan);  // now legal
  r.end_pass();
  return 0;
}

int plan_blend_and_outputless_rejected() {
  Renderer r(48, 40, Config{Mode::Tiled, 1, 16});
  r.clear(Vec4(0, 0, 0, 1));
  RenderPassPlan d;
  d.physical_slots = 1;
  d.clear_values = {{0, Vec4(0, 0, 0, 1)}};
  d.subpasses = {{{}, {0}}, {{0}, {}}};  // subpass 1 has no outputs
  d.final_output = 0;
  auto plan = compile_render_pass(d);
  auto tri = big_tri(0.2f, 0.5f);
  r.begin_pass(*plan);
  DrawState blended = no_cull();
  blended.blend = true;
  TASK_CHECK(expect_invalid([&] {
    r.subpass_draw<1>(tri.data(), 3, kTriIdx, 3, VS1{},
                      [](const Fragment<1>&, PassFragmentIO& io) {
                        io.out(0) = Vec4(1, 1, 1, 1);
                        return true;
                      },
                      blended);
  }));
  r.next_subpass();
  TASK_CHECK(expect_logic([&] {
    r.subpass_draw<1>(tri.data(), 3, kTriIdx, 3, VS1{},
                      [](const Fragment<1>&, PassFragmentIO&) { return true; }, no_cull());
  }));
  r.end_pass();
  return 0;
}

// ---------------- aliasing group ----------------

int alias_six_attachments_three_slots() {
  RenderPassPlan d;
  d.physical_slots = 3;
  d.clear_values = {{0, {}}, {1, {}}, {2, {}}, {3, {}}, {4, {}}, {5, {}}};
  // Liveness (closed intervals): 0[0,1] 1[0,2] 2[1,2] 3[2,3] 4[3,4] 5[4,4]. At most three
  // are ever live together, and the freed slots are reused, so the minimum is three.
  d.subpasses = {
      {{}, {0, 1}},          // sp0: 0,1 born
      {{0}, {2}},            // sp1: 0 last used, 2 born (touch at 1 -> cannot share with 0)
      {{1, 2}, {3}},         // sp2: 1,2 last used, 3 born -> reuses the slot freed by 0 at sp1
      {{3}, {4}},            // sp3: 3 last used, 4 born
      {{4}, {5}},            // sp4: 4 last used, 5 born (final)
  };
  d.final_output = 5;
  auto plan = compile_render_pass(d);
  TASK_CHECK(plan->info.min_slots == 3);
  TASK_CHECK(plan->info.last_use.at(0) == 1);
  TASK_CHECK(plan->info.last_use.at(1) == 2);
  TASK_CHECK(plan->info.last_use.at(2) == 2);
  TASK_CHECK(plan->info.last_use.at(5) == 4);
  // No two simultaneously-live attachments share a slot.
  for (int a = 0; a < 6; ++a)
    for (int b = a + 1; b < 6; ++b) {
      const bool overlap = plan->info.first_output.at(b) <= plan->info.last_use.at(a) &&
                           plan->info.first_output.at(a) <= plan->info.last_use.at(b);
      if (overlap) TASK_CHECK(plan->info.slot_for.at(a) != plan->info.slot_for.at(b));
    }
  // A slot is actually reused by an attachment born after another died (id 3/4/5 cycle).
  std::vector<int> used;
  for (int a = 0; a < 6; ++a) used.push_back(plan->info.slot_for.at(a));
  std::sort(used.begin(), used.end());
  used.erase(std::unique(used.begin(), used.end()), used.end());
  TASK_CHECK(used.size() == 3);
  return 0;
}

int alias_equal_endpoints_never_share() {
  // Attachment 0 is born in sp0 and read again in the non-adjacent sp2, so its closed interval
  // is [0,2]; attachment 1 is born exactly at sp2. They touch at endpoint 2 and must not alias.
  // Attachment 2 lives [1,1] only, so attachment 1 reuses its slot and the plan still fits two
  // physical slots -- but one slot must be refused.
  RenderPassPlan d;
  d.physical_slots = 2;
  d.clear_values = {{0, {}}, {1, {}}, {2, {}}};
  d.subpasses = {{{}, {0}}, {{}, {2}}, {{0}, {1}}};
  d.final_output = 1;
  auto plan = compile_render_pass(d);
  TASK_CHECK(plan->info.slot_for.at(0) != plan->info.slot_for.at(1));
  TASK_CHECK(plan->info.min_slots == 2);
  // id2 dies strictly before id1 is born (1 < 2), so those two do share a slot.
  TASK_CHECK(plan->info.slot_for.at(2) == plan->info.slot_for.at(1));
  d.physical_slots = 1;
  TASK_CHECK(expect_invalid([&] { compile_render_pass(d); }));
  return 0;
}

int alias_disjoint_intervals_share() {
  // Three output-only attachments born in successive subpasses: intervals [0,0], [1,1] and
  // [2,2], each separated by a strict gap at the boundary, so one physical slot serves all.
  RenderPassPlan d;
  d.physical_slots = 1;
  d.clear_values = {{0, {}}, {1, {}}, {2, {}}};
  d.subpasses = {{{}, {0}}, {{}, {1}}, {{}, {2}}};
  d.final_output = 2;
  auto plan = compile_render_pass(d);
  TASK_CHECK(plan->info.min_slots == 1);
  TASK_CHECK(plan->info.slot_for.at(0) == plan->info.slot_for.at(2));
  return 0;
}

int alias_final_extends_lifetime() {
  // id 0's last write is subpass 0, but as the final output it must stay alive to the end and
  // therefore cannot give its slot to id 1 born at subpass 1.
  RenderPassPlan d;
  d.physical_slots = 2;
  d.clear_values = {{0, {}}, {1, {}}};
  d.subpasses = {{{}, {0}}, {{}, {1}}};
  d.final_output = 0;
  auto plan = compile_render_pass(d);
  TASK_CHECK(plan->info.last_use.at(0) == 1);
  TASK_CHECK(plan->info.slot_for.at(0) != plan->info.slot_for.at(1));
  TASK_CHECK(plan->info.min_slots == 2);
  return 0;
}

int alias_storage_peak_bound() {
  RenderPassPlan d;
  d.physical_slots = 3;
  // Intervals: 0[0,1] 1[0,0] 2[1,2] 3[2,2]. Slot of id1 is freed before id2 is born, so the
  // true minimum is two slots even though four attachments are declared.
  d.clear_values = {{0, {}}, {1, {}}, {2, {}}, {3, {}}};
  d.subpasses = {{{}, {0, 1}}, {{0}, {2}}, {{2}, {3}}};
  d.final_output = 3;
  auto plan = compile_render_pass(d);
  TASK_CHECK(plan->info.min_slots == 2);
  const int tile = 16;
  for (int threads : {1, 3}) {
    Renderer r(64, 64, Config{Mode::Tiled, threads, tile});
    r.clear(Vec4(0, 0, 0, 1));
    r.begin_pass(*plan);
    auto tri = big_tri(0.2f, 0.5f);
    for (int s = 0; s < 3; ++s) {
      r.subpass_draw<1>(tri.data(), 3, kTriIdx, 3, VS1{},
                        [](const Fragment<1>& f, PassFragmentIO& io) {
                          for (int i = 0; i < io.output_count(); ++i)
                            io.out(i) = Vec4(f.attr[0], f.attr[0], f.attr[0], 1);
                          return true;
                        },
                        s == 0 ? no_cull() : resolve_state());
      if (s < 2) r.next_subpass();
    }
    r.end_pass();
    const size_t workers = static_cast<size_t>(threads);
    const size_t bound = workers * static_cast<size_t>(plan->info.min_slots) * tile * tile;
    TASK_CHECK(r.transient_peak_pixels() <= bound);
    TASK_CHECK(r.transient_peak_pixels() == bound);  // enough tiles to occupy every worker
  }
  // Immediate mode is serial: exactly one worker's worth of pixel storage.
  Renderer ri(64, 64, Config{Mode::Immediate, 8, tile});
  ri.clear(Vec4(0, 0, 0, 1));
  ri.begin_pass(*plan);
  auto tri = big_tri(0.2f, 0.5f);
  for (int s = 0; s < 3; ++s) {
    ri.subpass_draw<1>(tri.data(), 3, kTriIdx, 3, VS1{},
                       [](const Fragment<1>& f, PassFragmentIO& io) {
                         for (int i = 0; i < io.output_count(); ++i)
                           io.out(i) = Vec4(f.attr[0], f.attr[0], f.attr[0], 1);
                         return true;
                       },
                       s == 0 ? no_cull() : resolve_state());
    if (s < 2) ri.next_subpass();
  }
  ri.end_pass();
  TASK_CHECK(ri.transient_peak_pixels() == static_cast<size_t>(plan->info.min_slots) * tile * tile);
  return 0;
}

int alias_reoutput_preserves_content() {
  // id 0 is written in sp0 and re-output in sp2; pixels not covered by sp2 geometry keep the
  // sp0 value because re-output never re-clears.
  RenderPassPlan d;
  d.physical_slots = 2;
  d.clear_values = {{0, Vec4(0.11f, 0.22f, 0.33f, 1)}, {1, {}}};
  d.subpasses = {{{}, {0}}, {{0}, {1}}, {{1}, {0}}};
  d.final_output = 0;
  auto plan = compile_render_pass(d);
  TASK_CHECK(plan->info.first_output.at(0) == 0);
  Renderer r(48, 40, Config{Mode::Tiled, 1, 16});
  r.clear(Vec4(0, 0, 0, 1));
  r.begin_pass(*plan);
  auto big = big_tri(0.2f, 0.9f);
  auto corner = corner_tri(0.3f, 0.1f);
  r.subpass_draw<1>(big.data(), 3, kTriIdx, 3, VS1{},
                    [](const Fragment<1>& f, PassFragmentIO& io) {
                      io.out(0) = Vec4(f.attr[0], 0.5f, 0.5f, 1);
                      return true;
                    },
                    no_cull());
  r.next_subpass();
  r.subpass_draw<1>(big.data(), 3, kTriIdx, 3, VS1{},
                    [](const Fragment<1>&, PassFragmentIO& io) {
                      io.out(0) = io.in(0) + Vec4(0.0f, 0.0f, 0.1f, 0.0f);
                      return true;
                    },
                    resolve_state());
  r.next_subpass();
  // Only the small corner triangle re-outputs id 0 (depth test off so it is not rejected by
  // the nearer big triangle); everything else must retain sp0's value.
  r.subpass_draw<1>(corner.data(), 3, kTriIdx, 3, VS1{},
                    [](const Fragment<1>& f, PassFragmentIO& io) {
                      io.out(0) = Vec4(0.0f, 0.0f, f.attr[0], 1);
                      return true;
                    },
                    resolve_state());
  r.end_pass();
  int retained = 0, corner_px = 0;
  for (int y = 0; y < 40; ++y)
    for (int x = 0; x < 48; ++x) {
      const uint8_t* p = r.fb().color.at(x, y);
      if (p[0] == to_u8(0.9f) && p[1] == to_u8(0.5f)) ++retained;
      if (p[2] == to_u8(0.1f) && p[0] == 0) ++corner_px;
    }
  TASK_CHECK(retained > 100);
  TASK_CHECK(corner_px > 0);
  return 0;
}

// ---------------- pipeline group ----------------

// Shared three-subpass material flow used by the cross-config consistency checks:
// sp0 writes a material attachment (HDR/negative values), sp1 reads it and derives a new
// attachment, sp2 does the final shaded resolve.
constexpr int kMat = 0, kDeriv = 1, kColor = 2;

RenderPassPlan material_plan(int slots) {
  RenderPassPlan d;
  d.physical_slots = slots;
  d.clear_values = {{kMat, Vec4(0.05f, 0.06f, 0.07f, 1.0f)},
                    {kDeriv, Vec4(0.0f, 0.0f, 0.0f, 1.0f)},
                    {kColor, Vec4(0.02f, 0.03f, 0.04f, 1.0f)}};
  d.subpasses = {{{}, {kMat}}, {{kMat}, {kDeriv}}, {{kDeriv}, {kColor}}};
  d.final_output = kColor;
  return d;
}

struct MaterialFS {
  std::atomic<int>* calls;
  bool operator()(const Fragment<1>& f, PassFragmentIO& io) const {
    ++*calls;
    // Deliberately out-of-gamut and negative: RGBA32F must preserve both.
    io.out(0) = Vec4(-1.5f, 2.5f, f.attr[0], 1.0f);
    return true;
  }
};
struct DeriveFS {
  std::atomic<int>* calls;
  bool operator()(const Fragment<1>&, PassFragmentIO& io) const {
    ++*calls;
    const Vec4 m = io.in(0);
    // Full-float intermediates: m.x = -1.5 would already be 0 after an 8-bit clamp, which
    // would make the red channel 2.0 (255) instead of 0.5 (128). The green channel stays
    // negative (2.5 - 3.0) to exercise clamp-to-0 at the final resolve; z goes HDR.
    io.out(0) = Vec4(m.x + 2.0f, m.y - 3.0f, m.z * 4.0f, 1.0f);
    return true;
  }
};
struct ShadeFS {
  std::atomic<int>* calls;
  bool operator()(const Fragment<1>&, PassFragmentIO& io) const {
    ++*calls;
    io.out(0) = io.in(0);  // final resolve clamps through to_u8 at writeback
    return true;
  }
};

struct PipeResult {
  std::vector<uint8_t> color;
  std::vector<float> depth;
  int mat_calls = 0, deriv_calls = 0, shade_calls = 0;
  uint64_t shaded = 0, discarded = 0, early_z = 0;
  size_t peak = 0;
};

PipeResult run_material_pipe(Mode mode, int threads, int tile, int w = 322, int h = 198) {
  Renderer r(w, h, Config{mode, threads, tile});
  r.clear(Vec4(0, 0, 0, 1), 1.0f);
  auto plan = compile_render_pass(material_plan(3));
  PipeResult res;
  std::atomic<int> mat_calls{0}, deriv_calls{0}, shade_calls{0};
  r.begin_pass(*plan);
  auto big = big_tri(0.3f, 0.4f);
  auto corner = corner_tri(0.6f, 0.8f);
  // Every subpass re-walks the same surfaces; depth handling itself is pinned by the discard
  // and occlusion cases, so this shared helper uses resolve-state (test off) everywhere and
  // each covered fragment is shaded exactly once per subpass.
  r.subpass_draw<1>(big.data(), 3, kTriIdx, 3, VS1{}, MaterialFS{&mat_calls}, resolve_state());
  r.subpass_draw<1>(corner.data(), 3, kTriIdx, 3, VS1{}, MaterialFS{&mat_calls}, resolve_state());
  r.next_subpass();
  r.subpass_draw<1>(big.data(), 3, kTriIdx, 3, VS1{}, DeriveFS{&deriv_calls}, resolve_state());
  r.subpass_draw<1>(corner.data(), 3, kTriIdx, 3, VS1{}, DeriveFS{&deriv_calls}, resolve_state());
  r.next_subpass();
  r.subpass_draw<1>(big.data(), 3, kTriIdx, 3, VS1{}, ShadeFS{&shade_calls}, resolve_state());
  r.subpass_draw<1>(corner.data(), 3, kTriIdx, 3, VS1{}, ShadeFS{&shade_calls}, resolve_state());
  r.end_pass();
  res.mat_calls = mat_calls.load();
  res.deriv_calls = deriv_calls.load();
  res.shade_calls = shade_calls.load();
  res.color = r.fb().color.rgba;
  res.depth = r.fb().depth;
  res.shaded = r.stats().raster.fragments_shaded;
  res.discarded = r.stats().raster.fragments_discarded;
  res.early_z = r.stats().raster.early_z_rejected;
  res.peak = r.transient_peak_pixels();
  return res;
}

int pipe_hdr_and_negative_intermediates() {
  PipeResult res = run_material_pipe(Mode::Tiled, 1, 16);
  int found_mid = 0, found_clamp_hi = 0, found_clamp_lo = 0;
  for (size_t i = 0; i < res.color.size(); i += 4) {
    if (res.color[i] == to_u8(0.5f)) ++found_mid;       // -1.5 + 2.0, impossible after 8-bit clamp
    if (res.color[i + 2] == 255) ++found_clamp_hi;      // material z * 4 exceeds 1 -> clamps high
    if (res.color[i + 1] == 0) ++found_clamp_lo;        // 2.5 - 3.0 is negative -> clamps low
  }
  TASK_CHECK(found_mid > 100);
  TASK_CHECK(found_clamp_hi > 100);
  TASK_CHECK(found_clamp_lo > 0);
  // The final clear colour survives on pixels no geometry covered (edge tiles included).
  const uint8_t* corner = res.color.data();  // top-right pixel is outside both triangles
  TASK_CHECK(corner[0] == to_u8(0.02f) && corner[1] == to_u8(0.03f) && corner[2] == to_u8(0.04f));
  return 0;
}

int pipe_callback_counts() {
  PipeResult res = run_material_pipe(Mode::Tiled, 2, 16);
  // Each subpass shades every covered fragment once; the three FS call counters sum to the
  // renderer's shaded invocation count.
  TASK_CHECK(res.mat_calls > 0 && res.deriv_calls > 0 && res.shade_calls > 0);
  TASK_CHECK(static_cast<uint64_t>(res.mat_calls + res.deriv_calls + res.shade_calls) == res.shaded);
  TASK_CHECK(res.mat_calls == res.deriv_calls);
  TASK_CHECK(res.deriv_calls == res.shade_calls);
  return 0;
}

int pipe_discard_preserves_outputs_and_depth() {
  Renderer r(64, 48, Config{Mode::Tiled, 2, 16});
  r.clear(Vec4(0, 0, 0, 1), 1.0f);
  const Vec4 c0(0.3f, 0.3f, 0.3f, 1.0f), c1(0.08f, 0.08f, 0.08f, 1.0f);
  RenderPassPlan d;
  d.physical_slots = 2;
  d.clear_values = {{0, c0}, {1, c1}};
  d.subpasses = {{{}, {0}}, {{0}, {1}}};
  d.final_output = 1;
  auto plan = compile_render_pass(d);
  r.begin_pass(*plan);
  auto near = big_tri(0.2f, 1.0f);
  struct DiscardFS {
    bool operator()(const Fragment<1>& f, PassFragmentIO& io) const {
      if ((f.x + f.y) % 4 == 0) return false;  // periodic holes: no output write, no depth write
      io.out(0) = Vec4(0.9f, 0.9f, 0.9f, 1);
      return true;
    }
  };
  r.subpass_draw<1>(near.data(), 3, kTriIdx, 3, VS1{}, DiscardFS{}, no_cull());
  r.next_subpass();
  // Same coverage, further away. Where the near fragment passed, depth 0.2 rejects this one
  // and id1 keeps its clear c1; where it discarded, depth is still 1.0 so this passes, reads
  // the preserved material clear c0 and copies it through, writing depth 0.8.
  auto behind = big_tri(0.8f, 1.0f);
  struct FarFS {
    bool operator()(const Fragment<1>&, PassFragmentIO& io) const {
      io.out(0) = io.in(0);
      return true;
    }
  };
  r.subpass_draw<1>(behind.data(), 3, kTriIdx, 3, VS1{}, FarFS{}, no_cull());
  r.end_pass();
  const uint64_t discarded = r.stats().raster.fragments_discarded;
  TASK_CHECK(discarded > 0);
  TASK_CHECK(r.stats().raster.early_z_rejected > 0);
  int holes = 0, kept = 0;
  for (int y = 0; y < 48; ++y)
    for (int x = 0; x < 64; ++x) {
      const uint8_t* p = r.fb().color.at(x, y);
      const bool is_c0 = p[0] == to_u8(c0.x) && p[1] == to_u8(c0.y) && p[2] == to_u8(c0.z);
      const bool is_c1 = p[0] == to_u8(c1.x) && p[1] == to_u8(c1.y) && p[2] == to_u8(c1.z);
      TASK_CHECK(is_c0 || is_c1);  // nothing else can appear
      const float z = r.fb().depth_at(x, y);
      if (is_c0) {
        ++holes;  // discard hole: material clear survived, far fragment filled it at clip z 0.8
        TASK_CHECK(z > 0.89f && z < 0.91f);  // window depth = 0.8 * 0.5 + 0.5
      } else {
        ++kept;
        // Outside both triangles the loaded depth stays 1.0; inside, near wrote clip z 0.2,
        // window depth 0.6, and the far fragment was early-Z rejected.
        TASK_CHECK(z == 1.0f || (z > 0.59f && z < 0.61f));
      }
    }
  // Every discarded near fragment corresponds to exactly one far fragment that passed.
  TASK_CHECK(static_cast<uint64_t>(holes) == discarded);
  TASK_CHECK(kept > 0);
  return 0;
}

int pipe_occlusion_early_z() {
  Renderer r(64, 64, Config{Mode::Tiled, 2, 16});
  r.clear(Vec4(0, 0, 0, 1));
  auto plan = compile_render_pass(material_plan(3));
  std::atomic<int> calls{0};
  r.begin_pass(*plan);
  auto far = big_tri(0.8f, 0.2f);
  auto near = big_tri(0.2f, 0.9f);
  // Subpass 0: far surface first, then a near one covering the same pixels and winning depth.
  r.subpass_draw<1>(far.data(), 3, kTriIdx, 3, VS1{}, MaterialFS{&calls}, no_cull());
  r.subpass_draw<1>(near.data(), 3, kTriIdx, 3, VS1{}, MaterialFS{&calls}, no_cull());
  r.next_subpass();
  // The far surface is resubmitted with the depth test on: where the near triangle wrote
  // depth it is rejected before the shader (the early-Z count the test pins). The near
  // surface is then resubmitted with the test off, as a deferred resolve, so it always runs
  // at its own equal depth and overwrites the derived attachment from the near material.
  r.subpass_draw<1>(far.data(), 3, kTriIdx, 3, VS1{}, DeriveFS{&calls}, no_cull());
  r.subpass_draw<1>(near.data(), 3, kTriIdx, 3, VS1{}, DeriveFS{&calls}, resolve_state());
  r.next_subpass();
  r.subpass_draw<1>(near.data(), 3, kTriIdx, 3, VS1{}, ShadeFS{&calls}, resolve_state());
  r.end_pass();
  TASK_CHECK(r.stats().raster.early_z_rejected > 50);
  // Near material attr 0.9 -> derived channel z = 0.9 * 4 = 3.6 -> clamped to 255 everywhere
  // the triangle covers; the far path (0.2 -> 0.8 -> 204) must never win a covered pixel.
  int near_only = 0, far_leak = 0;
  for (int y = 0; y < 64; ++y)
    for (int x = 0; x < 64; ++x) {
      const uint8_t* p = r.fb().color.at(x, y);
      if (p[2] == 255) ++near_only;
      if (p[2] == to_u8(0.8f)) ++far_leak;
    }
  TASK_CHECK(near_only > 100);
  TASK_CHECK(far_leak == 0);
  return 0;
}

int pipe_consecutive_draws_different_shaders() {
  Renderer r(48, 40, Config{Mode::Tiled, 2, 16});
  r.clear(Vec4(0, 0, 0, 1));
  RenderPassPlan d;
  d.physical_slots = 1;
  d.clear_values = {{0, Vec4(0, 0, 0, 1)}};
  d.subpasses = {{{}, {0}}};
  d.final_output = 0;
  auto plan = compile_render_pass(d);
  r.begin_pass(*plan);
  // One subpass, two draws with different N and FS: N=1 big triangle then N=2 corner triangle.
  std::vector<V1> big = big_tri(0.3f, 0.6f);
  std::vector<V2> corner = {
      {{-1.0f, -1.0f, 0.5f, 1.0f}, 0.1f, 0.2f},
      {{-0.4f, -1.0f, 0.5f, 1.0f}, 0.1f, 0.2f},
      {{-1.0f, -0.4f, 0.5f, 1.0f}, 0.1f, 0.2f},
  };
  r.subpass_draw<1>(big.data(), 3, kTriIdx, 3, VS1{},
                    [](const Fragment<1>& f, PassFragmentIO& io) {
                      io.out(0) = Vec4(f.attr[0], 0.0f, 0.0f, 1);
                      return true;
                    },
                    no_cull());
  r.subpass_draw<2>(corner.data(), 3, kTriIdx, 3, VS2{},
                    [](const Fragment<2>& f, PassFragmentIO& io) {
                      io.out(0) = Vec4(0.0f, f.attr[0], f.attr[1], 1);
                      return true;
                    },
                    no_cull());
  r.end_pass();
  int red = 0, green_blue = 0;
  for (int y = 0; y < 40; ++y)
    for (int x = 0; x < 48; ++x) {
      const uint8_t* p = r.fb().color.at(x, y);
      if (p[0] == to_u8(0.6f) && p[1] == 0) ++red;
      if (p[1] == to_u8(0.1f) && p[2] == to_u8(0.2f)) ++green_blue;
    }
  TASK_CHECK(red > 50);
  TASK_CHECK(green_blue > 0);
  return 0;
}

int pipe_thread_tile_consistency() {
  const PipeResult ref = run_material_pipe(Mode::Immediate, 1, 32);
  struct Cfg {
    int threads, tile;
  };
  for (Cfg c : {Cfg{1, 16}, Cfg{1, 32}, Cfg{1, 64}, Cfg{3, 16}, Cfg{3, 32}, Cfg{3, 64}}) {
    const PipeResult got = run_material_pipe(Mode::Tiled, c.threads, c.tile);
    TASK_CHECK(got.color == ref.color);
    TASK_CHECK(got.depth == ref.depth);
    TASK_CHECK(got.shaded == ref.shaded);
    TASK_CHECK(got.discarded == ref.discarded);
    TASK_CHECK(got.early_z == ref.early_z);
    TASK_CHECK(got.mat_calls == ref.mat_calls);
  }
  return 0;
}

int pipe_edge_tiles_and_empty_subpass_clear() {
  // 50x33 with tile 16: partial tiles on every border; subpass 1 has no geometry but first
  // outputs id 1, so its clear must still land, and sp2 reads it back.
  Renderer r(50, 33, Config{Mode::Tiled, 3, 16});
  r.clear(Vec4(0, 0, 0, 1));
  RenderPassPlan d;
  d.physical_slots = 2;
  d.clear_values = {{0, Vec4(0.7f, 0.7f, 0.7f, 1)}, {1, Vec4(0.4f, 0.5f, 0.6f, 1)}};
  d.subpasses = {{{}, {0}}, {{0}, {1}}, {{1}, {0}}};
  d.final_output = 0;
  auto plan = compile_render_pass(d);
  r.begin_pass(*plan);
  auto big = big_tri(0.3f, 1.0f);
  r.subpass_draw<1>(big.data(), 3, kTriIdx, 3, VS1{},
                    [](const Fragment<1>&, PassFragmentIO& io) {
                      io.out(0) = Vec4(0.9f, 0.1f, 0.1f, 1);
                      return true;
                    },
                    no_cull());
  r.next_subpass();  // no draws: id 1 must still be cleared tile-wide
  r.next_subpass();
  r.subpass_draw<1>(big.data(), 3, kTriIdx, 3, VS1{},
                    [](const Fragment<1>&, PassFragmentIO& io) {
                      const Vec4 d = io.in(0);
                      io.out(0) = d;  // copy the cleared derived attachment through
                      return true;
                    },
                    resolve_state());
  r.end_pass();
  int cleared_readback = 0;
  for (int y = 0; y < 33; ++y)
    for (int x = 0; x < 50; ++x) {
      const uint8_t* p = r.fb().color.at(x, y);
      if (p[0] == to_u8(0.4f) && p[1] == to_u8(0.5f) && p[2] == to_u8(0.6f)) ++cleared_readback;
    }
  TASK_CHECK(cleared_readback > 50);  // includes partial edge tiles
  return 0;
}

int pipe_consecutive_passes_no_slot_leak() {
  Renderer r(48, 40, Config{Mode::Tiled, 3, 16});
  r.clear(Vec4(0, 0, 0, 1));
  auto plan = compile_render_pass(material_plan(3));
  auto run_once = [&](std::atomic<int>* mc, std::atomic<int>* dc, std::atomic<int>* sc) {
    r.begin_pass(*plan);
    auto big = big_tri(0.3f, 0.4f);
    auto corner = corner_tri(0.6f, 0.8f);
    // Resolve-state throughout: the second pass must not be affected by the depth the first
    // pass wrote back, so this isolates slot-reuse cleanliness from depth ordering.
    r.subpass_draw<1>(big.data(), 3, kTriIdx, 3, VS1{}, MaterialFS{mc}, resolve_state());
    r.subpass_draw<1>(corner.data(), 3, kTriIdx, 3, VS1{}, MaterialFS{mc}, resolve_state());
    r.next_subpass();
    r.subpass_draw<1>(big.data(), 3, kTriIdx, 3, VS1{}, DeriveFS{dc}, resolve_state());
    r.subpass_draw<1>(corner.data(), 3, kTriIdx, 3, VS1{}, DeriveFS{dc}, resolve_state());
    r.next_subpass();
    r.subpass_draw<1>(big.data(), 3, kTriIdx, 3, VS1{}, ShadeFS{sc}, resolve_state());
    r.subpass_draw<1>(corner.data(), 3, kTriIdx, 3, VS1{}, ShadeFS{sc}, resolve_state());
    r.end_pass();
  };
  std::atomic<int> mc1{0}, dc1{0}, sc1{0};
  run_once(&mc1, &dc1, &sc1);
  std::vector<uint8_t> color1 = r.fb().color.rgba;
  std::vector<float> depth1 = r.fb().depth;
  std::atomic<int> mc2{0}, dc2{0}, sc2{0};
  run_once(&mc2, &dc2, &sc2);
  TASK_CHECK(r.fb().color.rgba == color1);
  TASK_CHECK(r.fb().depth == depth1);
  TASK_CHECK(mc1.load() == mc2.load() && dc1.load() == dc2.load() && sc1.load() == sc2.load());
  return 0;
}

}  // namespace

// task_check and main are provided by task_support/driver.c (compiled as C++ by the harness).
const task_case task_cases[] = {
    // planning
    {"planning", "valid plan compiles", plan_valid_compiles},
    {"planning", "undeclared attachment references", plan_undeclared_reference},
    {"planning", "feedback loop rejected", plan_feedback_loop},
    {"planning", "read before any output rejected", plan_read_undefined},
    {"planning", "duplicate attachment ids rejected", plan_duplicate_ids},
    {"planning", "undefined final output rejected", plan_final_undefined},
    {"planning", "physical slot budget enforced", plan_slot_budget},
    {"planning", "empty subpass list rejected", plan_empty_subpass_list},
    {"planning", "unused attachment takes no slot", plan_unused_attachment_uses_no_slot},
    {"planning", "bad plan then normal pass", plan_bad_then_good_leaves_renderer_clean},
    {"planning", "command ordering errors", plan_ordering_errors},
    {"planning", "blend and outputless subpass rejected", plan_blend_and_outputless_rejected},
    // aliasing
    {"aliasing", "six attachments fit three slots", alias_six_attachments_three_slots},
    {"aliasing", "equal endpoint intervals do not alias", alias_equal_endpoints_never_share},
    {"aliasing", "disjoint intervals share a slot", alias_disjoint_intervals_share},
    {"aliasing", "final output extends lifetime", alias_final_extends_lifetime},
    {"aliasing", "transient storage peak bound", alias_storage_peak_bound},
    {"aliasing", "re-output preserves earlier content", alias_reoutput_preserves_content},
    // pipeline
    {"pipeline", "negative and hdr intermediates", pipe_hdr_and_negative_intermediates},
    {"pipeline", "shader callback counts", pipe_callback_counts},
    {"pipeline", "discard preserves outputs and depth", pipe_discard_preserves_outputs_and_depth},
    {"pipeline", "occlusion and early-z", pipe_occlusion_early_z},
    {"pipeline", "consecutive draws with different shaders", pipe_consecutive_draws_different_shaders},
    {"pipeline", "thread and tile size consistency", pipe_thread_tile_consistency},
    {"pipeline", "edge tiles and empty subpass clear", pipe_edge_tiles_and_empty_subpass_clear},
    {"pipeline", "consecutive passes keep slots clean", pipe_consecutive_passes_no_slot_leak},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
