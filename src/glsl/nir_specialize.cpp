#include "nir_specialize.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/glsl_types.h"
#include <cmath>
#include <cstring>
#include <map>

// ---------------------------------------------------------------------------
// diagnostics

static std::string
render_path(const glslopt_specialized_uniform& b)
{
   std::string s = b.uniform ? b.uniform : "(null)";
   for (int i = 0; i < b.segmentCount; i++) {
      const glslopt_path_segment& seg = b.segments[i];
      if (seg.kind == kGlslPathArrayIndex)
         s += "[" + std::to_string(seg.arrayIndex) + "]";
      else
         s += std::string(".") + (seg.field ? seg.field : "(null)");
   }
   return s;
}

// ---------------------------------------------------------------------------
// binding validation against the declared types (pre-optimization)

static bool
scalar_vector_kind(const glsl_type* t, glsl_base_type* bt, unsigned* comps)
{
   if (!glsl_type_is_scalar(t) && !glsl_type_is_vector(t))
      return false;
   *bt = glsl_get_base_type(t);
   *comps = glsl_get_vector_elements(t);
   return true;
}

static bool
supported_leaf_base(glsl_base_type bt)
{
   return bt == GLSL_TYPE_FLOAT || bt == GLSL_TYPE_INT ||
          bt == GLSL_TYPE_UINT || bt == GLSL_TYPE_BOOL;
}

bool
glslopt_spec_build_leaves(nir_shader* nir,
                          const glslopt_specialized_uniform* bindings,
                          int bindingCount,
                          std::vector<glslopt_spec_leaf>& leaves,
                          std::string& error)
{
   if (bindingCount < 0 || (bindingCount > 0 && !bindings)) {
      error = "specialization: invalid binding table";
      return false;
   }

   // Detect duplicate targets by the exact variable and field/index path.
   std::map<std::pair<nir_variable*, std::vector<unsigned>>, std::string> seen;

   for (int bi = 0; bi < bindingCount; bi++) {
      const glslopt_specialized_uniform& b = bindings[bi];
      std::string where = b.uniform ? render_path(b) : std::string("binding #") + std::to_string(bi);
      auto fail = [&](const std::string& msg) {
         error = "specialization: " + where + ": " + msg;
         return false;
      };

      if (!b.uniform || !*b.uniform)
         return fail("missing uniform name");
      if (!b.value)
         return fail("missing value");
      if (b.segmentCount < 0 || (b.segmentCount > 0 && !b.segments))
         return fail("invalid path segments");

      nir_variable* var = nullptr;
      nir_foreach_variable_in_shader(v, nir) {
         if (v->name && strcmp(v->name, b.uniform) == 0) {
            var = v;
            break;
         }
      }
      if (!var)
         return fail("unknown uniform");
      if (var->data.mode != nir_var_uniform || var->interface_type)
         return fail("only default (non-block) uniforms can be specialized");

      const glsl_type* cur = var->type;
      glslopt_spec_leaf leaf;
      leaf.var = var;

      for (int si = 0; si < b.segmentCount; si++) {
         const glslopt_path_segment& seg = b.segments[si];
         if (seg.kind == kGlslPathStructField) {
            if (!glsl_type_is_struct(cur) && !glsl_type_is_interface(cur))
               return fail("path reaches a field of a non-struct type");
            if (!seg.field)
               return fail("missing struct field name");
            int field = glsl_get_field_index(cur, seg.field);
            if (field < 0)
               return fail(std::string("no struct field '") + seg.field + "'");
            leaf.path.push_back({true, (unsigned)field});
            cur = glsl_get_struct_field(cur, field);
         } else if (seg.kind == kGlslPathArrayIndex) {
            if (!glsl_type_is_array(cur))
               return fail("path indexes a non-array type");
            if (glsl_type_is_unsized_array(cur))
               return fail("cannot index an unsized array");
            int len = (int)glsl_get_length(cur);
            if (seg.arrayIndex < 0 || seg.arrayIndex >= len)
               return fail("array index out of range");
            leaf.path.push_back({false, (unsigned)seg.arrayIndex});
            cur = glsl_get_array_element(cur);
         } else {
            return fail("invalid path segment");
         }
      }

      glsl_base_type bt;
      unsigned comps;
      if (!scalar_vector_kind(cur, &bt, &comps))
         return fail("path must end at a complete scalar or vector (matrices, structs and arrays are not leaves)");
      if (!supported_leaf_base(bt))
         return fail("samplers, images and opaque types cannot be specialized");

      const glslopt_uniform_value& val = *b.value;
      if (val.components != (int)comps)
         return fail("component count does not match the declared type");
      if (val.components <= 0 || val.components > 4)
         return fail("unsupported component count");

      bool typeOk = false;
      switch (val.type) {
      case kGlslValueTypeFloat32:
         typeOk = bt == GLSL_TYPE_FLOAT;
         break;
      case kGlslValueTypeInt32:
         typeOk = bt == GLSL_TYPE_INT || bt == GLSL_TYPE_UINT;
         break;
      case kGlslValueTypeBool:
         typeOk = bt == GLSL_TYPE_BOOL;
         break;
      default:
         return fail("unknown value type");
      }
      if (!typeOk)
         return fail("value type does not match the declared uniform type");

      if (val.type == kGlslValueTypeFloat32 && !val.floatValues)
         return fail("missing float values");
      if (val.type != kGlslValueTypeFloat32 && !val.intValues)
         return fail("missing integer values");

      leaf.type = cur;
      leaf.components = comps;
      leaf.bit_size = 32;
      for (int c = 0; c < val.components; c++) {
         nir_const_value cv = {};
         switch (val.type) {
         case kGlslValueTypeFloat32:
            if (!std::isfinite(val.floatValues[c]))
               return fail("float values must be finite");
            cv.f32 = val.floatValues[c];
            leaf.bit_size = 32;
            break;
         case kGlslValueTypeInt32:
            cv.i32 = val.intValues[c];
            leaf.bit_size = 32;
            break;
         case kGlslValueTypeBool:
            if (val.intValues[c] != 0 && val.intValues[c] != 1)
               return fail("bool values must be 0 or 1");
            cv.b = val.intValues[c] != 0;
            leaf.bit_size = 1;
            break;
         }
         leaf.values[c] = cv;
      }

      std::vector<unsigned> key;
      key.reserve(leaf.path.size() * 2);
      for (auto& st : leaf.path) {
         key.push_back(st.is_struct ? 1u : 0u);
         key.push_back(st.index);
      }
      if (!seen.emplace(std::make_pair(var, std::move(key)), where).second)
         return fail("duplicate specialization target");
      leaves.push_back(std::move(leaf));
   }

   return true;
}

