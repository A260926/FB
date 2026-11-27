// Partial-specialization test suite. Numeric cases run the optimized NIR of
// both the original shader (with runtime uniforms) and the specialized shader
// through the small CPU interpreter in nir_interp.h; structural cases inspect
// the generated GLSL, reflection and the NIR itself.
#include "glsl_optimizer.h"
#include "glslopt_specialize.h"
#include "nir_interp.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using nir_interp::Interpreter;
using nir_interp::Value;

namespace {

int g_failures = 0;
int g_checks = 0;

void failf(const char* fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   printf("  FAIL: ");
   vprintf(fmt, ap);
   printf("\n");
   va_end(ap);
   g_failures++;
}

void check(bool cond, const char* what)
{
   g_checks++;
   if (!cond)
      failf("%s", what);
}

bool contains(const std::string& haystack, const std::string& needle)
{
   return haystack.find(needle) != std::string::npos;
}

uint32_t fword(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
uint32_t iword(int32_t i) { uint32_t u; memcpy(&u, &i, 4); return u; }

glslopt_shader* plain(glslopt_ctx* ctx, glslopt_shader_type stage, const char* src)
{
   return glslopt_optimize(ctx, stage, src, 0);
}

glslopt_shader* spec(glslopt_ctx* ctx, glslopt_shader_type stage, const char* src,
                     const glslopt_specialized_value* v, unsigned n)
{
   return glslopt_optimize_specialized(ctx, stage, src, 0, v, n);
}

bool run_interp(nir_shader* nir, const std::vector<std::pair<std::string, std::vector<uint32_t>>>& inputs,
                Interpreter** made = nullptr)
{
   Interpreter* interp = new Interpreter(nir);
   for (auto& kv : inputs)
      interp->set_var_words(kv.first, kv.second);
   bool ok = interp->run();
   if (!ok)
      failf("interpreter error: %s", interp->error.c_str());
   if (made)
      *made = interp;
   else
      delete interp;
   return ok;
}

bool vec_close(const Value& a, const Value& b, unsigned channels)
{
   for (unsigned i = 0; i < channels; i++) {
      float x, y;
      memcpy(&x, &a.u[i], 4);
      memcpy(&y, &b.u[i], 4);
      float d = std::fabs(x - y), m = std::max(std::fabs(x), std::fabs(y));
      if (!(d <= 1e-5f * (1.0f + m)))
         return false;
   }
   return true;
}

// Run reference and specialized NIR over the same host uniform words, comparing
// the given vec4 output for every supplied input row.
void numeric_equivalence(glslopt_ctx* ctx, glslopt_shader_type stage, const char* src,
                         const glslopt_specialized_value* table, unsigned count,
                         const std::vector<std::vector<std::pair<std::string, std::vector<uint32_t>>>>& rows,
                         const char* out_name, unsigned out_channels)
{
   glslopt_shader* ref = plain(ctx, stage, src);
   glslopt_shader* sp = spec(ctx, stage, src, table, count);
   check(glslopt_get_status(ref), "reference compiles");
   check(glslopt_get_status(sp), "specialized compiles");
   if (!glslopt_get_status(ref) || !glslopt_get_status(sp)) {
      if (sp) printf("  log: %s\n", glslopt_get_log(sp));
      glslopt_shader_delete(ref);
      glslopt_shader_delete(sp);
      return;
   }
   nir_shader* ref_nir = glslopt_shader_get_nir_for_test(ref);
   nir_shader* sp_nir = glslopt_shader_get_nir_for_test(sp);
   for (size_t r = 0; r < rows.size(); r++) {
      Interpreter* a = nullptr;
      Interpreter* b = nullptr;
      bool ok1 = run_interp(ref_nir, rows[r], &a);
      bool ok2 = run_interp(sp_nir, rows[r], &b);
      if (ok1 && ok2) {
         const Value& va = a->output(out_name);
         const Value& vb = b->output(out_name);
         if (!vec_close(va, vb, out_channels))
            failf("row %zu mismatch: ref=%08x %08x %08x %08x spec=%08x %08x %08x %08x", r,
                  va.u[0], va.u[1], va.u[2], va.u[3], vb.u[0], vb.u[1], vb.u[2], vb.u[3]);
         else
            g_checks++;
      }
      delete a;
      delete b;
   }
   glslopt_shader_delete(ref);
   glslopt_shader_delete(sp);
}

// ---------------------------------------------------------------------------
// shader sources

const char* kPartialArray = R"(#version 330
uniform float weights[3];
uniform int pick;
out vec4 color;
void main() {
  int i = pick;
  float w = weights[i];
  color = vec4(w, weights[0], weights[1], weights[2]);
}
)";

const char* kStructArray = R"(#version 330
struct Material { float tint; vec3 base; };
uniform Material mats[2];
uniform int pick;
out vec4 color;
float shade(vec3 v) { return v.x * mats[0].tint + v.y * mats[1].tint; }
void main() {
  vec3 b = mats[pick].base;
  color = vec4(shade(b), mats[0].base.x, mats[1].base.y, mats[pick].tint);
}
)";

