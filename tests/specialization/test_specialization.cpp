// Functional tests for glslopt_optimize_specialized.
//
// Numeric checks run the optimized NIR of both the original shader (with
// runtime uniform values, table entries overlaid) and the specialized shader
// through the small CPU interpreter and compare outputs. Structural checks
// cover texture/branch removal, reflection and stats; malformed tables check
// the path diagnostics and the all-or-nothing failure contract.
#include "glsl_optimizer.h"
#include "glslopt_test_hook.h"
#include "nir_cpu_interp.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, ...) \
   do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); ++g_failures; } } while (0)

namespace {

struct FV { // float vector value with storage
   std::vector<float> s;
   glslopt_uniform_value v;
   FV(std::vector<float> x) : s(std::move(x)) { v = {kGlslValueTypeFloat32, (int)s.size(), s.data(), nullptr}; }
};
struct IV {
   std::vector<int32_t> s;
   glslopt_uniform_value v;
   IV(std::vector<int32_t> x) : s(std::move(x)) { v = {kGlslValueTypeInt32, (int)s.size(), nullptr, s.data()}; }
};
struct BV {
   std::vector<int32_t> s;
   glslopt_uniform_value v;
   BV(std::vector<int32_t> x) : s(std::move(x)) { v = {kGlslValueTypeBool, (int)s.size(), nullptr, s.data()}; }
};

// A runtime value pushed into the interpreter.
struct feed {
   std::string root;
   std::vector<glslopt_path_segment> path;
   glslopt_value_type type;
   const void* data;
   int components;
};

bool float_close(float a, float b)
{
   if (memcmp(&a, &b, 4) == 0) return true;
   float d = a - b, m = (fabsf(a) > fabsf(b) ? fabsf(a) : fabsf(b));
   return fabsf(d) <= 1e-5f * (m + 1.0f);
}

// Run one machine over its NIR after feeding the listed runtime values.
bool execute(nir_cpu_machine& m, const std::vector<feed>& feeds, bool tolerate_removed,
             const std::string& tag)
{
   for (const auto& f : feeds) {
      if (tolerate_removed && !m.has_root(f.root))
         continue; // fully specialized away: no host slot remains
      std::string err;
      if (!m.set_leaf(f.root, f.path, f.type, f.data, f.components, err)) {
         printf("FAIL [%s]: feed %s: %s\n", tag.c_str(), f.root.c_str(), err.c_str());
         ++g_failures;
         return false;
      }
   }
   std::string err;
   if (!m.run(err)) {
      printf("FAIL [%s]: interpreter: %s\n", tag.c_str(), err.c_str());
      ++g_failures;
      return false;
   }
   return true;
}

bool same_outputs(nir_cpu_machine& ref, nir_cpu_machine& spec, const std::vector<std::string>& names,
                  const std::string& tag)
{
   bool ok = true;
   for (const auto& name : names) {
      nir_cpu_value a, b;
      std::string e1, e2;
      if (!ref.get_output(name, a, e1)) { printf("FAIL [%s]: ref output %s: %s\n", tag.c_str(), name.c_str(), e1.c_str()); ok = false; continue; }
      if (!spec.get_output(name, b, e2)) { printf("FAIL [%s]: spec output %s: %s\n", tag.c_str(), name.c_str(), e2.c_str()); ok = false; continue; }
      if (a.components != b.components || a.bit_size != b.bit_size) {
         printf("FAIL [%s]: output %s shape differs\n", tag.c_str(), name.c_str()); ok = false; continue;
      }
      for (unsigned i = 0; i < a.components; i++) {
         bool match = a.bit_size == 1 ? a.v[i].b == b.v[i].b : float_close(a.v[i].f32, b.v[i].f32);
         if (!match) {
            printf("FAIL [%s]: output %s component %u differs\n", tag.c_str(), name.c_str(), i);
            ok = false;
         }
      }
   }
   return ok;
}

const char* kArrayShader =
"#version 330 core\n"
"uniform bool enabled;\n"
"uniform float weights[3];\n"
"out vec4 color;\n"
"void main(){ color = enabled ? vec4(weights[1]) : vec4(weights[2]); }\n";

const char* kDynamicShader =
"#version 330 core\n"
"uniform float weights[3];\n"
"uniform int i;\n"
"uniform bool gate;\n"
"out vec4 color;\n"
"int index_with_work(int x){ return x; }\n" // helper forces a real call/inline
"void main(){\n"
"  float w = 0.0;\n"
"  if (gate) w = weights[index_with_work(i)];\n"
"  color = vec4(w);\n"
"}\n";

const char* kStructShader =
"#version 330 core\n"
"struct Material { vec3 tint; float scale; int flags; };\n"
"uniform Material mat[2];\n"
"uniform float weights[3];\n"
"uniform int pick;\n"
"out vec4 color;\n"
"float helper(int idx){ return weights[idx] * mat[idx].scale; }\n"
"void main(){\n"
"  float v = helper(pick);\n"
"  if (mat[1].flags > 0) color = vec4(mat[0].tint, v);\n"
"  else color = vec4(v, v, v, v);\n"
"}\n";

const char* kTextureShader =
"#version 330 core\n"
"uniform sampler2D tex;\n"
"uniform int mode;\n"
"in vec2 uv;\n"
"out vec4 color;\n"
"void main(){\n"
"  if (mode > 0) color = texture(tex, uv);\n"
"  else color = vec4(0.0);\n"
"}\n";

const char* kVertexShader =
"#version 330 core\n"
"uniform float weights[3];\n"
"uniform int i;\n"
"in vec4 pos;\n"
"out float v;\n"
"void main(){ gl_Position = pos; v = weights[i]; }\n";

} // namespace

