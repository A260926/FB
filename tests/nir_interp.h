// A small, self-contained CPU interpreter for the subset of optimized NIR that
// the specialization tests generate. It is a numeric execution bridge, not a
// GLSL implementation: the same shader NIR is run twice (once built from the
// original shader with runtime uniform values, once specialized), and the
// outputs must match exactly. It also counts texture operations so a test can
// prove a sampled branch really disappeared.
//
// Only the opcodes/intrinsics exercised by the checked-in tests are handled;
// anything unsupported fails loudly instead of silently miscomputing.
#pragma once

#include "compiler/nir/nir.h"
#include "compiler/glsl_types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace nir_interp {

struct Value {
   unsigned n = 0;                 // number of live components (1..4)
   unsigned bit_size = 32;
   bool is_bool = false;
   uint32_t u[4] = {};

   float f(unsigned i = 0) const { float v; memcpy(&v, &u[i], 4); return v; }
   int32_t i(unsigned i = 0) const { int32_t v; memcpy(&v, &u[i], 4); return v; }
   bool b(unsigned i = 0) const { return u[i] != 0; }
   void setF(float x) { memcpy(&u[0], &x, 4); n = 1; bit_size = 32; }
   void setI(int32_t x) { memcpy(&u[0], &x, 4); n = 1; bit_size = 32; }
   static Value fromFloat(float x) { Value v; v.setF(x); return v; }
   static Value fromInt(int32_t x) { Value v; v.setI(x); return v; }
   static Value fromBool(bool x) { Value v; v.n = 1; v.bit_size = 1; v.is_bool = true; v.u[0] = x ? ~0u : 0u; return v; }
};

// Number of 32-bit words occupied by one instance of a type (matrices are
// flattened column-major; the test shaders avoid them).
unsigned word_count(const glsl_type* t)
{
   if (glsl_type_is_array(t))
      return glsl_get_length(t) * word_count(glsl_get_array_element(t));
   if (glsl_type_is_struct(t)) {
      unsigned total = 0;
      for (unsigned i = 0; i < glsl_get_length(t); i++)
         total += word_count(glsl_get_struct_field(t, i));
      return total;
   }
   if (glsl_type_is_matrix(t))
      return glsl_get_matrix_columns(t) * glsl_get_vector_elements(glsl_get_column_type(t));
   return glsl_get_vector_elements(t);
}

unsigned struct_word_offset(const glsl_type* t, unsigned field)
{
   unsigned off = 0;
   for (unsigned i = 0; i < field; i++)
      off += word_count(glsl_get_struct_field(t, i));
   return off;
}

enum Signal { signal_none, signal_break, signal_continue, signal_return };

class Interpreter {
public:
   explicit Interpreter(nir_shader* sh) : shader(sh) {}

   unsigned texture_calls = 0;
   std::string error;

   // Host data, as flattened 32-bit leaves in declaration/column order. The
   // test supplies one word per scalar; booleans use the low bit.
   void set_var_words(const std::string& name, const std::vector<uint32_t>& words)
   {
      nir_variable* var = find_var(name);
      if (!var)
         return; // fully specialized away: nothing to feed
      std::vector<uint32_t>& mem = memory[var];
      mem.assign(word_count(var->type), 0);
      for (size_t i = 0; i < words.size() && i < mem.size(); i++)
         mem[i] = words[i];
   }

   const Value& output(const std::string& name)
   {
      static Value empty;
      nir_variable* var = find_var(name);
      auto it = output_cache.find(name);
      if (it != output_cache.end())
         return it->second;
      if (!var) { error = "interp: no output named " + name; return empty; }
      const std::vector<uint32_t>& mem = mem_for(var);
      Value v;
      v.n = glsl_get_vector_elements(glsl_without_array(var->type));
      for (unsigned i = 0; i < v.n && i < mem.size(); i++)
         v.u[i] = mem[i];
      output_cache[name] = v;
      return output_cache[name];
   }

   bool run()
   {
      ssa.clear();
      regs.clear();
      texture_calls = 0;
      nir_function_impl* impl = nir_shader_get_entrypoint(shader);
      Signal sig = signal_none;
      exec_list(impl->body, sig);
      return error.empty();
   }

private:
   nir_shader* shader;
   std::map<nir_variable*, std::vector<uint32_t>> memory;
   std::map<unsigned, Value> ssa;          // keyed by nir_def::index
   std::map<nir_def*, Value> regs;         // decl_reg def -> current value
   std::map<std::string, Value> output_cache;

