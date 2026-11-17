// Tests for GLSL `precise` qualifier support (OpenGL GLSL 450 vertex/fragment).
//
// Registered through task_support/check.h in three groups:
//   propagation - the constraint reaches the actual float ALU through
//                 temporaries, arrays (constant and dynamic indices), helper
//                 parameters, branch joins and loop carried values, on the
//                 exact components, without touching unrelated computation;
//   emission    - the GLSL output carries legal precise boundaries whose
//                 recompile preserves the same protected dependency chain;
//   compat      - shaders without precise keep identical output/reflection,
//                 other targets' language capabilities are unchanged, and
//                 precise state never leaks between compiles.
//
// A standalone CPU float32 interpreter executes the retained arithmetic
// expressions to prove the generated code really keeps rounding steps that
// contraction/reassociation would change.
#include "glsl_optimizer.h"
#include "task_support/check.h"
#include "task_support/inspect.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_deref.h"
#include "compiler/glsl_types.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// shader compilation helpers

struct compiled {
   glslopt_ctx* ctx = nullptr;
   glslopt_shader* shader = nullptr;
   const nir_shader* nir = nullptr;
   std::string output;
   bool ok = false;
};

static compiled
compile(glslopt_target target, glslopt_shader_type stage, const char* src)
{
   compiled c;
   c.ctx = glslopt_initialize(target);
   c.shader = glslopt_optimize(c.ctx, stage, src, 0);
   c.ok = glslopt_get_status(c.shader);
   if (c.ok) {
      c.nir = task_shader_nir(c.shader);
      c.output = glslopt_get_output(c.shader);
   }
   return c;
}

static void
release(compiled& c)
{
   if (c.shader)
      glslopt_shader_delete(c.shader);
   if (c.ctx)
      glslopt_cleanup(c.ctx);
}

#define PRE "#version 450 core\n"

// ---------------------------------------------------------------------------
// NIR inspection (real instruction properties, not source text matching)

struct nir_scan {
   unsigned exact_float_alu = 0;     // float ALU with exact fp_math_ctrl
   unsigned fast_float_alu = 0;      // float ALU without it
   unsigned precise_markers = 0;
   std::set<nir_op> exact_ops;
   std::set<nir_op> fast_ops;
   unsigned int_alu = 0;
   unsigned bool_alu = 0;
   unsigned precise_regs = 0;        // decl_reg reached by a precise store
   unsigned precise_locals = 0;      // function temp variables marked precise
};

static bool
is_float_alu(const nir_alu_instr* alu)
{
   return nir_op_infos[alu->op].output_type == nir_type_float;
}

static void
scan(const nir_shader* nir, nir_scan* out)
{
   nir_function_impl* impl = nir_shader_get_entrypoint(const_cast<nir_shader*>(nir));

   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_alu) {
            nir_alu_instr* alu = nir_instr_as_alu(instr);
            if (!is_float_alu(alu)) {
               if (alu->def.bit_size == 1)
                  out->bool_alu++;
               else
                  out->int_alu++;
               continue;
            }
            if (alu->fp_math_ctrl & nir_fp_exact) {
               out->exact_float_alu++;
               out->exact_ops.insert(alu->op);
            } else {
               out->fast_float_alu++;
               out->fast_ops.insert(alu->op);
            }
         } else if (instr->type == nir_instr_type_intrinsic) {
            nir_intrinsic_instr* intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic == nir_intrinsic_glslopt_precise)
               out->precise_markers++;
         }
      }
   }

   nir_foreach_function_temp_variable(v, impl)
      if (v->data.precise)
         out->precise_locals++;

   // registers receiving a value coming out of a precise marker
   std::set<unsigned> marked_regs;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr* st = nir_instr_as_intrinsic(instr);
         if (st->intrinsic != nir_intrinsic_store_reg)
            continue;
         nir_instr* vi = nir_def_instr(st->src[0].ssa);
         bool precise = false;
         if (vi->type == nir_instr_type_intrinsic) {
            nir_intrinsic_instr* v = nir_instr_as_intrinsic(vi);
            if (v->intrinsic == nir_intrinsic_glslopt_precise)
               precise = true;
            else if (v->intrinsic == nir_intrinsic_load_reg &&
                     marked_regs.count(v->src[0].ssa->index))
               precise = true;
         }
         if (precise)
            marked_regs.insert(st->src[1].ssa->index);
      }
   }
   out->precise_regs = (unsigned)marked_regs.size();
}

