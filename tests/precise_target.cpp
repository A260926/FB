// Targeted checks for end-to-end 'precise' support: propagation through the
// value flow, emission of precise constraints in the GLSL output, and
// compatibility for shaders that use no precise qualifier.
//
// The NIR checks inspect the real NIR produced by the production compiler
// (task_shader_nir), looking at the fp_math_ctrl flags of the actual float
// instructions that a precise root reaches through temporaries, arrays,
// function copies, branch merges and loop back edges -- not at keywords in
// the printed text.  A small CPU bridge then interprets the final NIR of
// generated code on asymmetric inputs to confirm the preserved expression is
// the one that actually executes.

#include "task_support/check.h"
#include "task_support/inspect.h"
#include "glsl_optimizer.h"

#include "compiler/nir/nir.h"
#include "compiler/glsl_types.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// compile helpers

glslopt_shader* compile_ok(glslopt_ctx* ctx, glslopt_shader_type type, const char* src)
{
   glslopt_shader* s = glslopt_optimize(ctx, type, src, 0);
   if (!glslopt_get_status(s)) {
      fprintf(stderr, "compile failed: %s\n", glslopt_get_log(s));
   }
   return s;
}

// count exact float ALU instructions in the shader's NIR
int count_exact_alu(const nir_shader* n, unsigned* fp_mask_out = nullptr)
{
   int cnt = 0;
   unsigned mask = 0;
   nir_foreach_function_impl(impl, (nir_shader*)n) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            nir_alu_instr* a = nir_instr_as_alu(instr);
            if (nir_alu_instr_is_exact(a)) {
               cnt++;
               mask |= a->fp_math_ctrl;
            }
         }
      }
   }
   if (fp_mask_out)
      *fp_mask_out = mask;
   return cnt;
}

// collect exact ALU instructions of a given opcode
int count_exact_op(const nir_shader* n, nir_op op)
{
   int cnt = 0;
   nir_foreach_function_impl(impl, (nir_shader*)n) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_alu) {
               nir_alu_instr* a = nir_instr_as_alu(instr);
               if (a->op == op && nir_alu_instr_is_exact(a))
                  cnt++;
            }
         }
      }
   }
   return cnt;
}

// count all (exact or not) ALU instructions of a given opcode
int count_op(const nir_shader* n, nir_op op)
{
   int cnt = 0;
   nir_foreach_function_impl(impl, (nir_shader*)n) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_alu && nir_instr_as_alu(instr)->op == op)
               cnt++;
         }
      }
   }
   return cnt;
}

bool has_keyword(const char* text, const char* kw)
{
   return text && std::strstr(text, kw) != nullptr;
}

// ---------------------------------------------------------------------------
// CPU execution bridge: interpret the final NIR of a compiled shader.
//
// Handles the float32/int/bool scalar and vector operations, local arrays,
// registers (loop-carried values) and structured control flow that the
// production compiler emits for the arithmetic fragments under test.  This is
// an independent evaluator: it computes every operation in the order and with
// the float32 rounding the NIR prescribes.

struct interp {
   const nir_shader* nir;
   nir_function_impl* impl;
   bool failed = false;
   const char* why = nullptr;

   // storage
   std::map<const nir_variable*, std::vector<uint32_t>> vars;
   std::vector<std::vector<uint32_t>> ssa;             // by def index
   std::map<unsigned, std::vector<uint32_t>> regs;     // by reg def index

   enum Flow { NORMAL, BREAK, CONTINUE, RETURN };

   static unsigned type_slots(const glsl_type* t)
   {
      if (glsl_type_is_array(t))
         return glsl_get_length(t) * type_slots(glsl_get_array_element(t));
      if (glsl_type_is_matrix(t))
         return glsl_get_matrix_columns(t) * glsl_get_vector_elements(glsl_get_column_type(t));
      if (glsl_type_is_struct(t) || glsl_type_is_interface(t)) {
         unsigned n = 0;
         for (unsigned i = 0; i < glsl_get_length(t); i++)
            n += type_slots(glsl_get_struct_field(t, i));
         return n;
      }
      if (glsl_type_is_vector_or_scalar(t))
         return glsl_get_vector_elements(t);
      return 0;
   }