   nir_variable* find_var(const std::string& name)
   {
      nir_foreach_variable_in_shader(v, shader)
         if (v->name && name == v->name)
            return v;
      return nullptr;
   }

   // Lazily sized backing store; outputs start zeroed and grow on first store.
   std::vector<uint32_t>& mem_for(nir_variable* var)
   {
      std::vector<uint32_t>& mem = memory[var];
      if (mem.empty())
         mem.assign(word_count(var->type), 0);
      return mem;
   }

   static uint32_t bits(float x) { uint32_t u; memcpy(&u, &x, 4); return u; }
   static uint32_t bits(int32_t x) { uint32_t u; memcpy(&u, &x, 4); return u; }

   Value& def(nir_def* d) { return ssa[d->index]; }

   Value src_value(nir_src src, unsigned channel)
   {
      Value base = ssa[src.ssa->index];
      Value out = base;
      if (base.n > 1) {
         unsigned c = channel < base.n ? channel : 0;
         out.u[0] = base.u[c];
         out.n = 1;
      }
      return out;
   }

   // Resolve a deref chain to (variable, leaf word offset).
   nir_variable* resolve_deref(nir_deref_instr* leaf, unsigned* word_off, const glsl_type** leaf_type)
   {
      std::vector<nir_deref_instr*> chain;
      for (nir_deref_instr* d = leaf; d; d = nir_deref_instr_parent(d))
         chain.push_back(d);
      nir_variable* var = chain.back()->var;
      const glsl_type* cur = var->type;
      unsigned off = 0;
      for (int k = (int)chain.size() - 2; k >= 0; k--) {
         nir_deref_instr* d = chain[k];
         if (d->deref_type == nir_deref_type_struct) {
            off += struct_word_offset(cur, d->strct.index);
            cur = glsl_get_struct_field(cur, d->strct.index);
         } else if (d->deref_type == nir_deref_type_array) {
            unsigned idx = (unsigned)(int32_t)ssa[d->arr.index.ssa->index].u[0];
            if (glsl_type_is_array(cur)) {
               off += idx * word_count(glsl_get_array_element(cur));
               cur = glsl_get_array_element(cur);
            } else {
               off += idx; // vector component
               cur = glsl_scalar_type(glsl_get_base_type(cur));
            }
         } else {
            error = "interp: unsupported deref type";
            return nullptr;
         }
      }
      *word_off = off;
      if (leaf_type)
         *leaf_type = leaf->type;
      return var;
   }

   void exec_list(const struct exec_list& nodes, Signal& sig)
   {
      foreach_list_typed(nir_cf_node, node, node, &nodes) {
         if (sig != signal_none)
            return;
         if (node->type == nir_cf_node_block)
            exec_block(nir_cf_node_as_block(node), sig);
         else if (node->type == nir_cf_node_if)
            exec_if(nir_cf_node_as_if(node), sig);
         else if (node->type == nir_cf_node_loop)
            exec_loop(nir_cf_node_as_loop(node), sig);
      }
   }

   void exec_block(nir_block* block, Signal& sig)
   {
      nir_foreach_instr(instr, block) {
         if (sig != signal_none)
            return;
         exec_instr(instr, sig);
      }
   }

   void exec_if(nir_if* if_stmt, Signal& sig)
   {
      Value cond = ssa[if_stmt->condition.ssa->index];
      if (cond.b(0))
         exec_list(if_stmt->then_list, sig);
      else
         exec_list(if_stmt->else_list, sig);
   }

   void exec_loop(nir_loop* loop, Signal& sig)
   {
      // NIR loops are "while (true)" and only leave through break/return; a
      // continue restarts the body.
      for (int guard = 0; guard < 100000; guard++) {
         Signal inner = signal_none;
         exec_list(loop->body, inner);
         if (inner == signal_break) { sig = signal_none; return; }
         if (inner == signal_return) { sig = signal_return; return; }
      }
      error = "interp: loop iteration guard exceeded";
      sig = signal_return;
   }

