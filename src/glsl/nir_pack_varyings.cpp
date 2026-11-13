#include "nir_pack_varyings.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/glsl_types.h"
#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Qualifier grouping and slot assignment
//
// Two varyings share a four-component slot only when their scalar base type
// and every interpolation/storage qualifier match. An unqualified varying is
// interpreted as smooth (the GLSL default).
// ---------------------------------------------------------------------------

namespace {

struct QualKey {
   int base = GLSL_TYPE_FLOAT;       // GLSL_TYPE_FLOAT / INT / UINT
   unsigned interp = INTERP_MODE_SMOOTH;
   bool centroid = false;
   bool sample = false;
   bool invariant = false;

   bool operator<(const QualKey& o) const
   {
      if (base != o.base) return base < o.base;
      if (interp != o.interp) return interp < o.interp;
      if (centroid != o.centroid) return centroid < o.centroid;
      if (sample != o.sample) return sample < o.sample;
      return invariant < o.invariant;
   }
   bool operator==(const QualKey& o) const { return !(o < *this) && !(*this < o); }
};

struct InterfaceVar {
   std::string name;
   nir_variable* fs = nullptr; // fragment shader input
   nir_variable* vs = nullptr; // matching vertex shader output
   unsigned components = 0;
   QualKey key;
};

struct Placement {
   const InterfaceVar* var;
   unsigned offset; // first occupied component inside the slot
};

struct Slot {
   QualKey key;
   bool used[4] = {false, false, false, false};
   std::vector<Placement> places;
   std::string packed_name;
};

unsigned
effective_interp(const nir_variable* v)
{
   return v->data.interpolation == INTERP_MODE_NONE ? INTERP_MODE_SMOOTH : v->data.interpolation;
}

QualKey
qual_key(const nir_variable* v)
{
   QualKey k;
   k.base = glsl_get_base_type(v->type);
   k.interp = effective_interp(v);
   k.centroid = v->data.centroid;
   k.sample = v->data.sample;
   k.invariant = v->data.explicit_invariant;
   return k;
}

// Reject every interface shape the first version does not know how to pack.
// Vertex inputs, fragment outputs, uniforms and builtins are inspected by the
// caller and never reach this function.
bool
check_supported(nir_variable* v, std::string* err)
{
   const char* nm = v->name ? v->name : "<anonymous>";
   if (v->interface_type) {
      *err = std::string("interface block varying '") + nm + "' is not supported by pair optimization";
      return false;
   }
   if (v->data.explicit_location) {
      *err = std::string("explicit location on varying '") + nm + "' is not supported by pair optimization";
      return false;
   }
   if (glsl_type_is_array(v->type)) {
      *err = std::string("array varying '") + nm + "' is not supported by pair optimization";
      return false;
   }
   if (glsl_type_is_matrix(v->type)) {
      *err = std::string("matrix varying '") + nm + "' is not supported by pair optimization";
      return false;
   }
   if (v->data.compact) {
      *err = std::string("compact varying '") + nm + "' is not supported by pair optimization";
      return false;
   }
   const glsl_base_type bt = glsl_get_base_type(v->type);
   if (bt != GLSL_TYPE_FLOAT && bt != GLSL_TYPE_INT && bt != GLSL_TYPE_UINT) {
      *err = std::string("varying '") + nm + "' has a type not supported by pair optimization";
      return false;
   }
   if (!glsl_type_is_scalar(v->type) && !glsl_type_is_vector(v->type)) {
      *err = std::string("varying '") + nm + "' has a type not supported by pair optimization";
      return false;
   }
   if (glsl_get_vector_elements(v->type) > 4) {
      *err = std::string("varying '") + nm + "' has a type not supported by pair optimization";
      return false;
   }
   if ((bt == GLSL_TYPE_INT || bt == GLSL_TYPE_UINT) && effective_interp(v) != INTERP_MODE_FLAT) {
      *err = std::string("integer varying '") + nm + "' must be declared flat";
      return false;
   }
   return true;
}

bool
is_user_io(nir_variable* v, nir_variable_mode mode)
{
   return v->data.mode == mode && v->name && strncmp(v->name, "gl_", 3) != 0;
}

// Every deref of an interface variable has to be a plain variable deref used
// only by load/store_deref: interpolation intrinsics, copies or aggregate
// accesses cannot be remapped component-by-component.
bool
check_deref_usage(nir_shader* nir, const std::set<nir_variable*>& vars, std::string* err)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_deref)
               continue;
            nir_deref_instr* d = nir_instr_as_deref(instr);
            nir_variable* v = nir_deref_instr_get_variable(d);
            if (!v || !vars.count(v))
               continue;
            if (d->deref_type != nir_deref_type_var) {
               *err = std::string("unsupported access to varying '") + v->name + "'";
               return false;
            }
            nir_foreach_use(use, &d->def) {
               nir_instr* user = nir_src_use_instr(use);
               if (user->type != nir_instr_type_intrinsic) {
                  *err = std::string("unsupported access to varying '") + v->name + "'";
                  return false;
               }
               nir_intrinsic_op op = nir_instr_as_intrinsic(user)->intrinsic;
               if (op != nir_intrinsic_load_deref && op != nir_intrinsic_store_deref) {
                  *err = std::string("unsupported access to varying '") + v->name +
                         "' (only plain reads and writes can be repacked)";
                  return false;
               }
            }
         }
      }
   }
   return true;
}