   static float as_float(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
   static uint32_t as_bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
   static int32_t as_int(uint32_t u) { return (int32_t)u; }

   void fail(const char* w)
   {
      if (!failed) { failed = true; why = w; }
   }

   // base slot of a deref, evaluating array indices (constant or computed)
   unsigned deref_base(nir_deref_instr* d)
   {
      if (d->deref_type == nir_deref_type_var)
         return 0;
      nir_deref_instr* parent = nir_deref_instr_parent(d);
      unsigned base = deref_base(parent);
      const glsl_type* ptype = parent->type;
      if (d->deref_type == nir_deref_type_array) {
         unsigned idx;
         if (nir_src_is_const(d->arr.index))
            idx = (unsigned)nir_src_as_uint(d->arr.index);
         else
            idx = ssa[d->arr.index.ssa->index][0];
         return base + idx * type_slots(glsl_get_array_element(ptype));
      }
      if (d->deref_type == nir_deref_type_struct) {
         unsigned off = 0;
         for (unsigned i = 0; i < d->strct.index; i++)
            off += type_slots(glsl_get_struct_field(ptype, i));
         return base + off;
      }
      fail("unsupported deref");
      return 0;
   }

   std::vector<uint32_t>& def_slots(nir_def* d)
   {
      return ssa[d->index];
   }

   uint32_t src_bits(nir_src& src, unsigned comp)
   {
      return ssa[src.ssa->index][comp];
   }
   uint32_t alu_bits(nir_alu_instr* alu, unsigned i, unsigned comp)
   {
      nir_def* d = alu->src[i].src.ssa;
      return ssa[d->index][alu->src[i].swizzle[comp]];
   }

   void exec_alu(nir_alu_instr* alu)
   {
      unsigned n = alu->def.num_components;
      std::vector<uint32_t>& out = def_slots(&alu->def);
      out.assign(n, 0);
      auto f = [&](unsigned i, unsigned c) { return as_float(alu_bits(alu, i, c)); };
      auto i32 = [&](unsigned i, unsigned c) { return as_int(alu_bits(alu, i, c)); };
      auto b = [&](unsigned i, unsigned c) { return alu_bits(alu, i, c) != 0; };
      for (unsigned c = 0; c < n; c++) {
         switch (alu->op) {
         case nir_op_fadd: out[c] = as_bits(f(0, c) + f(1, c)); break;
         case nir_op_fsub: out[c] = as_bits(f(0, c) - f(1, c)); break;
         case nir_op_fmul: out[c] = as_bits(f(0, c) * f(1, c)); break;
         case nir_op_fdiv: out[c] = as_bits(f(0, c) / f(1, c)); break;
         case nir_op_fneg: out[c] = as_bits(-f(0, c)); break;
         case nir_op_fabs: out[c] = as_bits(std::fabs(f(0, c))); break;
         case nir_op_fmin: out[c] = as_bits(std::fmin(f(0, c), f(1, c))); break;
         case nir_op_fmax: out[c] = as_bits(std::fmax(f(0, c), f(1, c))); break;
         case nir_op_mov:  out[c] = alu_bits(alu, 0, c); break;
         case nir_op_vec2: case nir_op_vec3: case nir_op_vec4:
            out[c] = alu_bits(alu, c, 0); break;
         case nir_op_bcsel: out[c] = b(0, c) ? alu_bits(alu, 1, c) : alu_bits(alu, 2, c); break;
         case nir_op_flt: out[c] = f(0, c) < f(1, c) ? 1u : 0u; break;
         case nir_op_fge: out[c] = f(0, c) >= f(1, c) ? 1u : 0u; break;
         case nir_op_feq: out[c] = f(0, c) == f(1, c) ? 1u : 0u; break;
         case nir_op_fneu: out[c] = f(0, c) != f(1, c) ? 1u : 0u; break;
         case nir_op_ilt: out[c] = i32(0, c) < i32(1, c) ? 1u : 0u; break;
         case nir_op_ige: out[c] = i32(0, c) >= i32(1, c) ? 1u : 0u; break;
         case nir_op_ieq: out[c] = i32(0, c) == i32(1, c) ? 1u : 0u; break;
         case nir_op_ine: out[c] = i32(0, c) != i32(1, c) ? 1u : 0u; break;
         case nir_op_iadd: out[c] = (uint32_t)(i32(0, c) + i32(1, c)); break;
         case nir_op_isub: out[c] = (uint32_t)(i32(0, c) - i32(1, c)); break;
         case nir_op_imul: out[c] = (uint32_t)(i32(0, c) * i32(1, c)); break;
         case nir_op_ineg: out[c] = (uint32_t)(-i32(0, c)); break;
         case nir_op_i2f32: out[c] = as_bits((float)i32(0, c)); break;
         case nir_op_f2i32: out[c] = (uint32_t)(int32_t)f(0, c); break;
         case nir_op_b2f32: out[c] = as_bits(b(0, c) ? 1.0f : 0.0f); break;
         default:
            fail("unsupported alu op");
            return;
         }
      }
   }

   void exec_instr(nir_instr* instr, Flow& flow)
   {
      switch (instr->type) {
      case nir_instr_type_load_const: {
         nir_load_const_instr* lc = nir_instr_as_load_const(instr);
         std::vector<uint32_t>& out = def_slots(&lc->def);
         out.assign(lc->def.num_components, 0);
         for (unsigned c = 0; c < lc->def.num_components; c++) {
            if (lc->def.bit_size == 1)
               out[c] = lc->value[c].b ? 1u : 0u;
            else if (lc->def.bit_size == 32)
               out[c] = lc->value[c].u32;
            else
               fail("const bit size");
         }
         return;
      }
      case nir_instr_type_alu:
         exec_alu(nir_instr_as_alu(instr));
         return;
      case nir_instr_type_deref:
      case nir_instr_type_undef:
         return;
      case nir_instr_type_jump: {
         nir_jump_instr* j = nir_instr_as_jump(instr);
         flow = j->type == nir_jump_break ? BREAK :
                j->type == nir_jump_continue ? CONTINUE : RETURN;
         return;
      }
      case nir_instr_type_intrinsic: {
         nir_intrinsic_instr* in = nir_instr_as_intrinsic(instr);
         switch (in->intrinsic) {
         case nir_intrinsic_decl_reg:
            return; // register declaration; storage allocated on first store
         case nir_intrinsic_load_deref: {
            nir_deref_instr* d = nir_src_as_deref(in->src[0]);
            nir_variable* var = nir_deref_instr_get_variable(d);
            unsigned base = deref_base(d);
            std::vector<uint32_t>& out = def_slots(&in->def);
            out.assign(in->def.num_components, 0);
            auto& storage = vars[var];
            for (unsigned c = 0; c < in->def.num_components; c++)
               out[c] = storage[base + c];
            return;
         }
         case nir_intrinsic_store_deref: {
            nir_deref_instr* d = nir_src_as_deref(in->src[0]);
            nir_variable* var = nir_deref_instr_get_variable(d);
            unsigned base = deref_base(d);
            unsigned wrmask = nir_intrinsic_write_mask(in);
            auto& storage = vars[var];
            unsigned rank = 0;
            unsigned ncomps = glsl_type_is_vector_or_scalar(d->type) ? glsl_get_vector_elements(d->type) : 1;
            for (unsigned c = 0; c < ncomps; c++) {
               if (!(wrmask & (1u << c)))
                  continue;
               storage[base + c] = src_bits(in->src[1], rank);
               rank++;
            }
            return;
         }
         case nir_intrinsic_load_reg: {
            nir_def* reg = in->src[0].ssa;
            std::vector<uint32_t>& out = def_slots(&in->def);
            auto& storage = regs[reg->index];
            out = storage;
            return;
         }
         case nir_intrinsic_store_reg: {
            nir_def* reg = in->src[1].ssa;
            auto& storage = regs[reg->index];
            if (storage.empty())
               storage.assign(in->src[0].ssa->num_components, 0);
            unsigned wrmask = nir_intrinsic_write_mask(in);
            unsigned rank = 0;
            for (unsigned c = 0; c < storage.size(); c++) {
               if (!(wrmask & (1u << c)))
                  continue;
               storage[c] = src_bits(in->src[0], rank);
               rank++;
            }
            return;
         }
         default:
            fail("unsupported intrinsic");
            return;
         }
      }
      default:
         fail("unsupported instr");
         return;
      }
   }

   Flow exec_block(nir_block* block)
   {
      Flow flow = NORMAL;
      nir_foreach_instr(instr, block) {
         exec_instr(instr, flow);
         if (failed)
            return RETURN;
         if (flow != NORMAL)
            return flow;
      }
      return NORMAL;
   }

   Flow exec_list(struct exec_list* list)
   {
      foreach_list_typed(nir_cf_node, node, node, list) {
         Flow flow = exec_node(node);
         if (flow != NORMAL)
            return flow;
      }
      return NORMAL;
   }

   Flow exec_node(nir_cf_node* node)
   {
      switch (node->type) {
      case nir_cf_node_block:
         return exec_block(nir_cf_node_as_block(node));
      case nir_cf_node_if: {
         nir_if* nif = nir_cf_node_as_if(node);
         bool cond = ssa[nif->condition.ssa->index][0] != 0;
         return exec_list(cond ? &nif->then_list : &nif->else_list);
      }
      case nir_cf_node_loop: {
         nir_loop* loop = nir_cf_node_as_loop(node);
         for (unsigned iter = 0; iter < 1000000; iter++) {
            Flow flow = exec_list(&loop->body);
            if (flow == BREAK || flow == RETURN)
               return flow == BREAK ? NORMAL : RETURN;
            // CONTINUE and NORMAL both proceed to the next iteration
         }
         fail("loop trip count");
         return RETURN;
      }
      default:
         fail("unsupported cf node");
         return RETURN;
      }
   }

   // allocate storage for every variable; uniform/input values come from inputs
   bool run(const std::map<std::string, std::vector<float>>& inputs)
   {
      impl = nir_shader_get_entrypoint((nir_shader*)nir);
      if (!impl) { fail("no entry"); return false; }
      nir_index_ssa_defs(impl);
      ssa.assign(impl->ssa_alloc, {});
      nir_foreach_variable_in_shader(var, (nir_shader*)nir) {
         unsigned slots = type_slots(var->type);
         if (!slots)
            continue;
         auto& storage = vars[var];
         storage.assign(slots, 0);
         auto it = inputs.find(var->name ? var->name : "");
         if (it != inputs.end()) {
            // store inputs with the bit pattern matching the variable's type
            glsl_base_type bt = glsl_get_base_type(glsl_without_array(var->type));
            bool is_int = bt == GLSL_TYPE_INT || bt == GLSL_TYPE_UINT;
            for (unsigned i = 0; i < slots && i < it->second.size(); i++)
               storage[i] = is_int ? (uint32_t)(int32_t)it->second[i] : as_bits(it->second[i]);
         }
      }
      exec_list(&impl->body);
      return !failed;
   }

   bool read_var(const char* name, std::vector<float>& out)
   {
      nir_foreach_variable_in_shader(var, (nir_shader*)nir) {
         if (!var->name || strcmp(var->name, name) != 0)
            continue;
         auto it = vars.find(var);
         if (it == vars.end())
            return false;
         unsigned slots = type_slots(var->type);
         out.resize(slots);
         for (unsigned i = 0; i < slots; i++)
            out[i] = as_float(it->second[i]);
         return true;
      }
      return false;
   }
};

// interpret a compiled shader with the given uniform inputs; returns the named
// output variable's components
bool run_shader(const nir_shader* n, const std::map<std::string, std::vector<float>>& inputs,
                const char* out_name, std::vector<float>& out)
{
   interp I;
   I.nir = n;
   if (!I.run(inputs)) {
      fprintf(stderr, "interp failed: %s\n", I.why ? I.why : "?");
      return false;
   }
   return I.read_var(out_name, out);
}

// stepwise float32 reference: force each intermediate through memory so the
// host compiler cannot contract or reorder the rounding steps
float step(float v) { volatile float x = v; return x; }

} // namespace