   void exec_instr(nir_instr* instr, Signal& sig)
   {
      switch (instr->type) {
      case nir_instr_type_alu: exec_alu(nir_instr_as_alu(instr)); break;
      case nir_instr_type_intrinsic: exec_intrinsic(nir_instr_as_intrinsic(instr)); break;
      case nir_instr_type_deref: break; // resolved on demand
      case nir_instr_type_tex: exec_tex(nir_instr_as_tex(instr)); break;
      case nir_instr_type_load_const: exec_const(nir_instr_as_load_const(instr)); break;
      case nir_instr_type_undef: {
         Value v; v.n = nir_instr_as_undef(instr)->def.num_components;
         v.bit_size = nir_instr_as_undef(instr)->def.bit_size;
         ssa[nir_instr_as_undef(instr)->def.index] = v;
         break;
      }
      case nir_instr_type_jump: {
         switch (nir_instr_as_jump(instr)->type) {
         case nir_jump_break: sig = signal_break; break;
         case nir_jump_continue: sig = signal_continue; break;
         case nir_jump_return: sig = signal_return; break;
         default: error = "interp: unsupported jump"; break;
         }
         break;
      }
      default: error = "interp: unsupported instruction type"; break;
      }
   }

   void exec_const(nir_load_const_instr* lc)
   {
      Value v;
      v.n = lc->def.num_components;
      v.bit_size = lc->def.bit_size;
      v.is_bool = lc->def.bit_size == 1;
      for (unsigned i = 0; i < v.n; i++)
         v.u[i] = lc->value[i].u32;
      ssa[lc->def.index] = v;
   }

   void exec_intrinsic(nir_intrinsic_instr* in)
   {
      switch (in->intrinsic) {
      case nir_intrinsic_decl_reg:
         regs[&in->def] = Value{};
         regs[&in->def].n = in->def.num_components;
         regs[&in->def].bit_size = in->def.bit_size;
         break;
      case nir_intrinsic_load_reg: {
         nir_def* reg = in->src[0].ssa;
         ssa[in->def.index] = regs[reg];
         break;
      }
      case nir_intrinsic_store_reg: {
         Value val = ssa[in->src[0].ssa->index];
         nir_def* reg = in->src[1].ssa;
         Value cur = regs[reg];
         unsigned mask = nir_intrinsic_write_mask(in);
         for (unsigned i = 0; i < val.n; i++)
            if (mask & (1 << i))
               cur.u[i] = val.u[i];
         cur.n = regs[reg].n ? regs[reg].n : val.n;
         regs[reg] = cur;
         break;
      }
      case nir_intrinsic_load_deref: {
         unsigned off;
         nir_variable* var = resolve_deref(nir_src_as_deref(in->src[0]), &off, nullptr);
         if (!var) return;
         const std::vector<uint32_t>& mem = mem_for(var);
         Value v;
         v.n = in->def.num_components;
         v.bit_size = in->def.bit_size;
         v.is_bool = in->def.bit_size == 1;
         for (unsigned i = 0; i < v.n; i++)
            v.u[i] = off + i < mem.size() ? mem[off + i] : 0;
         ssa[in->def.index] = v;
         break;
      }
      case nir_intrinsic_store_deref: {
         unsigned off;
         nir_variable* var = resolve_deref(nir_src_as_deref(in->src[0]), &off, nullptr);
         if (!var) return;
         std::vector<uint32_t>& mem = mem_for(var);
         Value val = ssa[in->src[1].ssa->index];
         unsigned mask = nir_intrinsic_write_mask(in);
         for (unsigned i = 0; i < val.n; i++)
            if ((mask & (1 << i)) && off + i < mem.size())
               mem[off + i] = val.u[i];
         break;
      }
      case nir_intrinsic_glslopt_precision:
         ssa[in->def.index] = ssa[in->src[0].ssa->index];
         break;
      default:
         error = std::string("interp: unsupported intrinsic ") + nir_intrinsic_infos[in->intrinsic].name;
         break;
      }
   }