// ---------------------------------------------------------------------------
// NIR load rewriting

namespace {

struct chain {
   nir_variable* var;
   std::vector<nir_deref_instr*> derefs; // root (var deref) first, leaf last
};

bool
deref_to_chain(nir_deref_instr* leaf, chain* out)
{
   std::vector<nir_deref_instr*> rev;
   for (nir_deref_instr* d = leaf; d; d = nir_deref_instr_parent(d)) {
      if (d->deref_type != nir_deref_type_var &&
          d->deref_type != nir_deref_type_struct &&
          d->deref_type != nir_deref_type_array)
         return false; // casts, wildcards, ptr-as-array: leave alone
      rev.push_back(d);
   }
   if (rev.empty() || rev.back()->deref_type != nir_deref_type_var)
      return false;
   out->var = rev.back()->var;
   out->derefs.assign(rev.rbegin(), rev.rend());
   return true;
}

// Does this chain read exactly the leaf described by the binding path?
bool
chain_matches(const chain& c, const glslopt_spec_leaf& leaf)
{
   if (c.var != leaf.var)
      return false;
   if (c.derefs.size() != leaf.path.size() + 1)
      return false;
   for (size_t i = 0; i < leaf.path.size(); i++) {
      nir_deref_instr* d = c.derefs[i + 1];
      const glslopt_spec_leaf::step& st = leaf.path[i];
      if (st.is_struct) {
         if (d->deref_type != nir_deref_type_struct || d->strct.index != st.index)
            return false;
      } else {
         if (d->deref_type != nir_deref_type_array)
            return false;
         // A constant index that names a different element is not a read of
         // this leaf; dynamic indices are resolved at runtime by the bcsel.
         if (nir_src_is_const(d->arr.index) &&
             nir_src_as_int(d->arr.index) != (int64_t)st.index)
            return false;
      }
   }
   return true;
}

nir_def*
build_constant(nir_builder* b, const glslopt_spec_leaf& leaf)
{
   return nir_build_imm(b, leaf.components, leaf.bit_size, leaf.values);
}

// Rebuild the original deref chain from a fresh variable deref, reusing the
// original runtime index expressions. This is the runtime fallback load and
// keeps every dynamic index (and the side effects that compute it) exactly
// where the source program placed it.
nir_deref_instr*
clone_chain(nir_builder* b, const chain& c)
{
   nir_deref_instr* d = c.derefs[0]; // root variable deref
   nir_deref_instr* built = nir_build_deref_var(b, d->var);
   for (size_t i = 1; i < c.derefs.size(); i++) {
      nir_deref_instr* orig = c.derefs[i];
      if (orig->deref_type == nir_deref_type_struct) {
         built = nir_build_deref_struct(b, built, orig->strct.index);
      } else if (nir_src_is_const(orig->arr.index)) {
         built = nir_build_deref_array_imm(b, built, nir_src_as_int(orig->arr.index));
      } else {
         built = nir_build_deref_array(b, built, orig->arr.index.ssa);
      }
   }
   return built;
}

// Condition under which this chain reads the given leaf: every dynamic array
// step must equal that leaf's fixed index. Constant steps already matched in
// chain_matches(), and struct fields never need a runtime test. Returns NULL
// when the whole chain is constant, meaning the leaf applies unconditionally.
nir_def*
match_condition(nir_builder* b, const chain& c, const glslopt_spec_leaf& leaf)
{
   nir_def* cond = nullptr;
   for (size_t i = 0; i < leaf.path.size(); i++) {
      if (leaf.path[i].is_struct)
         continue;
      nir_deref_instr* d = c.derefs[i + 1];
      if (nir_src_is_const(d->arr.index))
         continue;
      nir_def* eq = nir_ieq_imm(b, d->arr.index.ssa, leaf.path[i].index);
      cond = cond ? nir_iand(b, cond, eq) : eq;
   }
   return cond;
}

} // namespace

