// Partial specialization of ordinary uniforms. See glslopt_specialize.h and
// glslopt_optimize_specialized() in glsl_optimizer.h.
#include "glslopt_specialize.h"

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/glsl_types.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// path parsing and declaration validation
//
// A path is a uniform name followed by any number of structure fields and
// constant, fixed-size array indices: "name", "name.field", "name[2].field".
// Swizzles and matrix/vector indexing are not part of the grammar.

namespace {

struct PathStep {
   bool is_field;       // true: struct field, false: array index
   unsigned index;      // field index or array index
};

struct ResolvedEntry {
   nir_variable* var = nullptr;
   std::vector<PathStep> steps; // ends at the leaf scalar/vector
   glslopt_value_type value_type;
   int components = 0;
   std::vector<float> floats;
   std::vector<int32_t> ints;
};

bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_ident_char(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
void skip_spaces(const std::string& s, size_t& p)
{
   while (p < s.size() && (s[p] == ' ' || s[p] == '\t'))
      p++;
}

// Token form that keeps field names until they are resolved against types.
struct RawStep {
   bool is_field;
   std::string field;
   unsigned index;
};

// Tokenize one path into the root uniform name and its field/array-index steps.
bool tokenize(const std::string& text, std::string* root, std::vector<RawStep>* raw, std::string* error)
{
   size_t p = 0;
   skip_spaces(text, p);
   size_t begin = p;
   while (p < text.size() && is_ident_char(text[p]))
      p++;
   if (p == begin || !is_ident_start(text[begin])) {
      *error = "malformed uniform path '" + text + "'";
      return false;
   }
   *root = text.substr(begin, p - begin);
   while (true) {
      skip_spaces(text, p);
      if (p == text.size())
         break;
      if (text[p] == '.') {
         p++;
         skip_spaces(text, p);
         size_t fb = p;
         while (p < text.size() && is_ident_char(text[p]))
            p++;
         if (p == fb) {
            *error = "malformed uniform path '" + text + "'";
            return false;
         }
         raw->push_back(RawStep{true, text.substr(fb, p - fb), 0});
      } else if (text[p] == '[') {
         p++;
         skip_spaces(text, p);
         bool negative = false;
         if (p < text.size() && text[p] == '-') {
            negative = true;
            p++;
         }
         size_t nb = p;
         uint64_t parsed = 0;
         bool overflow = false;
         while (p < text.size() && text[p] >= '0' && text[p] <= '9') {
            uint64_t digit = (uint64_t)(text[p] - '0');
            if (parsed > (UINT64_MAX - digit) / 10)
               overflow = true; // keep consuming the digits, but mark it
            else
               parsed = parsed * 10 + digit;
            p++;
         }
         if (p == nb) {
            *error = "malformed uniform path '" + text + "'";
            return false;
         }
         skip_spaces(text, p);
         if (p >= text.size() || text[p] != ']') {
            *error = "malformed uniform path '" + text + "'";
            return false;
         }
         p++;
         if (negative) {
            *error = "array index in path '" + text + "' is negative";
            return false;
         }
         // Saturate to a value larger than any legal array length; the later
         // declaration bounds check then reports it as out of range.
         unsigned safe_index = (overflow || parsed > UINT32_MAX) ? UINT32_MAX : (unsigned)parsed;
         raw->push_back(RawStep{false, "", safe_index});
      } else {
         *error = "malformed uniform path '" + text + "'";
         return false;
      }
   }
   return true;
}

const char* base_type_name(glslopt_value_type t)
{
   switch (t) {
   case kGlslValueFloat32: return "float32";
   case kGlslValueInt32: return "int32";
   case kGlslValueBool: return "bool";
   }
   return "?";
}

// Walk the declaration following the tokens and fill the resolved entry.
bool resolve_path(nir_shader* nir, const glslopt_specialized_value& value, const std::string& text,
                  ResolvedEntry* out, std::string* error)
{
   std::string root;
   std::vector<RawStep> raw;
   if (!tokenize(text, &root, &raw, error))
      return false;

   nir_variable* found = nullptr;
   nir_foreach_variable_in_shader(var, nir) {
      if (var->data.mode == nir_var_uniform && var->name && root == var->name) {
         found = var;
         break;
      }
   }
   if (!found) {
      // Give the precise reason when the root is a block instance, or a member
      // of an (anonymous) interface block, rather than a generic "unknown".
      nir_foreach_variable_in_shader(var, nir) {
         bool is_block = var->interface_type &&
                         (var->data.mode == nir_var_mem_ubo || var->data.mode == nir_var_mem_ssbo);
         if (!is_block)
            continue;
         bool names_instance = var->name && root == var->name;
         bool names_member = glsl_get_field_index(var->interface_type, root.c_str()) >= 0;
         if (names_instance || names_member) {
            *error = "specialization path '" + text +
                     "' reaches an interface-block member, which cannot be specialized";
            return false;
         }
      }
      *error = "specialization path '" + text + "' does not name a uniform in the pre-optimization declarations";
      return false;
   }
   out->var = found;

   const glsl_type* bare = glsl_without_array(found->type);
   if (found->interface_type) {
      *error = "specialization path '" + text + "' reaches an interface-block member, which cannot be specialized";
      return false;
   }
   if (glsl_type_is_sampler(bare) || glsl_type_is_image(bare)) {
      *error = "specialization path '" + text + "' targets a sampler or image, which cannot be specialized";
      return false;
   }
   if (glsl_contains_atomic(found->type) || glsl_type_contains_image(found->type)) {
      *error = "specialization path '" + text + "' targets an atomic counter or image resource, which cannot be specialized";
      return false;
   }

   const glsl_type* t = found->type;
   for (const RawStep& r : raw) {
      if (r.is_field) {
         if (!glsl_type_is_struct(t)) {
            *error = "specialization path '" + text + "' applies a struct field to a non-struct type; swizzles are not accepted";
            return false;
         }
         int field = glsl_get_field_index(t, r.field.c_str());
         if (field < 0) {
            *error = "specialization path '" + text + "' names an unknown struct field '" + r.field + "'";
            return false;
         }
         out->steps.push_back(PathStep{true, (unsigned)field});
         t = glsl_get_struct_field(t, field);
      } else {
         if (!glsl_type_is_array(t)) {
            *error = "specialization path '" + text + "' indexes a non-array type; swizzles and matrices are not accepted";
            return false;
         }
         if (glsl_type_is_unsized_array(t)) {
            *error = "specialization path '" + text + "' indexes an unsized array, which cannot be specialized";
            return false;
         }
         unsigned length = glsl_get_length(t);
         if (r.index >= length) {
            *error = "specialization path '" + text + "' has an out-of-range array index (size is " +
                     std::to_string(length) + ")";
            return false;
         }
         out->steps.push_back(PathStep{false, r.index});
         t = glsl_get_array_element(t);
      }
   }

   if (glsl_type_is_matrix(t) || glsl_type_is_array(t) || glsl_type_is_struct(t) ||
       glsl_type_is_interface(t) || !glsl_type_is_vector_or_scalar(t)) {
      *error = "specialization path '" + text + "' must end at a complete scalar or vector";
      return false;
   }
   glsl_base_type bt = glsl_get_base_type(t);
   glslopt_value_type want;
   if (bt == GLSL_TYPE_FLOAT && glsl_get_bit_size(t) == 32)
      want = kGlslValueFloat32;
   else if (bt == GLSL_TYPE_INT)
      want = kGlslValueInt32;
   else if (bt == GLSL_TYPE_BOOL)
      want = kGlslValueBool;
   else {
      *error = "specialization path '" + text + "' has an unsupported element type (only float32, int32 and bool are accepted)";
      return false;
   }
   if (value.type != want) {
      *error = "specialization path '" + text + "' expects a " + base_type_name(want) + " value but got " +
               base_type_name(value.type);
      return false;
   }
   int components = (int)glsl_get_vector_elements(t);
   if (value.components != components) {
      *error = "specialization path '" + text + "' expects " + std::to_string(components) +
               " component(s) but the table provides " + std::to_string(value.components);
      return false;
   }
   if (components < 1 || components > 4 || !value.data) {
      *error = "specialization path '" + text + "' has an invalid value descriptor";
      return false;
   }

   out->value_type = value.type;
   out->components = components;
   if (value.type == kGlslValueFloat32) {
      const float* f = (const float*)value.data;
      for (int i = 0; i < components; i++) {
         if (!std::isfinite(f[i])) {
            *error = "specialization path '" + text + "' contains a non-finite float value";
            return false;
         }
         out->floats.push_back(f[i]);
      }
   } else if (value.type == kGlslValueInt32) {
      const int32_t* i = (const int32_t*)value.data;
      out->ints.assign(i, i + components);
   } else {
      const int32_t* b = (const int32_t*)value.data;
      for (int i = 0; i < components; i++) {
         if (b[i] != 0 && b[i] != 1) {
            *error = "specialization path '" + text + "' contains a bool value other than 0 or 1";
            return false;
         }
         out->ints.push_back(b[i]);
      }
   }
   return true;
}

bool same_target(const ResolvedEntry& a, const ResolvedEntry& b)
{
   if (a.var != b.var || a.steps.size() != b.steps.size())
      return false;
   for (size_t i = 0; i < a.steps.size(); i++)
      if (a.steps[i].is_field != b.steps[i].is_field || a.steps[i].index != b.steps[i].index)
         return false;
   return true;
}

// ---------------------------------------------------------------------------
// NIR rewriting
//
// One concrete deref chain step of a uniform load.
struct ChainStep {
   bool is_struct;
   unsigned field;                 // struct field index
   bool array_const;               // array step: constant vs dynamic index
   unsigned array_index;           // constant array index
   nir_def* array_index_def;       // dynamic array index SSA value
   bool on_vector;                 // array step indexes a vector, not an array
};

struct Chain {
   nir_variable* var;
   std::vector<ChainStep> steps;   // down to the loaded type
};

bool build_chain(nir_deref_instr* leaf, Chain* out)
{
   out->var = nullptr;
   nir_deref_instr* d = leaf;
   std::vector<ChainStep> reverse;
   while (d) {
      if (d->deref_type == nir_deref_type_var) {
         out->var = d->var;
         break;
      }
      if (d->deref_type == nir_deref_type_struct) {
         reverse.push_back(ChainStep{true, d->strct.index, false, 0, nullptr, false});
      } else if (d->deref_type == nir_deref_type_array) {
         nir_deref_instr* parent = nir_deref_instr_parent(d);
         bool on_vector = !glsl_type_is_array(parent->type);
         ChainStep s{false, 0, false, 0, d->arr.index.ssa, on_vector};
         if (nir_src_is_const(d->arr.index)) {
            s.array_const = true;
            s.array_index = (unsigned)nir_src_as_uint(d->arr.index);
         }
         reverse.push_back(s);
      } else {
         return false; // casts / wildcard / ptr-as-array cannot name a uniform path
      }
      d = nir_deref_instr_parent(d);
   }
   if (!out->var)
      return false;
   out->steps.assign(reverse.rbegin(), reverse.rend());
   return true;
}

// True when the entry path matches the chain prefix. Struct steps must agree
// exactly; constant array steps must equal the entry index, dynamic array
// steps match any index. Only vector component steps may follow the entry.
bool entry_matches(const ResolvedEntry& e, const Chain& c, size_t* tail)
{
   if (e.var != c.var || e.steps.size() > c.steps.size())
      return false;
   for (size_t i = 0; i < e.steps.size(); i++) {
      const PathStep& ps = e.steps[i];
      const ChainStep& cs = c.steps[i];
      if (ps.is_field) {
         if (!cs.is_struct || cs.field != ps.index)
            return false;
      } else {
         if (cs.is_struct || cs.on_vector)
            return false;
         if (cs.array_const && cs.array_index != ps.index)
            return false;
      }
   }
   for (size_t i = e.steps.size(); i < c.steps.size(); i++) {
      const ChainStep& cs = c.steps[i];
      if (cs.is_struct || !cs.on_vector)
         return false; // the entry ends at a leaf; only vector components follow
   }
   *tail = e.steps.size();
   return true;
}

nir_def* build_constant(nir_builder* b, const ResolvedEntry& e)
{
   unsigned bit_size = e.value_type == kGlslValueBool ? 1 : 32;
   std::vector<nir_const_value> vals(e.components);
   for (int i = 0; i < e.components; i++) {
      if (e.value_type == kGlslValueFloat32)
         vals[i] = nir_const_value_for_float(e.floats[i], 32);
      else if (e.value_type == kGlslValueInt32)
         vals[i] = nir_const_value_for_int(e.ints[i], 32);
      else
         vals[i] = nir_const_value_for_bool(e.ints[i] != 0, 1);
   }
   return nir_build_imm(b, e.components, bit_size, vals.data());
}

// Reduce a vector value to one component; constant indices become a swizzled
// mov, dynamic indices a bcsel ladder over the components.
nir_def* extract_components(nir_builder* b, nir_def* vec, const std::vector<ChainStep>& tail)
{
   nir_def* cur = vec;
   for (const ChainStep& s : tail) {
      unsigned width = cur->num_components;
      if (s.array_const) {
         cur = nir_channel(b, cur, s.array_index < width ? s.array_index : 0);
         continue;
      }
      nir_def* fallback = nir_channel(b, cur, width - 1);
      for (unsigned k = width - 1; k-- > 0;) {
         nir_def* eq = nir_ieq(b, s.array_index_def, nir_imm_int(b, (int)k));
         fallback = nir_bcsel(b, eq, nir_channel(b, cur, k), fallback);
      }
      cur = fallback;
   }
   return cur;
}

// Condition that is true exactly when every dynamic array index of the chain
// currently equals the entry's constant indices.
nir_def* match_condition(nir_builder* b, const ResolvedEntry& e, const Chain& c)
{
   nir_def* cond = nullptr;
   for (size_t i = 0; i < e.steps.size(); i++) {
      if (e.steps[i].is_field || c.steps[i].array_const)
         continue;
      nir_def* eq = nir_ieq(b, c.steps[i].array_index_def, nir_imm_int(b, (int)e.steps[i].index));
      cond = cond ? nir_build_alu(b, nir_op_iand, cond, eq, nullptr, nullptr) : eq;
   }
   return cond;
}

bool specialize_loads(nir_shader* nir, const std::vector<ResolvedEntry>& entries)
{
   nir_function_impl* impl = nir_shader_get_entrypoint(nir);

   struct Job {
      nir_intrinsic_instr* load;
      std::vector<const ResolvedEntry*> matches;
      Chain chain;
      size_t tail;
   };
   std::vector<Job> jobs;

   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;
         nir_intrinsic_instr* intr = nir_instr_as_intrinsic(instr);
         if (intr->intrinsic != nir_intrinsic_load_deref)
            continue;
         nir_deref_instr* deref = nir_src_as_deref(intr->src[0]);
         nir_variable* var = nir_deref_instr_get_variable(deref);
         if (!var || var->data.mode != nir_var_uniform)
            continue;
         Chain chain;
         if (!build_chain(deref, &chain))
            continue;
         Job job{intr, {}, chain, 0};
         for (const ResolvedEntry& e : entries) {
            size_t tail = 0;
            if (entry_matches(e, chain, &tail)) {
               job.matches.push_back(&e);
               job.tail = tail;
            }
         }
         if (!job.matches.empty())
            jobs.push_back(std::move(job));
      }
   }