// ---------------------------------------------------------------------------
// propagation group: the precise root must reach the contributing float
// instructions through the value flow, visible as exact fp_math_ctrl flags in
// the production NIR.

static int propagation_temp_chain()
{
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   const char* src = "#version 450 core\n"
      "uniform vec4 a,b,c; layout(location=0) out vec4 color;\n"
      "void main(){ vec4 t=a*b; precise vec4 r=t+c; color=r; }\n";
   glslopt_shader* s = compile_ok(ctx, kGlslOptShaderFragment, src);
   TASK_CHECK(s != nullptr);
   const nir_shader* n = task_shader_nir(s);
   TASK_CHECK(n != nullptr);
   // the multiply that feeds the precise add through a temporary, and the add
   // itself, must both be exact in the production NIR
   TASK_CHECK(count_exact_op(n, nir_op_fmul) == 1);
   TASK_CHECK(count_exact_op(n, nir_op_fadd) == 1);
   unsigned mask = 0;
   TASK_CHECK(count_exact_alu(n, &mask) == 2);
   // the protection includes no-reassociation / no-contract / no-transform
   TASK_CHECK((mask & nir_fp_no_reassoc) != 0);
   TASK_CHECK((mask & nir_fp_no_contract) != 0);
   TASK_CHECK((mask & nir_fp_no_transform) != 0);
   glslopt_shader_delete(s);
   glslopt_cleanup(ctx);
   return 0;
}