// ---------------------------------------------------------------------------

static void test_partial_array(glslopt_ctx* ctx)
{
   FV w1({2.5f});
   BV en({1});
   glslopt_path_segment idx1 = {kGlslPathArrayIndex, nullptr, 1};
   std::vector<glslopt_specialized_uniform> table = {
      {"weights", 1, &idx1, &w1.v},
      {"enabled", 0, nullptr, &en.v},
   };
   auto* spec = glslopt_optimize_specialized(ctx, kGlslOptShaderFragment, kArrayShader, 0,
                                             table.data(), (int)table.size());
   CHECK(spec && glslopt_get_status(spec), "partial array: specialization failed: %s",
         spec ? glslopt_get_log(spec) : "null");
   if (!spec) return;
   CHECK(glslopt_shader_get_uniform_count(spec) == 0, "partial array: uniforms not fully removed (%d)",
         glslopt_shader_get_uniform_count(spec));
   const std::string out = glslopt_get_output(spec);
   CHECK(out.find("2.5") != std::string::npos, "partial array: constant did not reach output");
   CHECK(out.find("uniform") == std::string::npos, "partial array: uniform decl survived");

   // Numeric equivalence vs. the original across runtime combinations. The
   // reference runs the original shader with the table values overlaid (enabled
   // stays true, weights[1] stays 2.5); only non-specialized slots vary.
   auto* ref = glslopt_optimize(ctx, kGlslOptShaderFragment, kArrayShader, 0);
   for (float w0 : {0.25f, 7.5f})
      for (float w2 : {1.25f, 8.5f}) {
         float wv[3] = {w0, 2.5f, w2}; // element 1 is the table value
         std::vector<feed> rf;
         int32_t en = 1; // table value for enabled
         rf.push_back({"enabled", {}, kGlslValueTypeBool, &en, 1});
         for (int k = 0; k < 3; k++) {
            glslopt_path_segment s = {kGlslPathArrayIndex, nullptr, k};
            rf.push_back({"weights", {s}, kGlslValueTypeFloat32, &wv[k], 1});
         }
         nir_cpu_machine r2(glslopt_test_get_nir(ref)), s2(glslopt_test_get_nir(spec));
         if (execute(r2, rf, false, "array-ref") && execute(s2, rf, true, "array-spec"))
            same_outputs(r2, s2, {"color"}, "array");
      }
   glslopt_shader_delete(ref);
   glslopt_shader_delete(spec);
}

