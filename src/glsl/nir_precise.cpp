// Backward propagation of 'precise' constraints through the NIR value flow.
//
// The GLSL 'precise' qualifier requires that every operation contributing to a
// precise variable is evaluated with exactly the rounding steps written in the
// source: no reassociation, no mul+add contraction, no cancellation, no
// algebraic rules that ignore NaN/Inf.  The frontend records 'precise' on
// nir_variables; this pass walks the def-use chain backwards from the stores
// into those variables and marks the contributing float32 ALU instructions
// exact, so that the optimizer preserves their arithmetic.
//
// It runs before SSA conversion and before the algebraic pipeline, while the
// value flow through temporaries, local arrays, helper-function copies and
// branch/loop merges is still explicit as load/store/copy of local variables.
// Tracking is per component: an exact result component only constrains the
// source components it is actually computed from, so independent vector
// components and unrelated outputs keep their regular optimization.
//
// Only float32 scalar/vector add/sub/mul/div/negate, constructors, selects and
// swizzles take part; integer math, textures, atomics and explicit fma are out
// of scope and stop the walk (their values are inputs to the chain, not
// something this pass can constrain).

#include "nir_optimize.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/glsl_types.h"
#include <map>
#include <vector>

namespace {

// The exactness mask used for precise values: keep the rounding steps and the
// NaN/Inf behavior of the written expression.
const uint32_t kExactMask = nir_fp_exact | nir_fp_preserve_nan | nir_fp_preserve_inf;

// number of scalar leaf slots in a type (vec components, array elements, ...)
static unsigned
type_slots(const glsl_type* t)
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
   return 0; // opaque types are not tracked
}

// slots of one element when indexing into an array/matrix/vector
static unsigned
child_slots(const glsl_type* parent)
{
   return type_slots(glsl_get_array_element(parent));
}

static const glsl_type*
child_type(const glsl_type* parent)
{
   return glsl_get_array_element(parent);
}

// Can this opcode carry a float32 precise value to its consumers?
static bool
op_carries_float(nir_op op)
{
   switch (op) {
   case nir_op_mov:
   case nir_op_vec2:
   case nir_op_vec3:
   case nir_op_vec4:
   case nir_op_bcsel:
      return true; // typeless: selection and routing of values
   default:
      return nir_alu_type_get_base_type(nir_op_infos[op].output_type) == nir_type_float;
   }
}

static bool
trackable_variable(const nir_variable* var)
{
   return var->data.mode == nir_var_function_temp ||
          var->data.mode == nir_var_shader_temp ||
          var->data.mode == nir_var_shader_out;
}

struct precise_prop {
   nir_shader* nir;
   nir_function_impl* impl;

   // exact scalar slots per variable (bit i = slot i)
   std::map<nir_variable*, std::vector<uint64_t>> var_slots;
   // store_deref / copy_deref instructions writing each variable
   std::map<nir_variable*, std::vector<nir_intrinsic_instr*>> var_writes;
   // accumulated exact component mask per SSA def
   std::map<nir_def*, uint32_t> def_mask;

   std::vector<std::pair<nir_def*, uint32_t>> def_work;
   std::vector<nir_variable*> var_work;

   std::vector<uint64_t>& slots_of(nir_variable* var)
   {
      auto& s = var_slots[var];
      if (s.empty())
         s.assign((type_slots(var->type) + 63) / 64, 0);
      return s;
   }

   static bool slot_get(const std::vector<uint64_t>& s, unsigned i)
   {
      return i / 64 < s.size() && (s[i / 64] >> (i % 64)) & 1ull;
   }
   static void slot_set(std::vector<uint64_t>& s, unsigned i)
   {
      if (i / 64 < s.size())
         s[i / 64] |= 1ull << (i % 64);
   }

   void require_def(nir_def* def, uint32_t mask);
   void require_slots(nir_variable* var, const std::vector<uint64_t>& bits);
   void require_all_slots(nir_variable* var);

   void process_def(nir_def* def, uint32_t mask);
   void process_var(nir_variable* var);

   // base slot of a deref chain; false if a non-constant index is involved
   static bool base_slot(nir_deref_instr* d, unsigned* out);
};

