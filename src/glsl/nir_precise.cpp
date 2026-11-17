// GLSL precise-qualifier propagation.
//
// The frontend keeps the source `precise` qualifier on nir variables, but the
// operations that actually contribute to a precise value are spread across the
// SSA value stream: temporaries, local arrays (constant and dynamic indices),
// inlined helper in/out/inout parameters, branch joins and loop carried values.
// This pass runs before arithmetic-changing optimizations and:
//
//   1. marks the float32 ALU instructions on those dependency chains with the
//      exact fp_math_ctrl flags, so NIR's algebraic/CSE/peephole passes keep
//      the source's rounding steps (no reassociation, contraction, or NaN/Inf
//      dropping transforms);
//   2. wraps values loaded from precise storage in the glslopt_precise
//      identity intrinsic (BASE = protected component mask), which the
//      surviving passes treat as an opaque move and the GLSL printer turns
//      back into a legal `precise` declaration boundary.
//
// Tracking is per component, so independent vector components and unrelated
// outputs keep their normal optimizations. Boolean conditions, integer
// operations, textures, atomics and explicit fma builtins are not extended.
#include "nir_optimize.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_deref.h"
#include "compiler/glsl_types.h"
#include <map>
#include <set>
#include <vector>

// Exact semantics required by GLSL precise: no contraction/reassociation and
// no transforms that assume NaN/Inf behavior.
static const unsigned kPreciseCtrl =
   nir_fp_exact | nir_fp_preserve_nan | nir_fp_preserve_inf;

namespace {

// Precise components of one storage location. Constant array indices are
// tracked per element; a dynamic index or an aggregate operation makes the
// whole variable precise.
struct storage_mask {
   nir_component_mask_t whole = 0;
   std::map<std::vector<unsigned>, nir_component_mask_t> elems;
};

class precise_propagation {
public:
   precise_propagation(nir_shader* shader) : s(shader) {}

   bool run()
   {
      nir_function_impl* entry = nir_shader_get_entrypoint(s);
      if (!entry)
         return false;
      nir_index_ssa_defs(entry);
      need.assign(entry->ssa_alloc, 0);
      bool ever_changed = false;
      // Phase A, fixpoint: compute precise components per SSA def, set exact
      // fp flags on the float ALU and flow masks through storage. Each
      // iteration can seed new storage (a precise store marks its producing
      // value, whose loads mark further storage), and loop back-edges need
      // store/load information to flow around. All updates are monotonic masks
      // over finite storage and SSA defs, so this terminates.
      for (unsigned iter = 0; iter < 64; iter++) {
         if (!propagate(entry))
            break;
         ever_changed = true;
         // The number of fixpoint rounds is bounded by the longest store/load
         // dependency chain, far below the cap for realistic shaders; stop
         // cleanly rather than spin if a pathological program reaches it.
         if (iter == 63)
            break;
      }
      // Phase B: put legal precise boundaries on the exact float rounding
      // operations. Loads, moves, vector constructors and phis do no rounding,
      // so they get no boundary; precise output/storage variables are declared
      // precisely by the printer instead.
      if (ever_changed)
         insert_markers(entry);
      if (ever_changed)
         nir_progress(true, entry, nir_metadata_control_flow);
      return ever_changed;
   }

   // wrap every protected float rounding operation's result in a
   // glslopt_precise marker. Loads, moves, vector constructors and phis do no
   // rounding, so they get no boundary: the constraints reach the next real
   // operation through the analysis itself, and precise output variables are
   // declared precisely by the printer.
   void
   insert_markers(nir_function_impl* impl)
   {
      nir_builder b = nir_builder_create(impl);
      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_alu)
               continue;
            nir_alu_instr* alu = nir_instr_as_alu(instr);
            nir_def* def = &alu->def;
            if (def->bit_size != 32 || def->index >= need.size())
               continue;
            nir_component_mask_t mask = need[def->index] & full_mask(def);
            if (!mask)
               continue;
            if (nir_op_infos[alu->op].output_type == nir_type_float &&
                nir_op_infos[alu->op].valid_fp_math_ctrl &&
                !is_explicit_fma(alu->op))
               wrap_def(&b, def, mask);
         }
      }
   }