static void test_dynamic_and_short_circuit(glslopt_ctx* ctx)
{
   FV w1({2.5f});
   glslopt_path_segment idx1 = {kGlslPathArrayIndex, nullptr, 1};
   glslopt_specialized_uniform table = {"weights", 1, &idx1, &w1.v};
   auto* spec = glslopt_optimize_specialized(ctx, kGlslOptShaderFragment, kDynamicShader, 0, &table, 1);
   CHECK(spec && glslopt_get_status(spec), "dynamic: failed: %s", spec ? glslopt_get_log(spec) : "null");
   if (!spec) return;
   const std::string out = glslopt_get_output(spec);
   // Runtime read survives for the other elements and the guard still gates it.
   CHECK(out.find("weights[") != std::string::npos, "dynamic: runtime array read disappeared");
   CHECK(out.find("2.5") != std::string::npos, "dynamic: constant element missing");
   CHECK(glslopt_shader_get_uniform_count(spec) == 3, "dynamic: aggregate layout changed (%d)",
         glslopt_shader_get_uniform_count(spec));

   // The partially specialized array keeps its name, scalar type and [3]
   // shape; the host upload layout must not be rearranged.
   bool found_weights = false;
   for (int u = 0; u < glslopt_shader_get_uniform_count(spec); u++) {
      const char* nm;
      glslopt_basic_type bt;
      glslopt_precision prec;
      int vec, mat, arr, loc;
      glslopt_shader_get_uniform_desc(spec, u, &nm, &bt, &prec, &vec, &mat, &arr, &loc);
      if (strcmp(nm, "weights") == 0) {
         found_weights = true;
         CHECK(bt == kGlslTypeFloat && vec == 1 && arr == 3,
               "dynamic: weights reflection changed (type %d vec %d arr %d)", bt, vec, arr);
      }
   }
   CHECK(found_weights, "dynamic: weights uniform missing from reflection");

   auto* ref = glslopt_optimize(ctx, kGlslOptShaderFragment, kDynamicShader, 0);
   for (int gate = 0; gate < 2; gate++)
      for (int i = 0; i < 3; i++)
         for (float w0 : {0.5f, 3.25f}) {
            float wv[3] = {w0, 2.5f, 6.75f};
            std::vector<feed> rf;
            int32_t gate_v = gate, idx_v = i;
            rf.push_back({"gate", {}, kGlslValueTypeBool, &gate_v, 1});
            rf.push_back({"i", {}, kGlslValueTypeInt32, &idx_v, 1});
            for (int k = 0; k < 3; k++) {
               glslopt_path_segment s = {kGlslPathArrayIndex, nullptr, k};
               rf.push_back({"weights", {s}, kGlslValueTypeFloat32, &wv[k], 1});
            }
            nir_cpu_machine r2(glslopt_test_get_nir(ref)), s2(glslopt_test_get_nir(spec));
            if (execute(r2, rf, false, "dyn-ref") && execute(s2, rf, true, "dyn-spec"))
               same_outputs(r2, s2, {"color"}, "dynamic");
         }

   // An unspecialized element (weights[0]) still tracks its uploaded value.
   {
      float probe[2] = {1.0f, 9.0f};
      nir_cpu_value o[2];
      for (int q = 0; q < 2; q++) {
         nir_cpu_machine m(glslopt_test_get_nir(spec));
         float wv[3] = {probe[q], 2.5f, 6.75f};
         std::vector<feed> rf;
         int32_t gate_v = 1, idx_v = 0;
         rf.push_back({"gate", {}, kGlslValueTypeBool, &gate_v, 1});
         rf.push_back({"i", {}, kGlslValueTypeInt32, &idx_v, 1});
         for (int k = 0; k < 3; k++) {
            glslopt_path_segment s = {kGlslPathArrayIndex, nullptr, k};
            rf.push_back({"weights", {s}, kGlslValueTypeFloat32, &wv[k], 1});
         }
         std::string err, e2;
         bool gok = execute(m, rf, false, "dyn-live") && m.get_output("color", o[q], e2);
         CHECK(gok, "dyn-live: no color");
      }
      CHECK(!float_close(o[0].v[0].f32, o[1].v[0].f32),
            "dyn-live: unspecialized element did not follow its upload value (%f vs %f)",
            o[0].v[0].f32, o[1].v[0].f32);
   }

   glslopt_shader_delete(ref);
   glslopt_shader_delete(spec);
}