// static
bool
precise_prop::base_slot(nir_deref_instr* d, unsigned* out)
{
   std::vector<nir_deref_instr*> path;
   for (nir_deref_instr* p = d; p; p = nir_deref_instr_parent(p))
      path.insert(path.begin(), p);
   if (path.empty() || path[0]->deref_type != nir_deref_type_var)
      return false;
   unsigned base = 0;
   const glsl_type* type = path[0]->var->type;
   for (unsigned i = 1; i < path.size(); i++) {
      nir_deref_instr* s = path[i];
      if (s->deref_type == nir_deref_type_struct) {
         for (unsigned f = 0; f < s->strct.index; f++)
            base += type_slots(glsl_get_struct_field(type, f));
         type = glsl_get_struct_field(type, s->strct.index);
      } else if (s->deref_type == nir_deref_type_array) {
         if (!nir_src_is_const(s->arr.index))
            return false;
         base += (unsigned)nir_src_as_uint(s->arr.index) * child_slots(type);
         type = child_type(type);
      } else {
         return false; // casts and other derefs are not tracked
      }
   }
   *out = base;
   return true;
}

void
precise_prop::require_def(nir_def* def, uint32_t mask)
{
   if (!def)
      return;
   mask &= (def->num_components >= 32) ? ~0u : ((1u << def->num_components) - 1);
   if (!mask)
      return;
   uint32_t& cur = def_mask[def];
   uint32_t added = mask & ~cur;
   if (!added)
      return;
   cur |= mask;
   def_work.push_back({def, added});
}

void
precise_prop::require_slots(nir_variable* var, const std::vector<uint64_t>& bits)
{
   if (!trackable_variable(var))
      return;
   std::vector<uint64_t>& cur = slots_of(var);
   bool grew = false;
   for (size_t i = 0; i < cur.size(); i++) {
      uint64_t added = (i < bits.size() ? bits[i] : 0) & ~cur[i];
      if (added) {
         cur[i] |= added;
         grew = true;
      }
   }
   if (!grew)
      return;
   // the variable now holds precise values; declare it precise when printed
   var->data.precise = 1;
   var_work.push_back(var);
}

void
precise_prop::require_all_slots(nir_variable* var)
{
   if (!trackable_variable(var))
      return;
   std::vector<uint64_t> bits = slots_of(var);
   for (auto& w : bits)
      w = ~0ull;
   require_slots(var, bits);
}

void
precise_prop::process_def(nir_def* def, uint32_t mask)
{
   nir_instr* instr = nir_def_instr(def);
   switch (instr->type) {
   case nir_instr_type_alu: {
      nir_alu_instr* alu = nir_instr_as_alu(instr);
      alu->fp_math_ctrl = nir_op_valid_fp_math_ctrl(alu->op, alu->fp_math_ctrl | kExactMask);
      if (!op_carries_float(alu->op))
         return; // integer/bool math is out of scope; do not walk into it
      const nir_op_info* info = &nir_op_infos[alu->op];
      if (alu->op == nir_op_vec2 || alu->op == nir_op_vec3 || alu->op == nir_op_vec4) {
         // result component c is exactly source c's selected component
         for (unsigned c = 0; c < def->num_components && c < 32; c++)
            if (mask & (1u << c))
               require_def(alu->src[c].src.ssa, 1u << alu->src[c].swizzle[0]);
         return;
      }
      bool componentwise = info->output_size == 0;
      for (unsigned i = 0; i < info->num_inputs; i++) {
         if (alu->op == nir_op_bcsel && i == 0)
            continue; // the boolean condition itself is not precise
         if (componentwise && info->input_sizes[i] == 0) {
            uint32_t sm = 0;
            for (unsigned c = 0; c < def->num_components && c < 32; c++)
               if (mask & (1u << c))
                  sm |= 1u << alu->src[i].swizzle[c];
            require_def(alu->src[i].src.ssa, sm);
         } else {
            // reductions and explicitly sized inputs: every input component
            // can contribute to every output component
            require_def(alu->src[i].src.ssa, ~0u);
         }
      }
      return;
   }
   case nir_instr_type_intrinsic: {
      nir_intrinsic_instr* in = nir_instr_as_intrinsic(instr);
      if (in->intrinsic == nir_intrinsic_load_deref) {
         nir_deref_instr* d = nir_src_as_deref(in->src[0]);
         nir_variable* var = nir_deref_instr_get_variable(d);
         if (!var || !trackable_variable(var))
            return;
         unsigned base;
         if (!base_slot(d, &base)) {
            require_all_slots(var);
            return;
         }
         std::vector<uint64_t> bits = slots_of(var);
         for (auto& w : bits)
            w = 0;
         for (unsigned c = 0; c < def->num_components && c < 32; c++)
            if (mask & (1u << c))
               slot_set(bits, base + c);
         require_slots(var, bits);
         return;
      }
      if (in->intrinsic == nir_intrinsic_load_reg) {
         // values merged through a register (e.g. loop-carried): constrain
         // everything stored to that register
         nir_def* reg = in->src[0].ssa;
         nir_foreach_block(block, impl) {
            nir_foreach_instr(it, block) {
               if (it->type != nir_instr_type_intrinsic)
                  continue;
               nir_intrinsic_instr* st = nir_instr_as_intrinsic(it);
               if (st->intrinsic == nir_intrinsic_store_reg && st->src[1].ssa == reg)
                  require_def(st->src[0].ssa, mask);
            }
         }
         return;
      }
      // textures, uniform/input loads, atomics, ... are chain inputs
      return;
   }
   case nir_instr_type_phi: {
      nir_phi_instr* phi = nir_instr_as_phi(instr);
      nir_foreach_phi_src(src, phi)
         require_def(src->src.ssa, mask);
      return;
   }
   default:
      // constants and undefs are already exact
      return;
   }
}