   bool changed = false;
   for (Job& job : jobs) {
      // Snapshot the original uses (including if-conditions: a uniform bool may
      // decide a branch directly). Instructions created below must not be
      // rewritten as if they were runtime reads of this uniform.
      std::vector<nir_src*> uses;
      nir_foreach_use_including_if(src, &job.load->def)
         uses.push_back(src);

      nir_builder b = nir_builder_at(nir_after_instr(&job.load->instr));
      nir_def* original = &job.load->def;
      bool exact = job.tail == job.chain.steps.size();
      std::vector<ChainStep> tail_steps(job.chain.steps.begin() + job.tail, job.chain.steps.end());

      // Runtime fallback: the original read, reduced to the loaded component
      // when the entry ends before a vector component.
      nir_def* replacement = exact ? original : extract_components(&b, original, tail_steps);
      for (const ResolvedEntry* e : job.matches) {
         nir_def* constant = build_constant(&b, *e);
         if (!exact)
            constant = extract_components(&b, constant, tail_steps);
         nir_def* cond = match_condition(&b, *e, job.chain);
         // A null condition means the chain is exactly the specialized path:
         // the read is constant and the runtime fallback becomes dead.
         replacement = cond ? nir_bcsel(&b, cond, constant, replacement) : constant;
      }
      for (nir_src* src : uses)
         nir_src_rewrite(src, replacement);
      if (nir_def_is_unused(original))
         nir_instr_remove(&job.load->instr);
      changed = true;
   }
   if (changed)
      nir_progress(true, impl, nir_metadata_none);
   return changed;
}

} // namespace