static void test_struct_helper(glslopt_ctx* ctx)
{
   FV w1({2.5f});
   FV scale0({4.0f});
   IV flags1({0}); // kills the texture-free tint branch, keeps the else path
   glslopt_path_segment w_idx = {kGlslPathArrayIndex, nullptr, 1};
   glslopt_path_segment f_path[2] = {{kGlslPathArrayIndex, nullptr, 1}, {kGlslPathStructField, "flags", 0}};
   glslopt_path_segment s_path[2] = {{kGlslPathArrayIndex, nullptr, 0}, {kGlslPathStructField, "scale", 0}};
   std::vector<glslopt_specialized_uniform> table = {
      {"weights", 1, &w_idx, &w1.v},
      {"mat", 2, f_path, &flags1.v},
      {"mat", 2, s_path, &scale0.v},
   };
   auto* spec = glslopt_optimize_specialized(ctx, kGlslOptShaderFragment, kStructShader, 0,
                                             table.data(), (int)table.size());
   CHECK(spec && glslopt_get_status(spec), "struct: failed: %s", spec ? glslopt_get_log(spec) : "null");
   if (!spec) return;

   auto* ref = glslopt_optimize(ctx, kGlslOptShaderFragment, kStructShader, 0);
   for (int pick = 0; pick < 2; pick++)
      for (float wscale : {1.5f, 8.0f}) {
         float wv[3] = {0.5f, 2.5f, 9.5f};
         float scales[2] = {4.0f, wscale}; // element 0 overlaid by table
         int flags[2] = {1, 0};            // element 1 overlaid to 0
         float tint[2][3] = {{1, 2, 3}, {4, 5, 6}};
         std::vector<feed> rf;
         int32_t pick_v = pick;
         rf.push_back({"pick", {}, kGlslValueTypeInt32, &pick_v, 1});
         for (int k = 0; k < 3; k++) {
            glslopt_path_segment s = {kGlslPathArrayIndex, nullptr, k};
            rf.push_back({"weights", {s}, kGlslValueTypeFloat32, &wv[k], 1});
         }
         for (int e = 0; e < 2; e++) {
            // vec3 leaf mat[e].tint exercises vector leaf initialization.
            glslopt_path_segment tp[2] = {{kGlslPathArrayIndex, nullptr, e},
                                          {kGlslPathStructField, "tint", 0}};
            rf.push_back({"mat", {tp[0], tp[1]}, kGlslValueTypeFloat32, tint[e], 3});
            glslopt_path_segment sp[2] = {{kGlslPathArrayIndex, nullptr, e},
                                          {kGlslPathStructField, "scale", 0}};
            rf.push_back({"mat", {sp[0], sp[1]}, kGlslValueTypeFloat32, &scales[e], 1});
            glslopt_path_segment fp[2] = {{kGlslPathArrayIndex, nullptr, e},
                                          {kGlslPathStructField, "flags", 0}};
            rf.push_back({"mat", {fp[0], fp[1]}, kGlslValueTypeInt32, &flags[e], 1});
         }
         nir_cpu_machine r2(glslopt_test_get_nir(ref)), s2(glslopt_test_get_nir(spec));
         if (execute(r2, rf, false, "struct-ref") && execute(s2, rf, true, "struct-spec"))
            same_outputs(r2, s2, {"color"}, "struct");
      }
   glslopt_shader_delete(ref);
   glslopt_shader_delete(spec);
}

static void test_texture_removal(glslopt_ctx* ctx)
{
   IV mode_off({0}), mode_on({1});

   auto* gone = glslopt_optimize_specialized(ctx, kGlslOptShaderFragment, kTextureShader, 0,
      std::vector<glslopt_specialized_uniform>{{"mode", 0, nullptr, &mode_off.v}}.data(), 1);
   CHECK(gone && glslopt_get_status(gone), "texture: off specialization failed");
   if (gone) {
      const std::string out = glslopt_get_output(gone);
      CHECK(out.find("texture") == std::string::npos, "texture: texture call survived");
      CHECK(out.find("sampler2D") == std::string::npos, "texture: sampler decl survived");
      CHECK(glslopt_shader_get_texture_count(gone) == 0, "texture: sampler still reflected (%d)",
            glslopt_shader_get_texture_count(gone));
      int math, tex, flow;
      glslopt_shader_get_stats(gone, &math, &tex, &flow);
      CHECK(tex == 0, "texture: tex instruction count not zero (%d)", tex);
   }

   auto* kept = glslopt_optimize_specialized(ctx, kGlslOptShaderFragment, kTextureShader, 0,
      std::vector<glslopt_specialized_uniform>{{"mode", 0, nullptr, &mode_on.v}}.data(), 1);
   CHECK(kept && glslopt_get_status(kept), "texture: on specialization failed");
   if (kept) {
      CHECK(glslopt_shader_get_texture_count(kept) == 1, "texture: live sampler removed");
      int math, tex, flow;
      glslopt_shader_get_stats(kept, &math, &tex, &flow);
      CHECK(tex == 1, "texture: tex instruction count wrong (%d)", tex);
   }
   glslopt_shader_delete(gone);
   glslopt_shader_delete(kept);
}