const char* kBranchTex = R"(#version 330
uniform bool enabled;
uniform sampler2D tex;
uniform float scale;
out vec4 color;
void main() {
  if (enabled)
    color = texture(tex, gl_FragCoord.xy) * scale;
  else
    color = vec4(scale, 0.0, 0.0, 1.0);
}
)";

const char* kShortCircuit = R"(#version 330
uniform bool gate;
uniform sampler2D tex;
uniform float data[4];
uniform int pick;
out vec4 color;
void main() {
  if (gate) {
    // The dynamic uniform read and its side-effecting index expression must
    // stay inside this branch; the texture keeps the if from being flattened.
    float v = data[pick];
    color = texture(tex, gl_FragCoord.xy) * v;
  } else {
    color = vec4(0.0);
  }
}
)";

const char* kAggregateCopy = R"(#version 330
uniform float weights[3];
uniform int pick;
out vec4 color;
void main() {
  float tmp[3];
  tmp = weights;                 // whole-aggregate copy into a local
  color = vec4(tmp[pick], tmp[0], tmp[1], tmp[2]);
}
)";

const char* kVertex = R"(#version 330
uniform float weights[3];
uniform int pick;
in vec4 pos;
void main() {
  gl_Position = vec4(pos.x * weights[pick], pos.y, pos.z, 1.0);
}
)";

const char* kBlock = R"(#version 330
uniform Block { float member; } blk;
out vec4 color;
void main() { color = vec4(blk.member); }
)";

} // namespace

static void test_partial_array(glslopt_ctx* ctx)
{
   printf("[partial array: const fold, dynamic read, host layout]\n");
   const float w1 = 2.5f;
   glslopt_specialized_value table[] = {{"weights[1]", kGlslValueFloat32, 1, &w1}};

   // Numeric equivalence over all indices; host slot 1 equals the table value.
   std::vector<std::vector<std::pair<std::string, std::vector<uint32_t>>>> rows;
   for (int pick = 0; pick < 3; pick++)
      for (float a : {1.0f, -3.25f, 7.0f})
         for (float c : {4.0f, 0.5f}) {
            std::vector<std::pair<std::string, std::vector<uint32_t>>> row;
            row.push_back({"weights", {fword(a), fword(2.5f), fword(c)}});
            row.push_back({"pick", {iword(pick)}});
            rows.push_back(row);
         }
   numeric_equivalence(ctx, kGlslOptShaderFragment, kPartialArray, table, 1, rows, "color", 4);

   // The table value wins even when the host uploads something else in slot 1.
   glslopt_shader* sp = spec(ctx, kGlslOptShaderFragment, kPartialArray, table, 1);
   check(glslopt_get_status(sp), "partial-array compiles");
   nir_shader* sp_nir = glslopt_shader_get_nir_for_test(sp);
   {
      std::vector<std::pair<std::string, std::vector<uint32_t>>> in;
      in.push_back({"weights", {fword(1.0f), fword(9.0f), fword(4.0f)}}); // host slot 1 = 9, ignored
      in.push_back({"pick", {iword(1)}});
      Interpreter* it = nullptr;
      if (run_interp(sp_nir, in, &it)) {
         const Value& o = it->output("color");
         float got; memcpy(&got, &o.u[2], 4); // color.z = weights[1]
         check(got == 2.5f, "specialized slot always reads the table value");
         float x; memcpy(&x, &o.u[1], 4);    // color.y = weights[0] host value
         check(x == 1.0f, "unspecialized element 0 still follows the upload");
         float w; memcpy(&w, &o.u[3], 4);    // color.w = weights[2]
         check(w == 4.0f, "unspecialized element 2 still follows the upload");
      }
      delete it;
   }
   // Reflection keeps the original name and array shape.
   check(glslopt_shader_get_uniform_count(sp) == 2, "partial array uniform is retained");
   bool found_weights = false;
   for (int i = 0; i < glslopt_shader_get_uniform_count(sp); i++) {
      const char* name; glslopt_basic_type t; glslopt_precision p;
      int vec, mat, arr, loc;
      glslopt_shader_get_uniform_desc(sp, i, &name, &t, &p, &vec, &mat, &arr, &loc);
      if (std::string(name) == "weights") {
         found_weights = true;
         check(arr == 3, "retained uniform keeps array size 3");
      }
   }
   check(found_weights, "retained uniform keeps its name");
   // The known constant is visible in the generated source, not text-spliced input.
   check(contains(glslopt_get_output(sp), "2.5"), "constant reaches the generated source");
   glslopt_shader_delete(sp);
}