static int propagation_loop_array()
{
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   // constant-index local array feeding a loop-carried precise accumulator
   const char* src = "#version 450 core\n"
      "uniform vec4 a,b; uniform int n; layout(location=0) out vec4 color;\n"
      "void main(){\n"
      "  float w[2]; w[0]=a.x*b.x; w[1]=a.y*b.y;\n"
      "  precise float acc = w[0];\n"
      "  for (int i=0;i<n;i++){ acc += w[1]*b.z; }\n"
      "  color = vec4(acc);\n"
      "}\n";
   glslopt_shader* s = compile_ok(ctx, kGlslOptShaderFragment, src);
   TASK_CHECK(s != nullptr);
   const nir_shader* n = task_shader_nir(s);
   TASK_CHECK(n != nullptr);
   // the array-element multiplies and the loop-carried add must be exact;
   // the add inside the loop (back edge value) is among them
   TASK_CHECK(count_exact_op(n, nir_op_fmul) >= 2);
   TASK_CHECK(count_exact_op(n, nir_op_fadd) >= 1);
   // there must be a loop with an exact fadd on the carried value
   bool loop_exact_add = false;
   nir_foreach_function_impl(impl, (nir_shader*)n) {
      nir_foreach_block(block, impl) {
         if (!block->cf_node.parent || block->cf_node.parent->type != nir_cf_node_loop)
            continue;
         nir_foreach_instr(instr, block) {
            if (instr->type == nir_instr_type_alu) {
               nir_alu_instr* a = nir_instr_as_alu(instr);
               if (a->op == nir_op_fadd && nir_alu_instr_is_exact(a))
                  loop_exact_add = true;
            }
         }
      }
   }
   TASK_CHECK(loop_exact_add);
   glslopt_shader_delete(s);
   glslopt_cleanup(ctx);
   return 0;
}