static void test_vertex_stage(glslopt_ctx* ctx)
{
   FV w1({2.5f});
   glslopt_path_segment idx1 = {kGlslPathArrayIndex, nullptr, 1};
   glslopt_specialized_uniform table = {"weights", 1, &idx1, &w1.v};
   auto* spec = glslopt_optimize_specialized(ctx, kGlslOptShaderVertex, kVertexShader, 0, &table, 1);
   CHECK(spec && glslopt_get_status(spec), "vertex: failed: %s", spec ? glslopt_get_log(spec) : "null");
   if (!spec) return;
   auto* ref = glslopt_optimize(ctx, kGlslOptShaderVertex, kVertexShader, 0);
   float pos[4] = {0.25f, -0.5f, 0.75f, 1.0f};
   for (int i = 0; i < 3; i++) {
      float wv[3] = {3.0f, 2.5f, 11.0f};
      std::vector<feed> rf;
      int32_t idx_v = i;
      rf.push_back({"pos", {}, kGlslValueTypeFloat32, pos, 4});
      rf.push_back({"i", {}, kGlslValueTypeInt32, &idx_v, 1});
      for (int k = 0; k < 3; k++) {
         glslopt_path_segment s = {kGlslPathArrayIndex, nullptr, k};
         rf.push_back({"weights", {s}, kGlslValueTypeFloat32, &wv[k], 1});
      }
      nir_cpu_machine r2(glslopt_test_get_nir(ref)), s2(glslopt_test_get_nir(spec));
      if (execute(r2, rf, false, "vs-ref") && execute(s2, rf, true, "vs-spec"))
         same_outputs(r2, s2, {"gl_Position", "v"}, "vertex");
   }
   glslopt_shader_delete(ref);
   glslopt_shader_delete(spec);
}

static void test_multi_element_array(glslopt_ctx* ctx)
{
   static const char* kShader =
      "#version 330 core\nuniform float weights[3];\nuniform int i;\nout vec4 color;\n"
      "void main(){ color = vec4(weights[i]); }\n";
   FV w1({2.5f}), w2({3.5f});
   glslopt_path_segment i1 = {kGlslPathArrayIndex, nullptr, 1};
   glslopt_path_segment i2 = {kGlslPathArrayIndex, nullptr, 2};
   std::vector<glslopt_specialized_uniform> table = {
      {"weights", 1, &i1, &w1.v},
      {"weights", 1, &i2, &w2.v},
   };
   auto* spec = glslopt_optimize_specialized(ctx, kGlslOptShaderFragment, kShader, 0,
                                             table.data(), (int)table.size());
   CHECK(spec && glslopt_get_status(spec), "multi-element: failed: %s",
         spec ? glslopt_get_log(spec) : "null");
   if (!spec) return;
   // Element 0 remains a runtime slot; the array name and [3] shape survive.
   const std::string out = glslopt_get_output(spec);
   CHECK(out.find("2.5") != std::string::npos && out.find("3.5") != std::string::npos,
         "multi-element: one of the two constants is missing");
   CHECK(glslopt_shader_get_uniform_count(spec) == 2, "multi-element: layout changed (%d)",
         glslopt_shader_get_uniform_count(spec));

   auto* ref = glslopt_optimize(ctx, kGlslOptShaderFragment, kShader, 0);
   for (int i = 0; i < 3; i++)
      for (float live : {0.75f, 6.25f}) {
         float wv[3] = {live, 2.5f, 3.5f}; // table overlays elements 1 and 2
         std::vector<feed> rf;
         int32_t idx = i;
         rf.push_back({"i", {}, kGlslValueTypeInt32, &idx, 1});
         for (int k = 0; k < 3; k++) {
            glslopt_path_segment s = {kGlslPathArrayIndex, nullptr, k};
            rf.push_back({"weights", {s}, kGlslValueTypeFloat32, &wv[k], 1});
         }
         nir_cpu_machine r2(glslopt_test_get_nir(ref)), s2(glslopt_test_get_nir(spec));
         if (execute(r2, rf, false, "multi-ref") && execute(s2, rf, true, "multi-spec"))
            same_outputs(r2, s2, {"color"}, "multi-element");
      }
   glslopt_shader_delete(ref);
   glslopt_shader_delete(spec);
}