void
glslopt_spec_rewrite_loads(nir_shader* nir, const std::vector<glslopt_spec_leaf>& leaves)
{
   if (leaves.empty())
      return;

   nir_function_impl* impl = nir_shader_get_entrypoint(nir);
   std::vector<nir_intrinsic_instr*> loads;
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr* in = nir_instr_as_intrinsic(instr);
         if (in->intrinsic == nir_intrinsic_load_deref)
            loads.push_back(in);
      }
   }

   for (nir_intrinsic_instr* load : loads) {
      nir_deref_instr* leaf_deref = nir_src_as_deref(load->src[0]);
      chain c;
      if (!deref_to_chain(leaf_deref, &c))
         continue;

      // Every leaf that names this exact read. A constant-index read matches
      // at most one leaf; a dynamic read can match several elements.
      std::vector<const glslopt_spec_leaf*> matched;
      for (const auto& l : leaves)
         if (chain_matches(c, l))
            matched.push_back(&l);
      if (matched.empty())
         continue;

      nir_builder b = nir_builder_at(nir_before_instr(&load->instr));

      // Start from the original runtime value, then layer the known elements
      // on as guarded selections. Earlier bindings win on overlapping guards;
      // distinct element indices make those guards mutually exclusive.
      nir_def* replacement = nir_load_deref(&b, clone_chain(&b, c));
      for (const glslopt_spec_leaf* leaf : matched) {
         nir_def* constant = build_constant(&b, *leaf);
         nir_def* cond = match_condition(&b, c, *leaf);
         if (!cond)
            replacement = constant; // fully constant read
         else
            replacement = nir_bcsel(&b, cond, constant, replacement);
      }

      // The result has the leaf's scalar/vector type and width.
      assert(replacement->num_components == load->def.num_components);
      assert(replacement->bit_size == load->def.bit_size);
      nir_def_rewrite_uses(&load->def, replacement);
   }

   nir_progress(true, impl, nir_metadata_none);
}