static int propagation_function_merge()
{
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   // non-recursive helper with in/out parameters, plus a branch merge
   const char* src = "#version 450 core\n"
      "uniform vec4 a,b,c; layout(location=0) out vec4 color;\n"
      "void helper(in float x, in float y, out float o){ o = x*y + x; }\n"
      "void main(){\n"
      "  float h; helper(a.x, b.y, h);\n"
      "  precise float r;\n"
      "  if (c.x > 0.0) r = h + c.y; else r = h - c.y;\n"
      "  color = vec4(r);\n"
      "}\n";
   glslopt_shader* s = compile_ok(ctx, kGlslOptShaderFragment, src);
   TASK_CHECK(s != nullptr);
   const nir_shader* n = task_shader_nir(s);
   TASK_CHECK(n != nullptr);
   // the helper's multiply and add (inlined, flowing through its out param)
   // and the merge-feeding add must be exact
   TASK_CHECK(count_exact_op(n, nir_op_fmul) >= 1);
   TASK_CHECK(count_exact_op(n, nir_op_fadd) >= 1);
   glslopt_shader_delete(s);
   glslopt_cleanup(ctx);
   return 0;
}

static int propagation_independent()
{
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   // a precise chain next to an independent, redundant computation
   const char* src = "#version 450 core\n"
      "uniform vec4 a,b; layout(location=0) out vec4 color;\n"
      "void main(){\n"
      "  precise float r = a.x * b.x + a.y;\n"
      "  float s = a.z * b.z;\n"
      "  float s2 = a.z * b.z;\n"
      "  color = vec4(r, s, s2, 1.0);\n"
      "}\n";
   glslopt_shader* s = compile_ok(ctx, kGlslOptShaderFragment, src);
   TASK_CHECK(s != nullptr);
   const nir_shader* n = task_shader_nir(s);
   TASK_CHECK(n != nullptr);
   // the precise chain is exact
   TASK_CHECK(count_exact_op(n, nir_op_fmul) == 1);
   TASK_CHECK(count_exact_op(n, nir_op_fadd) == 1);
   // the independent multiply is NOT exact and its duplicate was eliminated
   // by CSE (only one non-exact fmul remains)
   int total_fmul = count_op(n, nir_op_fmul);
   TASK_CHECK(total_fmul == 2); // one exact (precise), one shared (independent)
   glslopt_shader_delete(s);
   glslopt_cleanup(ctx);
   return 0;
}