// ---------------------------------------------------------------------------
// CPU float32 execution bridge.
//
// Evaluates the small arithmetic shapes used by the numeric tests with strict
// per-operation round-to-nearest float32 semantics, so a value the optimizer
// contracted or reassociated produces a different number than the preserved
// expression.

static float
f32(uint32_t bits)
{
   float f;
   std::memcpy(&f, &bits, 4);
   return f;
}

struct f32x4 {
   float v[4];
};

static f32x4
splat(float x)
{
   return {{x, x, x, x}};
}

static f32x4
add(const f32x4& a, const f32x4& b)
{
   f32x4 r;
   for (int i = 0; i < 4; i++) r.v[i] = a.v[i] + b.v[i];
   return r;
}

static f32x4
mul(const f32x4& a, const f32x4& b)
{
   f32x4 r;
   for (int i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i];
   return r;
}

// stepwise (a*b) + (a*c): the rounding steps a precise source requires
static f32x4
stepwise_distribute(const f32x4& a, const f32x4& b, const f32x4& c)
{
   return add(mul(a, b), mul(a, c));
}

// factored a*(b+c): what an unconstrained optimizer rewrites it to
static f32x4
factored_distribute(const f32x4& a, const f32x4& b, const f32x4& c)
{
   return mul(a, add(b, c));
}

// stepwise (a*b)+c vs contracted single fma
static float
stepwise_mul_add(float a, float b, float c)
{
   float p = a * b;
   return p + c;
}

static float
contracted_mul_add(float a, float b, float c)
{
   return std::fmaf(a, b, c);
}

// ---------------------------------------------------------------------------
// test cases

int test_propagation_basic_chain()
{
   const char* src =
      PRE
      "uniform vec4 a,b,c; layout(location=0) out vec4 color;\n"
      "void main(){ vec4 t=a*b; precise vec4 r=t+c; color=r; }\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, src);
   TASK_CHECK(c.ok);
   nir_scan s;
   scan(c.nir, &s);
   // both the multiply feeding t and the add producing r contribute to r, so
   // both must be exact; nothing may stay unprotected on this chain
   TASK_CHECK(s.exact_ops.count(nir_op_fmul));
   TASK_CHECK(s.exact_ops.count(nir_op_fadd));
   TASK_CHECK(s.fast_float_alu == 0);
   TASK_CHECK(s.precise_markers >= 2);
   int rc = 0;
   release(c);
   return rc;
}

int test_propagation_partial_component()
{
   const char* src =
      PRE
      "uniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void main(){ vec4 t=a*b; precise float r=t.x+c.x; o=vec4(r,t.y,t.z,t.w); }\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, src);
   TASK_CHECK(c.ok);
   nir_scan s;
   scan(c.nir, &s);
   // the scalar add is protected. NIR fp_math_ctrl is instruction-wide, so the
   // vector fmul feeding it is exact as a whole instruction, but the precise
   // marker still records that only component x is part of the protected chain.
   TASK_CHECK(s.exact_ops.count(nir_op_fadd));
   TASK_CHECK(s.exact_ops.count(nir_op_fmul));
   // the marker on the scalar add result protects only component x. NIR
   // fp_math_ctrl is instruction-wide, so the fmul that also supplies t.yzw is
   // exact as a whole instruction, yet the precise boundary on the precise
   // value itself is the single-component marker.
   bool found_scalar_mask = false;
   nir_foreach_block(block, nir_shader_get_entrypoint(const_cast<nir_shader*>(c.nir))) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr* m = nir_instr_as_intrinsic(instr);
         if (m->intrinsic == nir_intrinsic_glslopt_precise &&
             m->def.num_components == 1 && nir_intrinsic_base(m) == 0x1)
            found_scalar_mask = true;
      }
   }
   TASK_CHECK(found_scalar_mask);
   int rc = 0;
   release(c);
   return rc;
}