private:
   nir_shader* s;
   bool grew = false; // any precise set grew during the current iteration
   // components (bit i = component i) of each SSA def that are precise
   std::vector<nir_component_mask_t> need;
   std::map<nir_variable*, storage_mask> storage;
   // defs already wrapped in a glslopt_precise marker
   std::set<nir_instr*> marked_defs;

   static bool
   float_storage(const nir_deref_instr* d)
   {
      const glsl_type* bare = glsl_without_array(d->type);
      return glsl_type_is_vector_or_scalar(bare) &&
             glsl_get_base_type(bare) == GLSL_TYPE_FLOAT;
   }

   static nir_component_mask_t
   full_mask(const nir_def* d)
   {
      return nir_component_mask(d->num_components);
   }

   static nir_component_mask_t
   full_mask_type(const glsl_type* t)
   {
      return nir_component_mask(glsl_get_vector_elements(glsl_without_array(t)));
   }

   // constant array indices of a deref chain; false if any index is dynamic
   static bool
   deref_indices(nir_deref_instr* d, std::vector<unsigned>* indices)
   {
      for (nir_deref_instr* p = d; p && p->deref_type != nir_deref_type_var;
           p = nir_deref_instr_parent(p)) {
         if (p->deref_type == nir_deref_type_struct)
            continue;
         if (p->deref_type != nir_deref_type_array || !nir_src_is_const(p->arr.index))
            return false;
         indices->insert(indices->begin(), nir_src_as_uint(p->arr.index));
      }
      return true;
   }

   static nir_component_mask_t
   lookup_storage(storage_mask& sm, const std::vector<unsigned>& indices,
                  nir_component_mask_t available)
   {
      nir_component_mask_t m = sm.whole;
      if (!indices.empty()) {
         auto it = sm.elems.find(indices);
         if (it != sm.elems.end())
            m |= it->second;
      }
      return m & available;
   }

   void
   update_storage(storage_mask& sm, const std::vector<unsigned>& indices, bool direct,
                  nir_component_mask_t mask)
   {
      if (!direct) {
         if ((sm.whole & mask) != mask) { sm.whole |= mask; grew = true; }
         for (auto& kv : sm.elems)
            if ((kv.second & mask) != mask) { kv.second |= mask; grew = true; }
         return;
      }
      if (indices.empty()) {
         if ((sm.whole & mask) != mask) { sm.whole |= mask; grew = true; }
         return;
      }
      nir_component_mask_t& slot = sm.elems[indices];
      if ((slot & mask) != mask) { slot |= mask; grew = true; }
   }

   // wrap the result of a protected float rounding operation in a
   // glslopt_precise marker carrying the protected component mask
   void
   wrap_def(nir_builder* b, nir_def* def, nir_component_mask_t mask)
   {
      nir_instr* parent = nir_def_instr(def);
      if (def->bit_size != 32 || !mask || marked_defs.count(parent))
         return;
      // Snapshot the existing uses first: once the marker is inserted, its own
      // source is also a use of the def and must not be rewritten onto itself.
      std::vector<nir_src*> uses;
      nir_foreach_use_including_if(u, def)
         uses.push_back(u);
      b->cursor = nir_after_instr(parent);
      nir_intrinsic_instr* m = nir_intrinsic_instr_create(s, nir_intrinsic_glslopt_precise);
      m->num_components = def->num_components;
      nir_def_init(&m->instr, &m->def, def->num_components, def->bit_size);
      m->src[0] = nir_src_for_ssa(def);
      nir_intrinsic_set_base(m, mask & full_mask(def));
      nir_builder_instr_insert(b, &m->instr);
      for (nir_src* u : uses)
         nir_src_rewrite(u, &m->def);
      marked_defs.insert(parent);
   }

   static bool
   is_explicit_fma(nir_op op)
   {
      return op == nir_op_ffma || op == nir_op_ffma_weak || op == nir_op_fmad ||
             op == nir_op_ffmaz || op == nir_op_fmadz;
   }

   // operations that only move/recombine components without rounding:
   // mov, vec constructors and selects
   static bool
   is_typeless_op(nir_op op)
   {
      return op == nir_op_mov || op == nir_op_vec2 || op == nir_op_vec3 ||
             op == nir_op_vec4 || op == nir_op_vec5 || op == nir_op_vec8 ||
             op == nir_op_vec16 || op == nir_op_bcsel;
   }

   // add precise components to an SSA def, reaching the float ALU that
   // computes them
   void
   mark_def(nir_def* def, nir_component_mask_t mask)
   {
      if (!def || def->bit_size != 32 || !mask)
         return;
      if (def->index >= need.size())
         need.resize(def->index + 1, 0);
      mask &= full_mask(def);
      if (!mask)
         return;
      // Re-traverse the operands on every iteration even if the def was
      // already precise: a fixpoint round can make one of its sources precise
      // later (e.g. an identity mov whose variable becomes precise through a
      // store processed afterwards or through a loop back-edge).
      bool is_new = (need[def->index] & mask) != mask;
      need[def->index] |= mask;
      if (is_new)
         grew = true;

      nir_instr* in = nir_def_instr(def);
      switch (in->type) {
      case nir_instr_type_alu: {
         nir_alu_instr* alu = nir_instr_as_alu(in);
         // Explicit fma builtins are not extended: the chain stops at them.
         if (is_explicit_fma(alu->op))
            return;
         // Typeless identities (mov, vector constructors, selects) carry
         // float values without a float output type; traverse them but do not
         // set fp flags. Typed integer/boolean operations end the chain.
         if (!is_typeless_op(alu->op) &&
             nir_op_infos[alu->op].output_type != nir_type_float)
            return;
         if (nir_op_infos[alu->op].valid_fp_math_ctrl)
            alu->fp_math_ctrl |=
               (kPreciseCtrl & nir_op_infos[alu->op].valid_fp_math_ctrl);
         propagate_alu_srcs(alu, mask);
         return;
      }
      case nir_instr_type_intrinsic: {
         nir_intrinsic_instr* intr = nir_instr_as_intrinsic(in);
         if (intr->intrinsic == nir_intrinsic_glslopt_precise ||
             intr->intrinsic == nir_intrinsic_glslopt_precision) {
            // identity markers: continue through the wrapped value
            mark_def(intr->src[0].ssa, mask);
            return;
         }
         if (intr->intrinsic == nir_intrinsic_load_deref) {
            nir_deref_instr* d = nir_src_as_deref(intr->src[0]);
            if (float_storage(d)) {
               std::vector<unsigned> indices;
               bool direct = deref_indices(d, &indices);
               update_storage(storage[nir_deref_instr_get_variable(d)], indices, direct, mask);
            }
         }
         return;
      }
      case nir_instr_type_phi: {
         nir_phi_instr* phi = nir_instr_as_phi(in);
         nir_foreach_phi_src(ps, phi)
            mark_def(ps->src.ssa, mask);
         return;
      }
      default:
         // textures, atomics and other producers are not extended
         return;
      }
   }

   void
   propagate_alu_srcs(nir_alu_instr* alu, nir_component_mask_t out_mask)
   {
      unsigned ninputs = nir_op_infos[alu->op].num_inputs;
      for (unsigned i = 0; i < ninputs; i++) {
         nir_def* src = alu->src[i].src.ssa;
         if (src->bit_size != 32)
            continue;
         unsigned read = nir_ssa_alu_instr_src_components(alu, i);
         nir_component_mask_t src_mask = 0;
         for (unsigned c = 0; c < alu->def.num_components; c++) {
            if (!(out_mask & (1u << c)))
               continue;
            if (alu->op == nir_op_bcsel && i == 0)
               continue; // the boolean control condition is never precise
            if (c < read)
               src_mask |= 1u << alu->src[i].swizzle[c];
         }
         if (src_mask)
            mark_def(src, src_mask);
      }
   }

   bool
   propagate(nir_function_impl* impl)
   {
      grew = false;
      // `need` and `storage` persist between iterations: every update here is
      // monotonic, and `grew` reports whether any precise set got larger.

      // seed: every precise variable is precise over all its components
      nir_foreach_function_temp_variable(v, impl) {
         if (v->data.precise &&
             glsl_get_base_type(glsl_without_array(v->type)) == GLSL_TYPE_FLOAT)
            storage[v].whole = full_mask_type(v->type);
      }
      nir_foreach_variable_in_shader(v, s) {
         if (v->data.precise &&
             glsl_get_base_type(glsl_without_array(v->type)) == GLSL_TYPE_FLOAT)
            storage[v].whole = full_mask_type(v->type);
      }

      nir_foreach_block(block, impl) {
         nir_foreach_instr_safe(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr* intr = nir_instr_as_intrinsic(instr);
            switch (intr->intrinsic) {
            case nir_intrinsic_store_deref: {
               nir_deref_instr* d = nir_src_as_deref(intr->src[0]);
               nir_variable* v = nir_deref_instr_get_variable(d);
               if (!v || !float_storage(d))
                  break;
               nir_def* val = intr->src[1].ssa;
               if (val->bit_size != 32)
                  break;
               std::vector<unsigned> indices;
               deref_indices(d, &indices);
               nir_component_mask_t wmask = nir_intrinsic_write_mask(intr);
               if (!wmask)
                  wmask = full_mask_type(d->type);
               nir_component_mask_t m = lookup_storage(storage[v], indices, wmask);
               if (m)
                  mark_def(val, m);
               break;
            }
            case nir_intrinsic_load_deref: {
               nir_deref_instr* d = nir_src_as_deref(intr->src[0]);
               nir_variable* v = nir_deref_instr_get_variable(d);
               if (!v || !float_storage(d))
                  break;
               std::vector<unsigned> indices;
               deref_indices(d, &indices);
               nir_component_mask_t m =
                  lookup_storage(storage[v], indices, full_mask(&intr->def));
               if (m)
                  mark_def(&intr->def, m);
               break;
            }
            case nir_intrinsic_copy_deref: {
               nir_deref_instr* dst = nir_src_as_deref(intr->src[0]);
               nir_deref_instr* src = nir_src_as_deref(intr->src[1]);
               nir_variable* dv = nir_deref_instr_get_variable(dst);
               nir_variable* sv = nir_deref_instr_get_variable(src);
               if (dv && sv && float_storage(dst)) {
                  std::vector<unsigned> idx;
                  bool direct = deref_indices(dst, &idx);
                  nir_component_mask_t m =
                     lookup_storage(storage[dv], idx, full_mask_type(dst->type));
                  if (m)
                     update_storage(storage[sv], idx, direct, m);
               }
               break;
            }
            default:
               break;
            }
         }
      }

      // Storage that only became precise through value flow carries the
      // qualifier as well, so surviving variables print precise declarations.
      for (auto& kv : storage) {
         nir_variable* v = kv.first;
         bool any = kv.second.whole != 0;
         for (auto& e : kv.second.elems)
            any |= e.second != 0;
         if (any && !v->data.precise &&
             (v->data.mode == nir_var_function_temp || v->data.mode == nir_var_shader_temp)) {
            v->data.precise = true;
            grew = true;
         }
      }
      return grew;
   }
};