static void test_aggregate_copy(glslopt_ctx* ctx)
{
   printf("[aggregate copy keeps the specialized element]\n");
   const float w1 = 2.5f;
   glslopt_specialized_value table[] = {{"weights[1]", kGlslValueFloat32, 1, &w1}};
   // The table value occupies host slot 1 in both the reference and the
   // specialized run; only the latter is allowed to ignore a different upload.
   std::vector<std::vector<std::pair<std::string, std::vector<uint32_t>>>> rows;
   for (int pick = 0; pick < 3; pick++) {
      std::vector<std::pair<std::string, std::vector<uint32_t>>> row;
      row.push_back({"weights", {fword(1.0f), fword(2.5f), fword(4.0f)}});
      row.push_back({"pick", {iword(pick)}});
      rows.push_back(row);
   }
   numeric_equivalence(ctx, kGlslOptShaderFragment, kAggregateCopy, table, 1, rows, "color", 4);

   // The copied local element is the constant; the uniform keeps its shape.
   glslopt_shader* sp = spec(ctx, kGlslOptShaderFragment, kAggregateCopy, table, 1);
   check(glslopt_get_status(sp), "aggregate-copy shader compiles");
   check(contains(glslopt_get_output(sp), "tmp[1] = 2.5"), "local copy receives the constant");
   check(glslopt_shader_get_uniform_count(sp) == 2, "uniform and its array shape retained");
   glslopt_shader_delete(sp);
}

static void test_struct_array_and_helper(glslopt_ctx* ctx)
{
   printf("[struct-array fields and helper functions]\n");
   const float t0 = 1.5f;
   const float base1[3] = {2.0f, 3.0f, 4.0f};
   glslopt_specialized_value table[] = {
      {"mats[0].tint", kGlslValueFloat32, 1, &t0},
      {"mats[1].base", kGlslValueFloat32, 3, base1},
   };
   // mats: [0]={tint=1.5(table), base=(..)}, [1]={tint=.., base=(2,3,4)(table)}
   std::vector<std::vector<std::pair<std::string, std::vector<uint32_t>>>> rows;
   for (int pick = 0; pick < 2; pick++) {
      std::vector<std::pair<std::string, std::vector<uint32_t>>> row;
      // Material layout: tint(1) + base(3) each
      std::vector<uint32_t> mats = {
         fword(1.5f), fword(0.1f), fword(0.2f), fword(0.3f),
         fword(5.0f), fword(2.0f), fword(3.0f), fword(4.0f),
      };
      row.push_back({"mats", mats});
      row.push_back({"pick", {iword(pick)}});
      rows.push_back(row);
   }
   numeric_equivalence(ctx, kGlslOptShaderFragment, kStructArray, table, 2, rows, "color", 4);

   glslopt_shader* sp = spec(ctx, kGlslOptShaderFragment, kStructArray, table, 2);
   check(glslopt_get_status(sp), "struct specialization compiles");
   check(glslopt_shader_get_uniform_count(sp) >= 1, "struct uniform retained for unspecialized reads");
   glslopt_shader_delete(sp);
}