void
precise_prop::process_var(nir_variable* var)
{
   const std::vector<uint64_t>& slots = var_slots[var];
   for (nir_intrinsic_instr* w : var_writes[var]) {
      if (w->intrinsic == nir_intrinsic_store_deref) {
         nir_deref_instr* d = nir_src_as_deref(w->src[0]);
         unsigned base;
         if (!base_slot(d, &base)) {
            require_def(w->src[1].ssa, ~0u);
            continue;
         }
         unsigned wrmask = nir_intrinsic_write_mask(w);
         unsigned ncomps = glsl_type_is_vector_or_scalar(d->type) ? glsl_get_vector_elements(d->type) : 1;
         uint32_t valuemask = 0;
         unsigned rank = 0;
         for (unsigned c = 0; c < ncomps; c++) {
            if (!(wrmask & (1u << c)))
               continue;
            if (slot_get(slots, base + c))
               valuemask |= 1u << rank;
            rank++;
         }
         if (valuemask)
            require_def(w->src[1].ssa, valuemask);
      } else if (w->intrinsic == nir_intrinsic_copy_deref) {
         nir_deref_instr* dst = nir_src_as_deref(w->src[0]);
         nir_deref_instr* src = nir_src_as_deref(w->src[1]);
         nir_variable* svar = nir_deref_instr_get_variable(src);
         if (!svar || !trackable_variable(svar))
            continue;
         unsigned db, sb;
         if (!base_slot(dst, &db) || !base_slot(src, &sb)) {
            require_all_slots(svar);
            continue;
         }
         unsigned cover = type_slots(dst->type);
         std::vector<uint64_t> bits = slots_of(svar);
         for (auto& word : bits)
            word = 0;
         for (unsigned i = 0; i < cover; i++)
            if (slot_get(slots, db + i))
               slot_set(bits, sb + i);
         require_slots(svar, bits);
      }
   }
}

} // namespace

void
glslopt_propagate_precise(nir_shader* nir)
{
   if (!nir->has_precise)
      return;
   nir_function_impl* impl = nir_shader_get_entrypoint(nir);
   if (!impl)
      return;

   precise_prop p;
   p.nir = nir;
   p.impl = impl;

   // index the writes to every trackable variable
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr* in = nir_instr_as_intrinsic(instr);
         if (in->intrinsic != nir_intrinsic_store_deref && in->intrinsic != nir_intrinsic_copy_deref)
            continue;
         nir_variable* var = nir_deref_instr_get_variable(nir_src_as_deref(in->src[0]));
         if (var && trackable_variable(var))
            p.var_writes[var].push_back(in);
      }
   }

   // seed: every component of a variable declared precise must be exact
   nir_foreach_variable_in_shader(var, nir) {
      if (var->data.precise)
         p.require_all_slots(var);
   }
   nir_foreach_function_temp_variable(var, impl) {
      if (var->data.precise)
         p.require_all_slots(var);
   }

   // fixpoint
   while (!p.def_work.empty() || !p.var_work.empty()) {
      while (!p.def_work.empty()) {
         auto item = p.def_work.back();
         p.def_work.pop_back();
         p.process_def(item.first, item.second);
      }
      while (!p.var_work.empty()) {
         nir_variable* var = p.var_work.back();
         p.var_work.pop_back();
         p.process_var(var);
      }
   }
}