static int propagation_single_component()
{
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   // only one component of a vector feeds the precise value, through a swizzle
   const char* src = "#version 450 core\n"
      "uniform vec4 a,b,c; layout(location=0) out vec4 color;\n"
      "void main(){ vec4 t = a*b; precise float r = t.y + c.y; color = vec4(r); }\n";
   glslopt_shader* s = compile_ok(ctx, kGlslOptShaderFragment, src);
   TASK_CHECK(s != nullptr);
   const nir_shader* n = task_shader_nir(s);
   TASK_CHECK(n != nullptr);
   // the vector multiply whose .y component the precise add reads, and the add
   // itself, are exact
   TASK_CHECK(count_exact_op(n, nir_op_fmul) == 1);
   TASK_CHECK(count_exact_op(n, nir_op_fadd) == 1);
   TASK_CHECK(count_exact_alu(n) == 2);
   glslopt_shader_delete(s);
   glslopt_cleanup(ctx);
   return 0;
}

// ---------------------------------------------------------------------------
// emission group: the generated GLSL must carry precise constraints in legal
// scope, and recompiling it must recover the same protected chain.

static int emission_keyword_and_recompile()
{
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   const char* src = "#version 450 core\n"
      "uniform vec4 a,b,c; layout(location=0) out vec4 color;\n"
      "void main(){ vec4 t=a*b; precise vec4 r=t+c; color=r; }\n";
   glslopt_shader* s = compile_ok(ctx, kGlslOptShaderFragment, src);
   TASK_CHECK(s != nullptr);
   const char* out = glslopt_get_output(s);
   TASK_CHECK(out != nullptr);
   // 'precise' appears on a real declaration (followed by a type), not just
   // appended at the end
   TASK_CHECK(has_keyword(out, "precise vec4"));
   int exact_before = count_exact_alu(task_shader_nir(s));
   TASK_CHECK(exact_before == 2);
   // recompile the emitted source: the same chain must survive
   glslopt_shader* s2 = compile_ok(ctx, kGlslOptShaderFragment, out);
   TASK_CHECK(s2 != nullptr);
   TASK_CHECK(count_exact_op(task_shader_nir(s2), nir_op_fmul) == 1);
   TASK_CHECK(count_exact_op(task_shader_nir(s2), nir_op_fadd) == 1);
   // and the second emission still carries the constraints
   TASK_CHECK(has_keyword(glslopt_get_output(s2), "precise vec4"));
   glslopt_shader_delete(s);
   glslopt_shader_delete(s2);
   glslopt_cleanup(ctx);
   return 0;
}

static int emission_loop_temp()
{
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   const char* src = "#version 450 core\n"
      "uniform vec4 a,b; uniform int n; layout(location=0) out vec4 color;\n"
      "void main(){\n"
      "  precise float acc = 0.0;\n"
      "  for (int i=0;i<n;i++){ acc += a[i] * b[i]; }\n"
      "  color = vec4(acc);\n"
      "}\n";
   glslopt_shader* s = compile_ok(ctx, kGlslOptShaderFragment, src);
   TASK_CHECK(s != nullptr);
   const char* out = glslopt_get_output(s);
   TASK_CHECK(out != nullptr);
   // the loop-carried temporary storage is declared precise
   TASK_CHECK(has_keyword(out, "precise float"));
   // recompile: the loop-carried chain (mul + add) is still exact
   glslopt_shader* s2 = compile_ok(ctx, kGlslOptShaderFragment, out);
   TASK_CHECK(s2 != nullptr);
   TASK_CHECK(count_exact_op(task_shader_nir(s2), nir_op_fmul) >= 1);
   TASK_CHECK(count_exact_op(task_shader_nir(s2), nir_op_fadd) >= 1);
   glslopt_shader_delete(s);
   glslopt_shader_delete(s2);
   glslopt_cleanup(ctx);
   return 0;
}

// CPU bridge: the generated code for a precise fragment must execute the
// stepwise float32 expression, on inputs where contraction/reassociation
// would give a different answer.
static int emission_cpu_stepwise()
{
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   // (a + b) - a : algebraically b, but not in float32 for these inputs
   const char* src = "#version 450 core\n"
      "uniform float a,b; layout(location=0) out vec4 color;\n"
      "void main(){ precise float r = (a + b) - a; color = vec4(r); }\n";
   glslopt_shader* s = compile_ok(ctx, kGlslOptShaderFragment, src);
   TASK_CHECK(s != nullptr);
   float a = 1.0e20f, b = 1.0f;
   // stepwise float32: round(round(a+b)-a); the algebraic value is b
   float expect = step(step(a + b) - a);
   float algebraic = b;
   TASK_CHECK(expect != algebraic); // inputs are sensitive to the rounding steps
   std::vector<float> outv;
   TASK_CHECK(run_shader(task_shader_nir(s), {{"a", {a}}, {"b", {b}}}, "color", outv));
   TASK_CHECK(outv.size() == 4);
   TASK_CHECK(outv[0] == expect);     // generated code computes the preserved expression
   TASK_CHECK(outv[0] != algebraic);  // ... not the reassociated one
   glslopt_shader_delete(s);
   glslopt_cleanup(ctx);
   return 0;
}