static void test_branch_and_texture(glslopt_ctx* ctx)
{
   printf("[branch folding removes texture and statistics]\n");
   const int off = 0, on = 1;
   glslopt_specialized_value off_table[] = {{"enabled", kGlslValueBool, 1, &off}};
   glslopt_specialized_value on_table[] = {{"enabled", kGlslValueBool, 1, &on}};

   glslopt_shader* dead = spec(ctx, kGlslOptShaderFragment, kBranchTex, off_table, 1);
   glslopt_shader* live = spec(ctx, kGlslOptShaderFragment, kBranchTex, on_table, 1);
   check(glslopt_get_status(dead) && glslopt_get_status(live), "both branches compile");
   std::string dead_src = glslopt_get_output(dead);
   check(!contains(dead_src, "texture("), "texture call removed from generated source");
   check(glslopt_shader_get_texture_count(dead) == 0, "sampler removed from reflection");
   int m, t, f;
   glslopt_shader_get_stats(dead, &m, &t, &f);
   check(t == 0, "texture instruction count drops to zero");
   check(f == 0, "dead control flow is cleaned up");
   check(glslopt_shader_get_texture_count(live) == 1, "texture retained on the taken branch");
   glslopt_shader_get_stats(live, &m, &t, &f);
   check(t == 1, "texture instruction retained on the taken branch");

   // Interpreter confirms no texture op executes when the branch is gone.
   nir_shader* dead_nir = glslopt_shader_get_nir_for_test(dead);
   Interpreter* it = nullptr;
   std::vector<std::pair<std::string, std::vector<uint32_t>>> in = {
      {"scale", {fword(3.0f)}},
      {"gl_FragCoord", {fword(0.25f), fword(0.75f), fword(0.0f), fword(1.0f)}},
   };
   if (run_interp(dead_nir, in, &it))
      check(it->texture_calls == 0, "interpreter executes no texture op after folding");
   delete it;
   glslopt_shader_delete(dead);
   glslopt_shader_delete(live);
}

static void test_short_circuit(glslopt_ctx* ctx)
{
   printf("[side-effecting dynamic index stays inside the guarded branch]\n");
   const float d1 = 1.0f;
   glslopt_specialized_value table[] = {{"data[1]", kGlslValueFloat32, 1, &d1}};
   glslopt_shader* sp = spec(ctx, kGlslOptShaderFragment, kShortCircuit, table, 1);
   check(glslopt_get_status(sp), "short-circuit shader compiles");
   nir_shader* nir = glslopt_shader_get_nir_for_test(sp);

   // The texture keeps the if from being flattened. The injected index
   // compare/select must live in a block nested below the entry block (inside
   // the guarded branch), never hoisted before the condition is evaluated.
   nir_function_impl* impl = nir_shader_get_entrypoint(nir);
   nir_block* entry = nir_start_block(impl);
   bool nested_select = false, any_select = false;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_alu)
            continue;
         nir_alu_instr* alu = nir_instr_as_alu(instr);
         if (alu->op != nir_op_bcsel)
            continue;
         any_select = true;
         if (block != entry)
            nested_select = true;
      }
   }
   check(any_select, "dynamic read lowered to a select");
   check(nested_select, "select is not hoisted ahead of the guarded branch");

   // Numeric behavior matches the reference for every gate/pick combination.
   std::vector<std::vector<std::pair<std::string, std::vector<uint32_t>>>> rows;
   for (int gate = 0; gate <= 1; gate++)
      for (int pick = 0; pick < 4; pick++) {
         std::vector<std::pair<std::string, std::vector<uint32_t>>> row;
         row.push_back({"gate", {iword(gate)}});
         row.push_back({"data", {fword(-1.0f), fword(1.0f), fword(2.0f), fword(3.0f)}});
         row.push_back({"pick", {iword(pick)}});
         row.push_back({"gl_FragCoord", {fword(0.25f), fword(0.75f), fword(0.0f), fword(1.0f)}});
         rows.push_back(row);
      }
   numeric_equivalence(ctx, kGlslOptShaderFragment, kShortCircuit, table, 1, rows, "color", 4);
   glslopt_shader_delete(sp);
}

static void test_vertex_stage(glslopt_ctx* ctx)
{
   printf("[vertex stage specialization]\n");
   const float w0 = 1.25f;
   glslopt_specialized_value table[] = {{"weights[0]", kGlslValueFloat32, 1, &w0}};
   std::vector<std::vector<std::pair<std::string, std::vector<uint32_t>>>> rows;
   for (int pick = 0; pick < 3; pick++) {
      std::vector<std::pair<std::string, std::vector<uint32_t>>> row;
      row.push_back({"weights", {fword(1.25f), fword(2.0f), fword(3.0f)}});
      row.push_back({"pick", {iword(pick)}});
      row.push_back({"pos", {fword(2.0f), fword(4.0f), fword(6.0f), fword(1.0f)}});
      rows.push_back(row);
   }
   numeric_equivalence(ctx, kGlslOptShaderVertex, kVertex, table, 1, rows, "gl_Position", 4);
}