static void test_invalid_tables(glslopt_ctx* ctx)
{
   auto expect_fail = [&](const char* tag, std::vector<glslopt_specialized_uniform> t) {
      auto* r = glslopt_optimize_specialized(ctx, kGlslOptShaderFragment, kArrayShader, 0,
                                             t.empty() ? nullptr : t.data(), (int)t.size());
      bool ok = r && !glslopt_get_status(r);
      std::string log = r ? glslopt_get_log(r) : "";
      CHECK(ok, "invalid %s: should have failed", tag);
      if (ok) {
         printf("  diagnostic [%s]: %s", tag, log.c_str());
         if (log.back() != '\n') printf("\n");
      }
      glslopt_shader_delete(r);
   };

   FV f({2.5f});
   IV iv({1});
   glslopt_path_segment idx1 = {kGlslPathArrayIndex, nullptr, 1};
   glslopt_path_segment idx9 = {kGlslPathArrayIndex, nullptr, 9};
   expect_fail("unknown path", {{"nope", 1, &idx1, &f.v}});
   expect_fail("out of range", {{"weights", 1, &idx9, &f.v}});
   expect_fail("type mismatch", {{"enabled", 0, nullptr, &iv.v}});
   FV bad({1.f / 0.f});
   expect_fail("non-finite", {{"weights", 1, &idx1, &bad.v}});
   expect_fail("duplicate", {{"weights", 1, &idx1, &f.v}, {"weights", 1, &idx1, &f.v}});
   glslopt_path_segment fld = {kGlslPathStructField, "nope", 0};
   expect_fail("bad field", {{"weights", 1, &fld, &f.v}});
   // A bool binding may only carry 0/1 even though the transport is int32.
   int32_t bool_two[1] = {2};
   glslopt_uniform_value bv = {kGlslValueTypeBool, 1, nullptr, bool_two};
   expect_fail("bool not 0/1", {{"enabled", 0, nullptr, &bv}});
}

// Scope limits and legal specialization of an unused uniform.
static void test_scope_and_unused(glslopt_ctx* ctx)
{
   // Unused (but legally declared) uniforms can still be specialized.
   static const char* kUnused =
      "#version 330 core\nuniform float unused[2];\nout vec4 color;\nvoid main(){ color = vec4(1.0); }\n";
   FV u0({3.5f});
   glslopt_path_segment uidx = {kGlslPathArrayIndex, nullptr, 0};
   glslopt_specialized_uniform ut = {"unused", 1, &uidx, &u0.v};
   auto* r = glslopt_optimize_specialized(ctx, kGlslOptShaderFragment, kUnused, 0, &ut, 1);
   CHECK(r && glslopt_get_status(r), "unused-uniform specialization should be valid: %s",
         r ? glslopt_get_log(r) : "null");
   if (r) glslopt_shader_delete(r);

   // Compute shaders are outside the first-version scope.
   static const char* kCompute =
      "#version 330 core\nlayout(local_size_x = 1) in;\nuniform float u;\n"
      "void main(){}\n";
   glslopt_specialized_uniform ct = {"u", 0, nullptr, &u0.v};
   auto* cr = glslopt_optimize_specialized(ctx, kGlslOptShaderCompute, kCompute, 0, &ct, 1);
   CHECK(cr && !glslopt_get_status(cr), "compute specialization must be rejected");
   if (cr) glslopt_shader_delete(cr);

   // The ES target is outside the first-version scope even for an ES 300 shader.
   auto* es = glslopt_initialize(kGlslTargetOpenGLES30);
   static const char* kES =
      "#version 300 es\nprecision mediump float;\nuniform float u;\nout vec4 color;\n"
      "void main(){ color = vec4(u); }\n";
   auto* er = glslopt_optimize_specialized(es, kGlslOptShaderFragment, kES, 0, &ct, 1);
   CHECK(er && !glslopt_get_status(er), "ES specialization must be rejected");
   if (er) glslopt_shader_delete(er);
   // Empty table on the ES target still follows the baseline pipeline.
   auto* ee = glslopt_optimize_specialized(es, kGlslOptShaderFragment, kES, 0, nullptr, 0);
   auto* eb = glslopt_optimize(es, kGlslOptShaderFragment, kES, 0);
   CHECK(ee && eb && glslopt_get_status(ee) && glslopt_get_status(eb) &&
            strcmp(glslopt_get_output(ee), glslopt_get_output(eb)) == 0,
         "ES empty-table output must match the baseline");
   if (ee) glslopt_shader_delete(ee);
   if (eb) glslopt_shader_delete(eb);
   glslopt_cleanup(es);
}