// Lowest run of `count` free components in a slot, or -1 if it does not fit.
int
free_span(const Slot& slot, unsigned count)
{
   for (unsigned start = 0; start + count <= 4; ++start) {
      bool fit = true;
      for (unsigned i = 0; i < count; ++i)
         if (slot.used[start + i]) { fit = false; break; }
      if (fit)
         return (int)start;
   }
   return -1;
}

// ---------------------------------------------------------------------------
// NIR rewriting
// ---------------------------------------------------------------------------

struct Remap {
   nir_variable* packed;
   unsigned offset;
   unsigned components;
};

const glsl_type*
packed_type(const QualKey& key)
{
   if (key.base == GLSL_TYPE_INT) return glsl_ivec_type(4);
   if (key.base == GLSL_TYPE_UINT) return glsl_uvec_type(4);
   return glsl_vec_type(4);
}

void
apply_qualifiers(nir_variable* v, const QualKey& key)
{
   v->data.interpolation = key.interp;
   v->data.centroid = key.centroid;
   v->data.sample = key.sample;
   v->data.explicit_invariant = key.invariant;
   v->data.invariant = key.invariant;
}

// Replace loads/stores of an old varying with swizzled accesses into the
// packed vec4. Stores keep their write mask, shifted to the packed offset, so
// partial and branch-local assignments keep their exact semantics.
void
rewrite_stage(nir_shader* nir, const std::map<nir_variable*, Remap>& remap,
              const std::set<nir_variable*>& dead)
{
   nir_function_impl* impl = nir_shader_get_entrypoint(nir);
   nir_builder b = nir_builder_create(impl);

   // A dead output that the vertex shader itself reads cannot simply vanish:
   // demote it to a function-local so its stores still feed the reads, while it
   // leaves the stage interface. Outputs with no reads lose their stores.
   std::set<nir_variable*> demoted;
   for (nir_variable* v : dead) {
      bool loaded = false;
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            nir_intrinsic_instr* in = nir_instr_as_intrinsic(instr);
            if (in->intrinsic != nir_intrinsic_load_deref)
               continue;
            if (nir_deref_instr_get_variable(nir_src_as_deref(in->src[0])) == v) {
               loaded = true;
               break;
            }
         }
         if (loaded) break;
      }
      if (loaded) {
         v->data.mode = nir_var_function_temp;
         exec_node_remove(&v->node);
         exec_list_push_tail(&impl->locals, &v->node);
         demoted.insert(v);
      }
   }

   nir_foreach_block_safe(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr* in = nir_instr_as_intrinsic(instr);
         if (in->intrinsic != nir_intrinsic_load_deref && in->intrinsic != nir_intrinsic_store_deref)
            continue;
         nir_deref_instr* d = nir_src_as_deref(in->src[0]);
         nir_variable* v = nir_deref_instr_get_variable(d);
         if (!v)
            continue;
         if (dead.count(v)) {
            if (demoted.count(v))
               continue; // now a local; keep its loads and stores intact
            // Truly unused output: drop only the store. The value computation
            // (and any side effects it carries) stays untouched.
            if (in->intrinsic == nir_intrinsic_store_deref)
               nir_instr_remove(instr);
            continue;
         }
         auto it = remap.find(v);
         if (it == remap.end())
            continue;
         const Remap& rm = it->second;

         b.cursor = nir_before_instr(instr);
         nir_deref_instr* pderef = nir_build_deref_var(&b, rm.packed);

         if (in->intrinsic == nir_intrinsic_load_deref) {
            nir_def* loaded = nir_load_deref(&b, pderef);
            unsigned swz[4];
            for (unsigned i = 0; i < rm.components; ++i)
               swz[i] = rm.offset + i;
            nir_def* sub = nir_swizzle(&b, loaded, swz, rm.components);
            nir_def_rewrite_uses(&in->def, sub);
            nir_instr_remove(instr);
         } else {
            nir_def* value = in->src[1].ssa;
            unsigned mask = nir_intrinsic_write_mask(in);
            const unsigned bitsize = value->bit_size;
            // Position the value at its packed channels; the write mask is
            // shifted by the same offset. Other channels are undef (masked off).
            nir_def* comps[4];
            for (unsigned i = 0; i < 4; ++i) {
               unsigned local = i - rm.offset;
               if (i >= rm.offset && local < rm.components && (mask & (1u << local)))
                  comps[i] = nir_channel(&b, value, local);
               else
                  comps[i] = nir_undef(&b, 1, bitsize);
            }
            nir_def* vec = nir_vec(&b, comps, 4);
            nir_store_deref(&b, pderef, vec, mask << rm.offset);
            nir_instr_remove(instr);
         }
      }
   }

   // Detach the old interface variables and their now-unused deref chains.
   // Demoted outputs already live in the function's locals, not this list.
   std::set<nir_variable*> removed;
   for (nir_variable* v : dead)
      if (!demoted.count(v)) removed.insert(v);
   for (auto& kv : remap)
      removed.insert(kv.first);
   nir_foreach_variable_in_shader_safe(var, nir) {
      if (removed.count(var)) {
         var->data.mode = (nir_variable_mode)0;
         exec_node_remove(&var->node);
      }
   }
   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type == nir_instr_type_deref)
            nir_deref_instr_remove_if_unused(nir_instr_as_deref(instr));
      }
   }
}

} // namespace