static void test_validation(glslopt_ctx* ctx)
{
   printf("[table validation and path diagnostics]\n");
   const char* src = kPartialArray;
   auto expect_fail = [&](const glslopt_specialized_value& v, const char* needle) {
      glslopt_shader* r = spec(ctx, kGlslOptShaderFragment, src, &v, 1);
      bool ok = !glslopt_get_status(r) && contains(glslopt_get_log(r), v.path ? v.path : "") &&
                contains(glslopt_get_log(r), needle);
      check(ok, std::string("reject: ").append(needle).c_str());
      if (!ok && r) printf("  log: %s\n", glslopt_get_log(r));
      glslopt_shader_delete(r);
   };

   const float f = 1.0f;
   const int i = 1;
   const float bad_f = std::nanf("");
   const int bad_bool = 2;
   expect_fail({"unknown[0]", kGlslValueFloat32, 1, &f}, "does not name a uniform");
   expect_fail({"weights[9]", kGlslValueFloat32, 1, &f}, "out-of-range");
   expect_fail({"weights[1]", kGlslValueInt32, 1, &i}, "expects a float32");
   expect_fail({"weights[1]", kGlslValueFloat32, 2, &f}, "component");
   expect_fail({"weights[1]", kGlslValueFloat32, 1, &bad_f}, "non-finite");
   expect_fail({"weights", kGlslValueFloat32, 1, &f}, "scalar or vector"); // whole array

   // bool range is checked against a real bool uniform
   {
      glslopt_specialized_value bad = {"enabled", kGlslValueBool, 1, &bad_bool};
      glslopt_shader* r = spec(ctx, kGlslOptShaderFragment, kBranchTex, &bad, 1);
      check(!glslopt_get_status(r) && contains(glslopt_get_log(r), "enabled") &&
               contains(glslopt_get_log(r), "0 or 1"),
            "bool value other than 0/1 rejected with its path");
      glslopt_shader_delete(r);
   }

   // sampler
   const int one = 1;
   glslopt_specialized_value sampler_v = {"enabled", kGlslValueBool, 1, &one};
   {
      glslopt_shader* r = spec(ctx, kGlslOptShaderFragment, kBranchTex, &sampler_v, 1);
      check(glslopt_get_status(r), "control bool accepted");
      glslopt_shader_delete(r);
   }
   const float ff = 1.0f;
   glslopt_specialized_value tex_v = {"tex", kGlslValueFloat32, 1, &ff};
   {
      glslopt_shader* r = spec(ctx, kGlslOptShaderFragment, kBranchTex, &tex_v, 1);
      check(!glslopt_get_status(r) && contains(glslopt_get_log(r), "tex") &&
               contains(glslopt_get_log(r), "sampler or image"),
            "sampler path rejected with its name");
      glslopt_shader_delete(r);
   }

   // interface block member
   glslopt_specialized_value block_v = {"blk.member", kGlslValueFloat32, 1, &ff};
   {
      glslopt_shader* r = spec(ctx, kGlslOptShaderFragment, kBlock, &block_v, 1);
      check(!glslopt_get_status(r) && contains(glslopt_get_log(r), "blk.member") &&
               contains(glslopt_get_log(r), "interface-block"),
            "block member rejected with its path");
      glslopt_shader_delete(r);
   }

   // duplicate target
   const float a = 1, b = 2;
   glslopt_specialized_value dup[] = {
      {"weights[1]", kGlslValueFloat32, 1, &a},
      {"weights[1]", kGlslValueFloat32, 1, &b},
   };
   {
      glslopt_shader* r = spec(ctx, kGlslOptShaderFragment, src, dup, 2);
      check(!glslopt_get_status(r) && contains(glslopt_get_log(r), "more than once"), "duplicate rejected");
      glslopt_shader_delete(r);
   }
}