// CPU bridge over a loop-carried precise accumulator: the generated loop must
// execute the running sum with its float32 rounding steps.
static int emission_cpu_loop()
{
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   const char* src = "#version 450 core\n"
      "uniform vec4 a,b; uniform int n; layout(location=0) out vec4 color;\n"
      "void main(){\n"
      "  precise float acc = 0.0;\n"
      "  for (int i=0;i<n;i++){ acc += a[i] * b[i]; }\n"
      "  color = vec4(acc);\n"
      "}\n";
   glslopt_shader* s = compile_ok(ctx, kGlslOptShaderFragment, src);
   TASK_CHECK(s != nullptr);
   // inputs where the running float32 sum loses the small term
   std::vector<float> av = {1.0e20f, 1.0f, -1.0e20f, 0.0f};
   std::vector<float> bv = {1.0f, 1.0f, 1.0f, 0.0f};
   // stepwise float32 reference: round each product and each running sum
   float acc = 0.0f;
   for (int i = 0; i < 3; i++) {
      volatile float p = av[i] * bv[i];
      volatile float sum = acc + p;
      acc = sum;
   }
   TASK_CHECK(acc == 0.0f); // the small term is lost in the running sum
   std::vector<float> outv;
   TASK_CHECK(run_shader(task_shader_nir(s),
                         {{"a", av}, {"b", bv}, {"n", {3.0f}}}, "color", outv));
   TASK_CHECK(outv.size() == 4);
   TASK_CHECK(outv[0] == acc);
   glslopt_shader_delete(s);
   glslopt_cleanup(ctx);
   return 0;
}

// CPU bridge with the original qualifier samples as controls: the precise
// sample preserves the stepwise multiply-add; the unconstrained sample is a
// valid reference for the same inputs.
static int emission_cpu_control()
{
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   const char* precise_src = "#version 450 core\n"
      "uniform vec4 a,b,c; layout(location=0) out vec4 color;\n"
      "void main(){ vec4 t=a*b; precise vec4 r=t+c; color=r; }\n";
   const char* plain_src = "#version 450 core\n"
      "uniform vec4 a,b,c; layout(location=0) out vec4 color;\n"
      "void main(){ vec4 t=a*b; color=t+c; }\n";
   // inputs where stepwise float32 differs from a contracted fma
   float av = 1.0f + 1.0e-7f, bv = 1.0f - 1.0e-7f, cv = -1.0f;
   float stepwise = step(step(av * bv) + cv);
   float contracted = std::fma(av, bv, cv);
   TASK_CHECK(stepwise != contracted); // the inputs distinguish the rounding steps

   std::map<std::string, std::vector<float>> inputs = {
      {"a", {av, av, av, av}}, {"b", {bv, bv, bv, bv}}, {"c", {cv, cv, cv, cv}},
   };
   glslopt_shader* sp = compile_ok(ctx, kGlslOptShaderFragment, precise_src);
   TASK_CHECK(sp != nullptr);
   std::vector<float> outv;
   TASK_CHECK(run_shader(task_shader_nir(sp), inputs, "color", outv));
   TASK_CHECK(outv.size() == 4);
   // the precise sample executes the stepwise expression, not the contraction
   TASK_CHECK(outv[0] == stepwise);
   TASK_CHECK(outv[0] != contracted);
   glslopt_shader_delete(sp);

   // control: the unconstrained sample computes a*b+c too (here the optimizer
   // keeps the same shape); it must at least be a finite, consistent result
   glslopt_shader* sn = compile_ok(ctx, kGlslOptShaderFragment, plain_src);
   TASK_CHECK(sn != nullptr);
   TASK_CHECK(!has_keyword(glslopt_get_output(sn), "precise"));
   std::vector<float> outn;
   TASK_CHECK(run_shader(task_shader_nir(sn), inputs, "color", outn));
   TASK_CHECK(outn.size() == 4 && std::isfinite(outn[0]));
   glslopt_shader_delete(sn);
   glslopt_cleanup(ctx);
   return 0;
}

// ---------------------------------------------------------------------------
// compat group: shaders without precise are unchanged, and interleaved
// compilations stay independent.