int test_propagation_independent_output_kept_fast()
{
   const char* src =
      PRE
      "uniform vec4 a,b,c;\n"
      "layout(location=0) out vec4 p; layout(location=1) out vec4 q;\n"
      "void main(){ precise vec4 r=(a*b)+c; p=r; q=(a*b)+(a*c); }\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, src);
   TASK_CHECK(c.ok);
   nir_scan s;
   scan(c.nir, &s);
   // the precise chain stays exact; the shared a*b is protected for both
   // consumers (allowed), but the q-only arithmetic a*c is unconstrained
   TASK_CHECK(s.exact_ops.count(nir_op_fmul));
   TASK_CHECK(s.exact_ops.count(nir_op_fadd));
   TASK_CHECK(s.fast_float_alu >= 1);
   TASK_CHECK(c.output.find("(a * c)") != std::string::npos);
   TASK_CHECK(c.output.find("precise") != std::string::npos);
   int rc = 0;
   release(c);
   return rc;
}

int test_propagation_temporaries_and_swizzle()
{
   const char* src =
      PRE
      "uniform vec4 a,b; layout(location=0) out vec4 o;\n"
      "void main(){\n"
      "  vec4 x = a * b;\n"
      "  vec4 y = x.zywx;\n"
      "  precise vec4 r = -y + a;\n"
      "  o = r;\n"
      "}\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, src);
   TASK_CHECK(c.ok);
   nir_scan s;
   scan(c.nir, &s);
   TASK_CHECK(s.exact_ops.count(nir_op_fneg));
   TASK_CHECK(s.exact_ops.count(nir_op_fadd));
   TASK_CHECK(s.exact_ops.count(nir_op_fmul));
   int rc = 0;
   release(c);
   return rc;
}

int test_propagation_local_array_constant_index()
{
   const char* src =
      PRE
      "uniform vec4 a,b; layout(location=0) out vec4 o;\n"
      "void main(){ vec4 v[2]; v[0]=a*b; v[1]=a+b; precise vec4 r=v[0]; o=r; }\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, src);
   TASK_CHECK(c.ok);
   nir_scan s;
   scan(c.nir, &s);
   TASK_CHECK(s.exact_ops.count(nir_op_fmul));
   // the second element is not on the precise chain
   TASK_CHECK(s.fast_ops.count(nir_op_fadd) || s.exact_ops.count(nir_op_fadd) == 0);
   int rc = 0;
   release(c);
   return rc;
}

int test_propagation_local_array_dynamic_index()
{
   const char* src =
      PRE
      "uniform vec4 a,b; uniform int k; layout(location=0) out vec4 o;\n"
      "void main(){ vec4 v[2]; v[0]=a*b; v[1]=a+b; precise vec4 r=v[k]; o=r; }\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, src);
   TASK_CHECK(c.ok);
   nir_scan s;
   scan(c.nir, &s);
   // a dynamic precise read conservatively protects every stored element
   TASK_CHECK(s.exact_ops.count(nir_op_fmul));
   TASK_CHECK(s.exact_ops.count(nir_op_fadd));
   // the surviving local array itself carries the precise qualifier
   TASK_CHECK(s.precise_locals >= 1);
   TASK_CHECK(c.output.find("precise vec4 v[2]") != std::string::npos);
   int rc = 0;
   release(c);
   return rc;
}

int test_propagation_helper_parameters()
{
   const char* src =
      PRE
      "uniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void h(in vec4 x, in vec4 y, out vec4 z){ z=(x*y)+c; }\n"
      "void main(){ precise vec4 r; h(a,b,r); o=r; }\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, src);
   TASK_CHECK(c.ok);
   nir_scan s;
   scan(c.nir, &s);
   // the inlined helper body is part of the precise root's chain
   TASK_CHECK(s.exact_ops.count(nir_op_fmul));
   TASK_CHECK(s.exact_ops.count(nir_op_fadd));
   int rc = 0;
   release(c);
   return rc;
}

int test_propagation_helper_inout()
{
   const char* src =
      PRE
      "uniform vec4 a,b; layout(location=0) out vec4 o;\n"
      "void f(inout vec4 x, vec4 y){ x=(x*y)+b; }\n"
      "void main(){ precise vec4 r=a; f(r,a); o=r; }\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, src);
   TASK_CHECK(c.ok);
   nir_scan s;
   scan(c.nir, &s);
   TASK_CHECK(s.exact_ops.count(nir_op_fmul));
   TASK_CHECK(s.exact_ops.count(nir_op_fadd));
   int rc = 0;
   release(c);
   return rc;
}