   void exec_tex(nir_tex_instr* tex)
   {
      texture_calls++;
      // Deterministic stand-in: derive a vec4 from the coordinate words, so the
      // same inputs always yield the same "texel" on both shader variants.
      Value coord;
      for (unsigned i = 0; i < tex->num_srcs; i++) {
         if (tex->src[i].src_type == nir_tex_src_coord) {
            coord = ssa[tex->src[i].src.ssa->index];
            break;
         }
      }
      Value v;
      v.n = 4;
      for (unsigned i = 0; i < 4; i++) {
         float x = coord.n ? coord.f(i % coord.n) : 0.0f;
         v.u[i] = bits(std::fmod(std::fabs(x) * (1.0f + 0.25f * i) + 0.1f * i, 1.0f));
      }
      ssa[tex->def.index] = v;
   }

   // ---- ALU ---------------------------------------------------------------
   static bool is_float_alu(nir_alu_instr* alu)
   {
      return nir_alu_type_get_base_type(nir_op_infos[alu->op].output_type) == nir_type_float;
   }

   Value alu_input(nir_alu_instr* alu, unsigned s, unsigned comp)
   {
      nir_alu_src& asrc = alu->src[s];
      Value base = ssa[asrc.src.ssa->index];
      unsigned c = asrc.swizzle[comp < base.n ? comp : 0];
      Value out;
      out.n = 1;
      out.bit_size = base.bit_size;
      out.is_bool = base.is_bool;
      out.u[0] = base.u[c < base.n ? c : 0];
      return out;
   }

   void set_scalar(nir_alu_instr* alu, Value dst[4], unsigned comp, Value v)
   {
      v.n = 1;
      dst[comp] = v;
   }