std::string glslopt_apply_specialization(nir_shader* nir, const glslopt_specialized_value* values, unsigned count)
{
   std::vector<ResolvedEntry> entries;
   for (unsigned i = 0; i < count; i++) {
      const glslopt_specialized_value& value = values[i];
      std::string text = value.path ? value.path : "";
      if (text.empty())
         return "specialization table contains an empty path";
      if (value.components < 1 || value.components > 4 || !value.data)
         return "specialization path '" + text + "' has an invalid value descriptor";
      ResolvedEntry entry;
      std::string error;
      if (!resolve_path(nir, value, text, &entry, &error))
         return error;
      for (const ResolvedEntry& other : entries)
         if (same_target(other, entry))
            return "specialization table targets path '" + text + "' more than once";
      entries.push_back(std::move(entry));
   }
   if (entries.empty())
      return "";

   // After glslopt_lower_nir the shader is out of SSA: literal array indices
   // live in frontend const-temp registers, so deref indices are not yet
   // recognizable as constants. Promote function temporaries to SSA (uniforms
   // stay as derefs) so constant indices and their values are visible.
   bool progress = false;
   NIR_PASS(progress, nir, nir_lower_vars_to_ssa);
   NIR_PASS(progress, nir, nir_opt_dce);

   specialize_loads(nir, entries);
   return "";
}