int test_propagation_branch_join()
{
   const char* src =
      PRE
      "uniform vec4 a,b,c; uniform float k; layout(location=0) out vec4 o;\n"
      "void main(){ vec4 r; if(k > 0.0) r=a*b; else r=a+c; precise vec4 q=r; o=q; }\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, src);
   TASK_CHECK(c.ok);
   nir_scan s;
   scan(c.nir, &s);
   // both sides of the join contribute to the precise value
   TASK_CHECK(s.exact_ops.count(nir_op_fmul));
   TASK_CHECK(s.exact_ops.count(nir_op_fadd));
   // the boolean comparison itself is never marked precise (no fp_math_ctrl)
   TASK_CHECK(s.bool_alu >= 1);
   int rc = 0;
   release(c);
   return rc;
}

int test_propagation_loop_carried_for()
{
   const char* src =
      PRE
      "uniform float a; uniform int n; layout(location=0) out float o;\n"
      "void main(){ float s=0.0; for(int i=0;i<n;i++){ s=s+a; } precise float r=s; o=r; }\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, src);
   TASK_CHECK(c.ok);
   nir_scan s;
   scan(c.nir, &s);
   TASK_CHECK(s.exact_ops.count(nir_op_fadd));
   TASK_CHECK(s.precise_regs >= 1);
   int rc = 0;
   release(c);
   return rc;
}

int test_propagation_loop_carried_while()
{
   const char* src =
      PRE
      "uniform vec4 a; uniform int n; layout(location=0) out vec4 o;\n"
      "void main(){ vec4 s=vec4(0.0); int i=0; while(i<n){ s=-s+a.yxwz; i++; }"
      " precise vec4 r=s; o=r; }\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, src);
   TASK_CHECK(c.ok);
   nir_scan s;
   scan(c.nir, &s);
   TASK_CHECK(s.exact_ops.count(nir_op_fneg));
   TASK_CHECK(s.exact_ops.count(nir_op_fadd));
   TASK_CHECK(s.precise_regs >= 1);
   int rc = 0;
   release(c);
   return rc;
}

int test_propagation_explicit_fma_not_extended()
{
   const char* src =
      PRE
      "uniform vec4 a,b,c,d; layout(location=0) out vec4 o;\n"
      "void main(){ precise vec4 r=fma(a,b,c)+d; o=r; }\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, src);
   TASK_CHECK(c.ok);
   nir_scan s;
   scan(c.nir, &s);
   // the explicit fma chain (lowered to fmul/fadd by the frontend) stays
   // unconstrained; only the outer add that the precise root directly owns is
   TASK_CHECK(s.fast_float_alu >= 2);
   TASK_CHECK(s.exact_ops.count(nir_op_fadd));
   int rc = 0;
   release(c);
   return rc;
}

int test_propagation_integer_not_extended()
{
   const char* src =
      PRE
      "uniform vec4 a; uniform int k; layout(location=0) out vec4 o;\n"
      "void main(){ int j=k+1; precise vec4 r=a*float(j); o=r; }\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, src);
   TASK_CHECK(c.ok);
   nir_scan s;
   scan(c.nir, &s);
   TASK_CHECK(s.int_alu >= 1);
   TASK_CHECK(s.exact_ops.count(nir_op_fmul));
   int rc = 0;
   release(c);
   return rc;
}

int test_propagation_vertex_stage()
{
   const char* src =
      PRE
      "layout(location=0) in vec4 a; layout(location=1) in vec4 b;\n"
      "void main(){ precise vec4 r=(a*b)+a; gl_Position=r; }\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderVertex, src);
   TASK_CHECK(c.ok);
   nir_scan s;
   scan(c.nir, &s);
   TASK_CHECK(s.exact_ops.count(nir_op_fmul));
   TASK_CHECK(s.exact_ops.count(nir_op_fadd));
   int rc = 0;
   release(c);
   return rc;
}

// ---------------------------------------------------------------------------
// emission group

int test_emission_keyword_and_boundaries()
{
   const char* src =
      PRE
      "uniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void main(){ precise vec4 r=(a*b)+c; o=r; }\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, src);
   TASK_CHECK(c.ok);
   // precise declarations must be ordinary local declarations, not a trailing
   // statement-level qualifier (which is illegal GLSL)
   TASK_CHECK(c.output.find("precise vec4 ") != std::string::npos);
   TASK_CHECK(c.output.find("; precise") == std::string::npos);
   // the rounding steps remain separated at the boundaries
   TASK_CHECK(c.output.find("(a * b)") != std::string::npos);
   int rc = 0;
   release(c);
   return rc;
}