static void test_compatibility(glslopt_ctx* ctx)
{
   // Empty table is byte-identical to the classic entry point, on this target.
   auto* plain = glslopt_optimize(ctx, kGlslOptShaderFragment, kArrayShader, 0);
   auto* empty = glslopt_optimize_specialized(ctx, kGlslOptShaderFragment, kArrayShader, 0, nullptr, 0);
   CHECK(plain && empty && glslopt_get_status(plain) && glslopt_get_status(empty),
         "compat: baseline compiles");
   if (plain && empty)
      CHECK(strcmp(glslopt_get_output(plain), glslopt_get_output(empty)) == 0,
            "compat: empty table changed output");
   glslopt_shader_delete(plain);
   glslopt_shader_delete(empty);

   // Two different tables interleaved on one context stay independent, and the
   // caller's table memory is not modified.
   FV w1({2.5f});
   BV en({1});
   glslopt_path_segment idx1 = {kGlslPathArrayIndex, nullptr, 1};
   std::vector<glslopt_specialized_uniform> t1 = {{"weights", 1, &idx1, &w1.v}};
   std::vector<glslopt_specialized_uniform> t2 = {{"enabled", 0, nullptr, &en.v}};
   auto snapshot = [&] {
      return std::string(t1[0].uniform) + std::to_string(t1[0].segmentCount) +
             std::to_string(t2[0].segmentCount) + std::to_string(en.s[0]);
   };
   std::string before = snapshot();
   std::string o1, o2;
   for (int round = 0; round < 3; round++) {
      auto* a = glslopt_optimize_specialized(ctx, kGlslOptShaderFragment, kArrayShader, 0, t1.data(), 1);
      auto* b = glslopt_optimize_specialized(ctx, kGlslOptShaderFragment, kArrayShader, 0, t2.data(), 1);
      CHECK(a && b && glslopt_get_status(a) && glslopt_get_status(b), "compat: interleaved call failed");
      if (round == 0 && a && b) { o1 = glslopt_get_output(a); o2 = glslopt_get_output(b); }
      if (a && b) {
         CHECK(glslopt_get_output(a) == o1, "compat: table-1 result not stable");
         CHECK(glslopt_get_output(b) == o2, "compat: table-2 result not stable");
      }
      glslopt_shader_delete(a);
      glslopt_shader_delete(b);
   }
   CHECK(snapshot() == before, "compat: input binding table was modified");
}

int main()
{
   auto* ctx = glslopt_initialize(kGlslTargetOpenGL);
   test_partial_array(ctx);
   test_multi_element_array(ctx);
   test_dynamic_and_short_circuit(ctx);
   test_struct_helper(ctx);
   test_texture_removal(ctx);
   test_vertex_stage(ctx);
   test_invalid_tables(ctx);
   test_scope_and_unused(ctx);
   test_compatibility(ctx);
   glslopt_cleanup(ctx);
   if (g_failures) {
      printf("\nSPECIALIZATION TESTS: %d FAILURE(S)\n", g_failures);
      return 1;
   }
   printf("\nALL SPECIALIZATION TESTS PASSED\n");
   return 0;
}