   void exec_alu(nir_alu_instr* alu)
   {
      unsigned n = alu->def.num_components;
      Value dst[4];
      for (unsigned c = 0; c < n; c++) {
         dst[c].n = 1;
         dst[c].bit_size = alu->def.bit_size;
         dst[c].is_bool = alu->def.bit_size == 1;
      }
      nir_op op = alu->op;
      bool float_out = is_float_alu(alu);

      auto finput = [&](unsigned s, unsigned c) { return alu_input(alu, s, c).f(); };
      auto iinput = [&](unsigned s, unsigned c) { return alu_input(alu, s, c).i(); };
      auto uinput = [&](unsigned s, unsigned c) { return (uint32_t)alu_input(alu, s, c).i(); };
      auto binput = [&](unsigned s, unsigned c) { return alu_input(alu, s, c).b(); };

      for (unsigned c = 0; c < n; c++) {
         Value r;
         r.n = 1;
         r.bit_size = alu->def.bit_size;
         r.is_bool = alu->def.bit_size == 1;
         switch (op) {
         case nir_op_mov: r = alu_input(alu, 0, c); break;
         case nir_op_vec2: case nir_op_vec3: case nir_op_vec4:
            r = alu_input(alu, c, 0); break;
         case nir_op_fadd: r.setF(finput(0, c) + finput(1, c)); break;
         case nir_op_fsub: r.setF(finput(0, c) - finput(1, c)); break;
         case nir_op_fmul: r.setF(finput(0, c) * finput(1, c)); break;
         case nir_op_fdiv: r.setF(finput(0, c) / finput(1, c)); break;
         case nir_op_fneg: r.setF(-finput(0, c)); break;
         case nir_op_fabs: r.setF(std::fabs(finput(0, c))); break;
         case nir_op_fsign: { float x = finput(0, c); r.setF((x > 0) - (x < 0)); break; }
         case nir_op_ffloor: r.setF(std::floor(finput(0, c))); break;
         case nir_op_fceil: r.setF(std::ceil(finput(0, c))); break;
         case nir_op_ftrunc: r.setF(std::trunc(finput(0, c))); break;
         case nir_op_ffract: r.setF(finput(0, c) - std::floor(finput(0, c))); break;
         case nir_op_fsat: { float x = finput(0, c); r.setF(x < 0 ? 0 : x > 1 ? 1 : x); break; }
         case nir_op_flrp: r.setF(finput(0, c) * (1 - finput(2, c)) + finput(1, c) * finput(2, c)); break;
         case nir_op_fmin: r.setF(std::fmin(finput(0, c), finput(1, c))); break;
         case nir_op_fmax: r.setF(std::fmax(finput(0, c), finput(1, c))); break;
         case nir_op_iadd: r.setI(iinput(0, c) + iinput(1, c)); break;
         case nir_op_isub: r.setI(iinput(0, c) - iinput(1, c)); break;
         case nir_op_imul: r.setI(iinput(0, c) * iinput(1, c)); break;
         case nir_op_idiv: r.setI(iinput(0, c) / iinput(1, c)); break;
         case nir_op_irem: r.setI(iinput(0, c) % iinput(1, c)); break;
         case nir_op_umod: r.u[0] = uinput(0, c) % uinput(1, c); break;
         case nir_op_udiv: r.u[0] = uinput(0, c) / uinput(1, c); break;
         case nir_op_ineg: r.setI(-iinput(0, c)); break;
         case nir_op_imin: r.setI(std::min(iinput(0, c), iinput(1, c))); break;
         case nir_op_imax: r.setI(std::max(iinput(0, c), iinput(1, c))); break;
         case nir_op_umin: r.u[0] = std::min(uinput(0, c), uinput(1, c)); break;
         case nir_op_umax: r.u[0] = std::max(uinput(0, c), uinput(1, c)); break;
         case nir_op_ishl: r.u[0] = uinput(0, c) << (uinput(1, c) & 31); break;
         case nir_op_ishr: r.setI(iinput(0, c) >> (iinput(1, c) & 31)); break;
         case nir_op_ushr: r.u[0] = uinput(0, c) >> (uinput(1, c) & 31); break;
         case nir_op_iand: r.u[0] = uinput(0, c) & uinput(1, c); break;
         case nir_op_ior: r.u[0] = uinput(0, c) | uinput(1, c); break;
         case nir_op_ixor: r.u[0] = uinput(0, c) ^ uinput(1, c); break;
         case nir_op_inot: r.u[0] = ~uinput(0, c); break;
         case nir_op_feq: r = Value::fromBool(finput(0, c) == finput(1, c)); break;
         case nir_op_fneu: r = Value::fromBool(finput(0, c) != finput(1, c)); break;
         case nir_op_flt: r = Value::fromBool(finput(0, c) < finput(1, c)); break;
         case nir_op_fge: r = Value::fromBool(finput(0, c) >= finput(1, c)); break;
         case nir_op_ieq: r = Value::fromBool(iinput(0, c) == iinput(1, c)); break;
         case nir_op_ine: r = Value::fromBool(iinput(0, c) != iinput(1, c)); break;
         case nir_op_ilt: r = Value::fromBool(iinput(0, c) < iinput(1, c)); break;
         case nir_op_ige: r = Value::fromBool(iinput(0, c) >= iinput(1, c)); break;
         case nir_op_ult: r = Value::fromBool(uinput(0, c) < uinput(1, c)); break;
         case nir_op_uge: r = Value::fromBool(uinput(0, c) >= uinput(1, c)); break;
         case nir_op_bcsel:
            r = binput(0, c) ? alu_input(alu, 1, c) : alu_input(alu, 2, c);
            break;
         case nir_op_b2f32: r.setF(binput(0, c) ? 1.0f : 0.0f); break;
         case nir_op_b2i32: r.setI(binput(0, c) ? 1 : 0); break;
         case nir_op_i2f32: r.setF((float)iinput(0, c)); break;
         case nir_op_u2f32: r.setF((float)uinput(0, c)); break;
         case nir_op_f2i32: r.setI((int32_t)finput(0, c)); break;
         case nir_op_f2u32: r.u[0] = (uint32_t)(int32_t)finput(0, c); break;
         case nir_op_f2f16: case nir_op_f2f32: case nir_op_i2i32: case nir_op_u2u32: case nir_op_b2b1:
            r = alu_input(alu, 0, c); break;
         case nir_op_fdot2: case nir_op_fdot3: case nir_op_fdot4: {
            float acc = 0;
            for (unsigned k = 0; k < n; k++)
               acc += finput(0, k) * finput(1, k);
            r.setF(acc);
            break;
         }
         default:
            error = std::string("interp: unsupported ALU op ") + nir_op_infos[op].name;
            return;
         }
         dst[c] = r;
      }
      (void)float_out;
      Value out;
      out.n = n;
      out.bit_size = alu->def.bit_size;
      out.is_bool = alu->def.bit_size == 1;
      for (unsigned c = 0; c < n; c++)
         out.u[c] = dst[c].u[0];
      ssa[alu->def.index] = out;
   }
};

} // namespace nir_interp