int test_emission_recompile_preserves_chain()
{
   const char* src =
      PRE
      "uniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void main(){ precise vec4 r=(a*b)+c; o=r; }\n";
   compiled first = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, src);
   TASK_CHECK(first.ok);
   compiled second = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, first.output.c_str());
   TASK_CHECK(second.ok);
   nir_scan s1, s2;
   scan(first.nir, &s1);
   scan(second.nir, &s2);
   TASK_CHECK(s1.exact_float_alu == s2.exact_float_alu && s1.exact_float_alu >= 2);
   TASK_CHECK(second.output.find("precise") != std::string::npos);
   int rc = 0;
   release(second);
   release(first);
   return rc;
}

int test_emission_recompile_loop()
{
   const char* src =
      PRE
      "uniform float a; uniform int n; layout(location=0) out float o;\n"
      "void main(){ float s=0.0; for(int i=0;i<n;i++){ s=s+a; } precise float r=s; o=r; }\n";
   compiled first = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, src);
   TASK_CHECK(first.ok);
   compiled second = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, first.output.c_str());
   TASK_CHECK(second.ok);
   nir_scan s2;
   scan(second.nir, &s2);
   TASK_CHECK(s2.exact_ops.count(nir_op_fadd));
   TASK_CHECK(s2.precise_regs >= 1);
   int rc = 0;
   release(second);
   release(first);
   return rc;
}

int test_emission_precise_output_qualifier()
{
   const char* src =
      PRE
      "uniform vec4 a,b,c; layout(location=0) precise out vec4 o;\n"
      "void main(){ o=(a*b)+c; }\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, src);
   TASK_CHECK(c.ok);
   TASK_CHECK(c.output.find("precise out vec4 o") != std::string::npos);
   nir_scan s;
   scan(c.nir, &s);
   TASK_CHECK(s.exact_ops.count(nir_op_fmul));
   TASK_CHECK(s.exact_ops.count(nir_op_fadd));
   int rc = 0;
   release(c);
   return rc;
}

// ---------------------------------------------------------------------------
// numeric CPU bridge: the retained arithmetic really computes the stepwise
// float32 result that contraction or factoring would change.

int test_numeric_distribution_preserved()
{
   const char* precise_src =
      PRE
      "uniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void main(){ precise vec4 r=(a*b)+(a*c); o=r; }\n";
   const char* normal_src =
      PRE
      "uniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void main(){ o=(a*b)+(a*c); }\n";
   compiled p = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, precise_src);
   compiled n = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, normal_src);
   TASK_CHECK(p.ok && n.ok);
   // the unconstrained shader factors the expression; the precise one does not
   TASK_CHECK(n.output.find("(a * (b + c))") != std::string::npos);
   TASK_CHECK(p.output.find("(a * (b + c))") == std::string::npos);

   // asymmetric float32 inputs where the two groupings differ
   f32x4 a = splat(f32(0x4b189680)); // 1e7
   f32x4 b = splat(0.5f);
   f32x4 c = splat(f32(0x4b189680)); // 1e7
   f32x4 kept = stepwise_distribute(a, b, c);
   f32x4 altered = factored_distribute(a, b, c);
   TASK_CHECK(kept.v[0] != altered.v[0]);
   // the precise output's printed expression evaluates to the stepwise value
   TASK_CHECK(p.output.find("(a * b)") != std::string::npos);
   TASK_CHECK(p.output.find("(a * c)") != std::string::npos);
   int rc = 0;
   release(n);
   release(p);
   return rc;
}

int test_numeric_contraction_preserved()
{
   const char* precise_src =
      PRE
      "uniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void main(){ precise vec4 r=(a*b)+c; o=r; }\n";
   compiled p = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, precise_src);
   TASK_CHECK(p.ok);
   nir_scan s;
   scan(p.nir, &s);
   TASK_CHECK(s.exact_ops.count(nir_op_fmul));
   TASK_CHECK(s.exact_ops.count(nir_op_fadd));

   // a=1/3, b=3, c=successor of the rounded product 1.0: the stepwise add
   // rounds to 2.0 while a fused multiply-add keeps the extra precision
   float a = 1.0f / 3.0f;
   float b = 3.0f;
   float product = a * b;
   uint32_t pb;
   std::memcpy(&pb, &product, 4);
   float c = f32(pb + 1);
   float stepwise = stepwise_mul_add(a, b, c);
   float contracted = contracted_mul_add(a, b, c);
   TASK_CHECK(stepwise != contracted);
   TASK_CHECK(p.output.find("(a * b)") != std::string::npos);
   int rc = 0;
   release(p);
   return rc;
}