static int compat_no_precise_unchanged()
{
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   const char* src = "#version 450 core\n"
      "uniform vec4 a,b,c; layout(location=0) out vec4 color;\n"
      "void main(){ vec4 t=a*b; color=t+c; }\n";
   glslopt_shader* s = compile_ok(ctx, kGlslOptShaderFragment, src);
   TASK_CHECK(s != nullptr);
   const char* out = glslopt_get_output(s);
   TASK_CHECK(out != nullptr);
   // no precise keyword, and the expression is folded exactly as before
   TASK_CHECK(!has_keyword(out, "precise"));
   TASK_CHECK(has_keyword(out, "color = ((a * b) + c);"));
   // no exact instructions leak into an unconstrained shader
   TASK_CHECK(count_exact_alu(task_shader_nir(s)) == 0);
   // reflection is unchanged: three vec4 uniforms
   TASK_CHECK(glslopt_shader_get_uniform_count(s) == 3);
   glslopt_shader_delete(s);
   glslopt_cleanup(ctx);
   return 0;
}

static int compat_interleaved_independent()
{
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   const char* precise_src = "#version 450 core\n"
      "uniform vec4 a,b,c; layout(location=0) out vec4 color;\n"
      "void main(){ vec4 t=a*b; precise vec4 r=t+c; color=r; }\n";
   const char* plain_src = "#version 450 core\n"
      "uniform vec4 a,b,c; layout(location=0) out vec4 color;\n"
      "void main(){ vec4 t=a*b; color=t+c; }\n";
   // interleave precise and plain compilations in one context
   glslopt_shader* p1 = compile_ok(ctx, kGlslOptShaderFragment, precise_src);
   glslopt_shader* n1 = compile_ok(ctx, kGlslOptShaderFragment, plain_src);
   glslopt_shader* p2 = compile_ok(ctx, kGlslOptShaderFragment, precise_src);
   TASK_CHECK(p1 && n1 && p2);
   TASK_CHECK(has_keyword(glslopt_get_output(p1), "precise"));
   TASK_CHECK(!has_keyword(glslopt_get_output(n1), "precise"));
   TASK_CHECK(has_keyword(glslopt_get_output(p2), "precise"));
   // the plain compile between two precise ones is unconstrained
   TASK_CHECK(count_exact_alu(task_shader_nir(n1)) == 0);
   // delete a precise shader, then a fresh plain compile is still clean
   glslopt_shader_delete(p1);
   glslopt_shader_delete(p2);
   glslopt_shader* n2 = compile_ok(ctx, kGlslOptShaderFragment, plain_src);
   TASK_CHECK(n2 != nullptr);
   TASK_CHECK(!has_keyword(glslopt_get_output(n2), "precise"));
   TASK_CHECK(count_exact_alu(task_shader_nir(n2)) == 0);
   glslopt_shader_delete(n1);
   glslopt_shader_delete(n2);
   glslopt_cleanup(ctx);
   return 0;
}

static int compat_invariant_unchanged()
{
   glslopt_ctx* ctx = glslopt_initialize(kGlslTargetOpenGL);
   // invariant (a different qualifier) must not gain precise declarations
   const char* src = "#version 450 core\n"
      "in vec4 pos; uniform mat4 m; invariant out vec4 pout;\n"
      "void main(){ vec4 p = m * pos; pout = p; gl_Position = p; }\n";
   glslopt_shader* s = compile_ok(ctx, kGlslOptShaderVertex, src);
   TASK_CHECK(s != nullptr);
   const char* out = glslopt_get_output(s);
   TASK_CHECK(out != nullptr);
   TASK_CHECK(has_keyword(out, "invariant"));
   TASK_CHECK(!has_keyword(out, "precise"));
   glslopt_shader_delete(s);
   glslopt_cleanup(ctx);
   return 0;
}

// ---------------------------------------------------------------------------

const struct task_case task_cases[] = {
   {"propagation", "temp_chain", propagation_temp_chain},
   {"propagation", "loop_array", propagation_loop_array},
   {"propagation", "function_merge", propagation_function_merge},
   {"propagation", "independent", propagation_independent},
   {"propagation", "single_component", propagation_single_component},
   {"emission", "keyword_and_recompile", emission_keyword_and_recompile},
   {"emission", "loop_temp", emission_loop_temp},
   {"emission", "cpu_stepwise", emission_cpu_stepwise},
   {"emission", "cpu_loop", emission_cpu_loop},
   {"emission", "cpu_control", emission_cpu_control},
   {"compat", "no_precise_unchanged", compat_no_precise_unchanged},
   {"compat", "interleaved_independent", compat_interleaved_independent},
   {"compat", "invariant_unchanged", compat_invariant_unchanged},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