static void test_empty_and_compat(glslopt_ctx* ctx)
{
   printf("[empty table, byte-identical output and call independence]\n");
   const char* src = kPartialArray;

   glslopt_shader* base = plain(ctx, kGlslOptShaderFragment, src);
   glslopt_shader* empty = spec(ctx, kGlslOptShaderFragment, src, nullptr, 0);
   glslopt_specialized_value dummy_table[1] = {{"weights[1]", kGlslValueFloat32, 1, (const void*)nullptr}};
   glslopt_shader* null_data = spec(ctx, kGlslOptShaderFragment, src, dummy_table, 0);
   check(glslopt_get_status(empty) && glslopt_get_status(null_data), "empty tables succeed");
   check(std::string(glslopt_get_output(base)) == glslopt_get_output(empty),
         "empty specialized table is byte-identical to glslopt_optimize");
   check(std::string(glslopt_get_output(base)) == glslopt_get_output(null_data),
         "zero-count table ignores its pointer");
   glslopt_shader_delete(base);
   glslopt_shader_delete(empty);
   glslopt_shader_delete(null_data);

   // Two different tables interleaved on the same context stay independent.
   const float w0 = 9.0f, w1 = 2.5f;
   glslopt_specialized_value t0[] = {{"weights[0]", kGlslValueFloat32, 1, &w0}};
   glslopt_specialized_value t1[] = {{"weights[1]", kGlslValueFloat32, 1, &w1}};
   glslopt_shader* r0 = spec(ctx, kGlslOptShaderFragment, src, t0, 1);
   glslopt_shader* r1 = spec(ctx, kGlslOptShaderFragment, src, t1, 1);
   glslopt_shader* r0b = spec(ctx, kGlslOptShaderFragment, src, t0, 1);
   check(glslopt_get_status(r0) && glslopt_get_status(r1) && glslopt_get_status(r0b), "interleaved calls compile");
   check(std::string(glslopt_get_output(r0)) == glslopt_get_output(r0b), "same table gives same result");
   check(std::string(glslopt_get_output(r0)) != glslopt_get_output(r1), "different tables give different results");
   check(contains(glslopt_get_output(r0), "9.0") && contains(glslopt_get_output(r1), "2.5"),
         "each result keeps its own constant");
   glslopt_shader_delete(r0);
   glslopt_shader_delete(r1);
   glslopt_shader_delete(r0b);

   // The input table is not modified.
   glslopt_specialized_value copy = t0[0];
   float sentinel = w0;
   glslopt_shader* r = spec(ctx, kGlslOptShaderFragment, src, &copy, 1);
   check(glslopt_get_status(r), "table-mutation probe compiles");
   check(copy.components == 1 && copy.type == kGlslValueFloat32 && copy.path == t0[0].path &&
            *(const float*)copy.data == sentinel,
         "input table is left untouched");
   glslopt_shader_delete(r);
}

static void test_target_guards(glslopt_ctx* gl)
{
   printf("[target and stage guards]\n");
   const int one = 1;
   glslopt_specialized_value v = {"enabled", kGlslValueBool, 1, &one};
   glslopt_ctx* es = glslopt_initialize(kGlslTargetOpenGLES30);
   glslopt_ctx* metal = glslopt_initialize(kGlslTargetMetal);
   glslopt_shader* a = glslopt_optimize_specialized(es, kGlslOptShaderFragment, kBranchTex, 0, &v, 1);
   glslopt_shader* b = glslopt_optimize_specialized(metal, kGlslOptShaderFragment, kBranchTex, 0, &v, 1);
   const char* compute = R"(#version 330
layout(local_size_x = 1) in;
uniform bool enabled;
void main() { if (enabled) {} }
)";
   glslopt_shader* c = glslopt_optimize_specialized(gl, kGlslOptShaderCompute, compute, 0, &v, 1);
   check(!glslopt_get_status(a), "ES target rejects specialization");
   check(!glslopt_get_status(b), "Metal target rejects specialization");
   check(!glslopt_get_status(c), "compute stage rejects specialization");
   glslopt_shader_delete(a);
   glslopt_shader_delete(b);
   glslopt_shader_delete(c);
   glslopt_cleanup(es);
   glslopt_cleanup(metal);
}

int run_specialization_tests(const char* /*testsDir*/)
{
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   if (!ctx) {
      printf("could not initialize OpenGL-target context\n");
      return 1;
   }
   test_partial_array(ctx);
   test_aggregate_copy(ctx);
   test_struct_array_and_helper(ctx);
   test_branch_and_texture(ctx);
   test_short_circuit(ctx);
   test_vertex_stage(ctx);
   test_validation(ctx);
   test_empty_and_compat(ctx);
   test_target_guards(ctx);
   glslopt_cleanup(ctx);

   printf("\nspecialization: %d checks, %d failure(s)\n", g_checks, g_failures);
   return g_failures ? 1 : 0;
}