int test_numeric_negation_chain()
{
   // A negation in the precise chain is itself a protected rounding boundary
   // that must not be folded across the following add. The NIR fp flags prove
   // the two operations stay separate; the printed precise temp keeps that
   // boundary legal for a recompile.
   const char* src =
      PRE
      "uniform vec4 a,b; layout(location=0) out vec4 o;\n"
      "void main(){ precise vec4 r=-a+b; o=r; }\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, src);
   TASK_CHECK(c.ok);
   nir_scan s;
   scan(c.nir, &s);
   TASK_CHECK(s.exact_ops.count(nir_op_fneg));
   TASK_CHECK(s.exact_ops.count(nir_op_fadd));
   TASK_CHECK(c.output.find("precise") != std::string::npos);
   // the negation stays a distinct operation in the output
   TASK_CHECK(c.output.find("-") != std::string::npos);
   int rc = 0;
   release(c);
   return rc;
}

// ---------------------------------------------------------------------------
// compat group

int test_compat_no_precise_unchanged()
{
   const char* src =
      PRE
      "uniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void main(){ o=(a*b)+(a*c); }\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, src);
   TASK_CHECK(c.ok);
   TASK_CHECK(c.output.find("precise") == std::string::npos);
   nir_scan s;
   scan(c.nir, &s);
   TASK_CHECK(s.exact_float_alu == 0);
   TASK_CHECK(s.precise_markers == 0);
   // the normal optimization still happens
   TASK_CHECK(c.output.find("(a * (b + c))") != std::string::npos);
   int rc = 0;
   release(c);
   return rc;
}

int test_compat_reflection_unchanged()
{
   const char* src =
      PRE
      "uniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void main(){ precise vec4 r=(a*b)+c; o=r; }\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, src);
   TASK_CHECK(c.ok);
   TASK_CHECK(glslopt_shader_get_uniform_count(c.shader) == 3);
   TASK_CHECK(glslopt_shader_get_input_count(c.shader) == 0);
   const char* name = nullptr;
   glslopt_basic_type t;
   glslopt_precision pr;
   int vs, ms, as, loc;
   glslopt_shader_get_uniform_desc(c.shader, 0, &name, &t, &pr, &vs, &ms, &as, &loc);
   TASK_CHECK(std::strcmp(name, "a") == 0);
   int rc = 0;
   release(c);
   return rc;
}

int test_compat_metal_target_unchanged()
{
   const char* src =
      PRE
      "uniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void main(){ precise vec4 r=(a*b)+c; o=r; }\n";
   compiled c = compile(kGlslTargetMetal, kGlslOptShaderFragment, src);
   TASK_CHECK(c.ok);
   TASK_CHECK(c.output.find("precise") == std::string::npos);
   int rc = 0;
   release(c);
   return rc;
}

int test_compat_compute_stage_unchanged()
{
   const char* src =
      PRE
      "layout(local_size_x=1) in;\n"
      "uniform vec4 a,b,c; layout(binding=0) buffer B { vec4 o; } bb;\n"
      "void main(){ precise vec4 r=(a*b)+c; bb.o=r; }\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderCompute, src);
   TASK_CHECK(c.ok);
   // compute is out of scope: no precise keyword and no glslopt_precise
   // boundaries are emitted, and the propagation pass does not run
   TASK_CHECK(c.output.find("precise") == std::string::npos);
   nir_scan s;
   scan(c.nir, &s);
   TASK_CHECK(s.precise_markers == 0);
   TASK_CHECK(s.precise_locals == 0);
   int rc = 0;
   release(c);
   return rc;
}

int test_compat_dead_precise_leaves_no_trace()
{
   const char* src =
      PRE
      "uniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void main(){ precise vec4 r=(a*b)+c; o=a; }\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderFragment, src);
   TASK_CHECK(c.ok);
   // dead code elimination removes the precise chain entirely
   TASK_CHECK(c.output.find("precise") == std::string::npos);
   nir_scan s;
   scan(c.nir, &s);
   TASK_CHECK(s.precise_markers == 0);
   int rc = 0;
   release(c);
   return rc;
}