const char*
glslopt_pack_varying_interface(nir_shader* vertex, nir_shader* fragment,
                                const char* vertexSource, const char* fragmentSource)
{
   static thread_local std::string error;
   error.clear();
   std::string err;

   // ---- collect the user interface on both sides -------------------------
   std::map<std::string, nir_variable*> fs_inputs, vs_outputs;
   nir_foreach_variable_in_shader(v, fragment) {
      if (is_user_io(v, nir_var_shader_in))
         fs_inputs[v->name] = v;
   }
   nir_foreach_variable_in_shader(v, vertex) {
      if (is_user_io(v, nir_var_shader_out))
         vs_outputs[v->name] = v;
   }

   for (auto& kv : fs_inputs)
      if (!check_supported(kv.second, &err)) { error = err; return error.c_str(); }
   for (auto& kv : vs_outputs)
      if (!check_supported(kv.second, &err)) { error = err; return error.c_str(); }

   // ---- match by name, then type and qualifiers ---------------------------
   std::vector<InterfaceVar> matched;
   std::set<nir_variable*> vs_dead;
   for (auto& kv : vs_outputs)
      if (!fs_inputs.count(kv.first))
         vs_dead.insert(kv.second);

   for (auto& kv : fs_inputs) {
      const std::string& nm = kv.first;
      nir_variable* fv = kv.second;
      auto oit = vs_outputs.find(nm);
      if (oit == vs_outputs.end()) {
         error = "fragment input '" + nm + "' has no matching vertex output";
         return error.c_str();
      }
      nir_variable* vv = oit->second;
      if (glsl_get_base_type(fv->type) != glsl_get_base_type(vv->type) ||
          glsl_get_vector_elements(fv->type) != glsl_get_vector_elements(vv->type)) {
         error = "vertex output and fragment input named '" + nm + "' have mismatched types";
         return error.c_str();
      }
      QualKey fk = qual_key(fv), vk = qual_key(vv);
      if (!(fk == vk)) {
         error = "vertex output and fragment input named '" + nm +
                 "' have mismatched interpolation or storage qualifiers";
         return error.c_str();
      }
      InterfaceVar iv;
      iv.name = nm;
      iv.fs = fv;
      iv.vs = vv;
      iv.components = glsl_get_vector_elements(fv->type);
      iv.key = fk;
      matched.push_back(iv);
   }

   std::set<nir_variable*> all_vars = vs_dead;
   for (const InterfaceVar& iv : matched) {
      all_vars.insert(iv.fs);
      all_vars.insert(iv.vs);
   }
   if (!check_deref_usage(fragment, all_vars, &err)) { error = err; return error.c_str(); }
   if (!check_deref_usage(vertex, all_vars, &err)) { error = err; return error.c_str(); }

   // ---- group and bin-pack ------------------------------------------------
   std::map<QualKey, std::vector<InterfaceVar*>> groups;
   for (InterfaceVar& iv : matched)
      groups[iv.key].push_back(&iv);

   std::vector<Slot> slots;
   for (auto& gk : groups) {
      std::vector<InterfaceVar*>& members = gk.second;
      std::sort(members.begin(), members.end(), [](InterfaceVar* a, InterfaceVar* b) {
         if (a->components != b->components) return a->components > b->components;
         return a->name < b->name;
      });
      for (InterfaceVar* iv : members) {
         Slot* chosen = nullptr;
         for (Slot& slot : slots) {
            if (!(slot.key == iv->key))
               continue;
            if (free_span(slot, iv->components) >= 0) {
               chosen = &slot;
               break;
            }
         }
         if (!chosen) {
            Slot slot;
            slot.key = iv->key;
            slots.push_back(slot);
            chosen = &slots.back();
         }
         int start = free_span(*chosen, iv->components);
         for (unsigned i = 0; i < iv->components; ++i)
            chosen->used[start + i] = true;
         chosen->places.push_back({iv, (unsigned)start});
      }
   }

// ---- internal names that avoid every identifier in both stages --------
std::set<std::string> taken;
auto collect_names = [&](nir_shader* nir) {
   nir_foreach_variable_in_shader(v, nir)
      if (v->name) taken.insert(v->name);
   nir_foreach_function_temp_variable(v, nir_shader_get_entrypoint(nir))
      if (v->name) taken.insert(v->name);
};
collect_names(vertex);
collect_names(fragment);
// Struct/block type names and other identifiers that never become nir
// variables still live in the user source; generated globals must not collide.
auto collect_source_ids = [&](const char* src) {
   if (!src) return;
   for (const char* p = src; *p;) {
      if (!(isalpha((unsigned char)*p) || *p == '_')) { ++p; continue; }
      const char* q = p;
      while (isalnum((unsigned char)*q) || *q == '_') ++q;
      taken.insert(std::string(p, q));
      p = q;
   }
};
collect_source_ids(vertexSource);
collect_source_ids(fragmentSource);
   for (Slot& slot : slots) {
      for (unsigned n = 0;; ++n) {
         std::string candidate = "glslopt_pv" + std::to_string(n);
         if (!taken.count(candidate)) {
            taken.insert(candidate);
            slot.packed_name = candidate;
            break;
         }
      }
   }

   // ---- create the packed variables and rewrite both stages ---------------
   std::map<nir_variable*, Remap> fs_remap, vs_remap;
   for (const Slot& slot : slots) {
      nir_variable* fpacked = nir_variable_create(fragment, nir_var_shader_in, packed_type(slot.key),
                                                   ralloc_strdup(fragment, slot.packed_name.c_str()));
      apply_qualifiers(fpacked, slot.key);
      nir_variable* vpacked = nir_variable_create(vertex, nir_var_shader_out, packed_type(slot.key),
                                                  ralloc_strdup(vertex, slot.packed_name.c_str()));
      apply_qualifiers(vpacked, slot.key);
      for (const Placement& p : slot.places) {
         fs_remap[p.var->fs] = {fpacked, p.offset, p.var->components};
         vs_remap[p.var->vs] = {vpacked, p.offset, p.var->components};
      }
   }

   rewrite_stage(fragment, fs_remap, {});
   rewrite_stage(vertex, vs_remap, vs_dead);

   NIR_PASS(_, fragment, nir_opt_dce);
   NIR_PASS(_, vertex, nir_opt_dce);

   return nullptr;
}