// Drop markers that no longer bound anything useful: their input folded to a
// constant/undef, or markers became nested.
static bool
opt_precise_markers(nir_shader* nir)
{
   bool progress = false;
   nir_function_impl* impl = nir_shader_get_entrypoint(nir);
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr* m = nir_instr_as_intrinsic(instr);
         if (m->intrinsic != nir_intrinsic_glslopt_precise)
            continue;
         nir_instr* si = nir_def_instr(m->src[0].ssa);
         if (si->type == nir_instr_type_load_const || si->type == nir_instr_type_undef) {
            nir_def_rewrite_uses(&m->def, m->src[0].ssa);
            nir_instr_remove(instr);
            progress = true;
         } else if (si->type == nir_instr_type_intrinsic &&
                    nir_instr_as_intrinsic(si)->intrinsic == nir_intrinsic_glslopt_precise) {
            nir_intrinsic_instr* inner = nir_instr_as_intrinsic(si);
            nir_intrinsic_set_base(m, nir_intrinsic_base(m) | nir_intrinsic_base(inner));
            nir_src_rewrite(&m->src[0], inner->src[0].ssa);
            progress = true;
         }
      }
   }
   if (progress)
      nir_progress(true, impl, nir_metadata_control_flow);
   else
      nir_no_progress(impl);
   return progress;
}

} // namespace

void
glslopt_propagate_precise(nir_shader* nir)
{
   precise_propagation p(nir);
   p.run();
}

bool
glslopt_opt_precise_markers(nir_shader* nir)
{
   return opt_precise_markers(nir);
}