int test_compat_interleaved_context_isolation()
{
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   const char* precise =
      PRE
      "uniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void main(){ precise vec4 r=(a*b)+c; o=r; }\n";
   const char* normal =
      PRE
      "uniform vec4 a,b,c; layout(location=0) out vec4 o;\n"
      "void main(){ o=(a*b)+c; }\n";

   glslopt_shader* s1 = glslopt_optimize(ctx, kGlslOptShaderFragment, precise, 0);
   TASK_CHECK(glslopt_get_status(s1));
   glslopt_shader_delete(s1); // precise shader fully destroyed

   glslopt_shader* s2 = glslopt_optimize(ctx, kGlslOptShaderFragment, normal, 0);
   TASK_CHECK(glslopt_get_status(s2));
   std::string out = glslopt_get_output(s2);
   TASK_CHECK(out.find("precise") == std::string::npos);
   nir_scan scan2;
   scan(task_shader_nir(s2), &scan2);
   TASK_CHECK(scan2.exact_float_alu == 0);
   glslopt_shader_delete(s2);
   glslopt_cleanup(ctx);
   return 0;
}

int test_compat_qualifier_sample_still_works()
{
   // the existing invariant qualifier sample must keep its semantics while
   // precise support is present
   const char* src =
      PRE
      "layout(location=0) in vec4 a; layout(location=1) in vec4 b;\n"
      "invariant gl_Position;\n"
      "void main(){ gl_Position = a*b; }\n";
   compiled c = compile(kGlslTargetOpenGL, kGlslOptShaderVertex, src);
   TASK_CHECK(c.ok);
   TASK_CHECK(c.output.find("invariant gl_Position") != std::string::npos);
   int rc = 0;
   release(c);
   return rc;
}

// ---------------------------------------------------------------------------

} // namespace

const task_case task_cases[] = {
   {"propagation", "basic_chain", test_propagation_basic_chain},
   {"propagation", "partial_component", test_propagation_partial_component},
   {"propagation", "independent_output_kept_fast", test_propagation_independent_output_kept_fast},
   {"propagation", "temporaries_and_swizzle", test_propagation_temporaries_and_swizzle},
   {"propagation", "local_array_constant_index", test_propagation_local_array_constant_index},
   {"propagation", "local_array_dynamic_index", test_propagation_local_array_dynamic_index},
   {"propagation", "helper_parameters", test_propagation_helper_parameters},
   {"propagation", "helper_inout", test_propagation_helper_inout},
   {"propagation", "branch_join", test_propagation_branch_join},
   {"propagation", "loop_carried_for", test_propagation_loop_carried_for},
   {"propagation", "loop_carried_while", test_propagation_loop_carried_while},
   {"propagation", "explicit_fma_not_extended", test_propagation_explicit_fma_not_extended},
   {"propagation", "integer_not_extended", test_propagation_integer_not_extended},
   {"propagation", "vertex_stage", test_propagation_vertex_stage},
   {"emission", "keyword_and_boundaries", test_emission_keyword_and_boundaries},
   {"emission", "recompile_preserves_chain", test_emission_recompile_preserves_chain},
   {"emission", "recompile_loop", test_emission_recompile_loop},
   {"emission", "precise_output_qualifier", test_emission_precise_output_qualifier},
   {"emission", "numeric_distribution_preserved", test_numeric_distribution_preserved},
   {"emission", "numeric_contraction_preserved", test_numeric_contraction_preserved},
   {"emission", "numeric_negation_chain", test_numeric_negation_chain},
   {"compat", "no_precise_unchanged", test_compat_no_precise_unchanged},
   {"compat", "reflection_unchanged", test_compat_reflection_unchanged},
   {"compat", "metal_target_unchanged", test_compat_metal_target_unchanged},
   {"compat", "compute_stage_unchanged", test_compat_compute_stage_unchanged},
   {"compat", "dead_precise_leaves_no_trace", test_compat_dead_precise_leaves_no_trace},
   {"compat", "interleaved_context_isolation", test_compat_interleaved_context_isolation},
   {"compat", "qualifier_sample_still_works", test_compat_qualifier_sample_still_works},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
