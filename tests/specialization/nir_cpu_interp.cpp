#include "nir_cpu_interp.h"
#include "compiler/nir/nir_constant_expressions.h"
#include "compiler/glsl_types.h"
#include <algorithm>
#include <cstring>

using std::string;

namespace {

unsigned
base_bit_size(const glsl_type* t)
{
   // NIR represents booleans as 1-bit values even though the GLSL type is 32-bit.
   if (glsl_get_base_type(glsl_without_array(t)) == GLSL_TYPE_BOOL)
      return 1;
   return glsl_base_type_bit_size(glsl_get_base_type(glsl_without_array(t)));
}

bool
is_opaque(const glsl_type* t)
{
   const glsl_type* bare = glsl_without_array(t);
   return glsl_type_is_sampler(bare) || glsl_type_is_image(bare);
}

bool
contains_matrix(const glsl_type* t)
{
   if (glsl_type_is_matrix(t))
      return true;
   if (glsl_type_is_array(t))
      return contains_matrix(glsl_get_array_element(t));
   if (glsl_type_is_struct(t) || glsl_type_is_interface(t)) {
      for (unsigned i = 0; i < glsl_get_length(t); i++)
         if (contains_matrix(glsl_get_struct_field(t, i)))
            return true;
   }
   return false;
}

} // namespace

nir_cpu_machine::nir_cpu_machine(nir_shader* shader) : nir(shader)
{
   nir_foreach_variable_in_shader(var, nir) {
      if (var->data.mode != nir_var_uniform && var->data.mode != nir_var_shader_in &&
          var->data.mode != nir_var_shader_out)
         continue;
      // In these modes interface_type only tags built-in IO (gl_PerVertex),
      // not UBO/SSBO blocks; those live under nir_var_mem_ubo/ssbo. Samplers
      // and images are the only opaque leaves and are out of scope here.
      if (is_opaque(var->type))
         continue;
      cell* c = build_cells(var->type);
      roots[var] = c;
   }
}

nir_cpu_machine::cell*
nir_cpu_machine::build_cells(const glsl_type* type)
{
   cell* c = new cell;
   owned.push_back(c);
   c->type = type;
   c->bit_size = base_bit_size(type);

   if (glsl_type_is_array(type)) {
      unsigned n = glsl_get_length(type);
      c->kids.reserve(n);
      for (unsigned i = 0; i < n; i++)
         c->kids.push_back(build_cells(glsl_get_array_element(type)));
   } else if (glsl_type_is_struct(type) || glsl_type_is_interface(type)) {
      unsigned n = glsl_get_length(type);
      c->kids.reserve(n);
      for (unsigned i = 0; i < n; i++)
         c->kids.push_back(build_cells(glsl_get_struct_field(type, i)));
   } else {
      // scalar or vector leaf (matrices are rejected by unsupported())
      c->leaf = true;
      c->vals.assign(glsl_get_components(type), nir_const_value{});
   }
   return c;
}

bool
nir_cpu_machine::unsupported(string* reason) const
{
   nir_function_impl* impl = nir_shader_get_entrypoint(nir);
   nir_foreach_block(block, impl) {
      nir_foreach_instr(instr, block) {
         if (instr->type == nir_instr_type_tex) {
            if (reason) *reason = "texture operations are not executed by the CPU bridge";
            return true;
         }
         if (instr->type == nir_instr_type_intrinsic) {
            switch (nir_instr_as_intrinsic(instr)->intrinsic) {
            case nir_intrinsic_decl_reg:
            case nir_intrinsic_load_reg:
            case nir_intrinsic_load_reg_indirect:
            case nir_intrinsic_store_reg:
            case nir_intrinsic_store_reg_indirect:
            case nir_intrinsic_load_deref:
            case nir_intrinsic_store_deref:
            case nir_intrinsic_glslopt_precision:
               break;
            default:
               if (reason) *reason = "unsupported intrinsic in CPU bridge";
               return true;
            }
         }
         if (instr->type == nir_instr_type_deref) {
            if (nir_instr_as_deref(instr)->deref_type == nir_deref_type_cast ||
                nir_instr_as_deref(instr)->deref_type == nir_deref_type_ptr_as_array ||
                nir_instr_as_deref(instr)->deref_type == nir_deref_type_array_wildcard) {
               if (reason) *reason = "unsupported deref form in CPU bridge";
               return true;
            }
         }
      }
   }
   nir_foreach_variable_in_shader(var, nir) {
      if ((var->data.mode == nir_var_uniform || var->data.mode == nir_var_shader_in ||
           var->data.mode == nir_var_shader_out) &&
          contains_matrix(var->type)) {
         if (reason) *reason = "matrix variables are not modeled by the CPU bridge";
         return true;
      }
   }
   return false;
}

nir_cpu_machine::cell*
nir_cpu_machine::root_cell(const string& name) const
{
   for (auto& kv : roots)
      if (kv.first->name && name == kv.first->name)
         return kv.second;
   return nullptr;
}

bool
nir_cpu_machine::set_leaf(const string& root,
                          const std::vector<glslopt_path_segment>& path,
                          glslopt_value_type type, const void* component_data,
                          int components, string& error)
{
   cell* c = root_cell(root);
   if (!c) {
      error = "interpreter: unknown variable " + root;
      return false;
   }
   for (const auto& seg : path) {
      if (seg.kind == kGlslPathStructField) {
         if (!glsl_type_is_struct(c->type)) {
            error = "interpreter: field on non-struct in " + root;
            return false;
         }
         int idx = glsl_get_field_index(c->type, seg.field);
         if (idx < 0) {
            error = "interpreter: no field " + string(seg.field);
            return false;
         }
         c = c->kids[idx];
      } else {
         if (!glsl_type_is_array(c->type) || seg.arrayIndex < 0 ||
             (unsigned)seg.arrayIndex >= c->kids.size()) {
            error = "interpreter: bad array index for " + root;
            return false;
         }
         c = c->kids[seg.arrayIndex];
      }
   }
   if (!c->leaf || (int)c->vals.size() != components) {
      error = "interpreter: leaf shape mismatch for " + root;
      return false;
   }
   const float* fv = (const float*)component_data;
   const int32_t* iv = (const int32_t*)component_data;
   glsl_base_type bt = glsl_get_base_type(c->type);
   for (int i = 0; i < components; i++) {
      nir_const_value cv = {};
      switch (type) {
      case kGlslValueTypeFloat32:
         if (bt != GLSL_TYPE_FLOAT) { error = "interpreter: float on non-float leaf"; return false; }
         cv.f32 = fv[i];
         break;
      case kGlslValueTypeInt32:
         if (bt == GLSL_TYPE_INT) cv.i32 = iv[i];
         else if (bt == GLSL_TYPE_UINT) cv.u32 = (uint32_t)iv[i];
         else { error = "interpreter: int on non-int leaf"; return false; }
         break;
      case kGlslValueTypeBool:
         if (bt != GLSL_TYPE_BOOL) { error = "interpreter: bool on non-bool leaf"; return false; }
         cv.b = iv[i] != 0;
         break;
      }
      c->vals[i] = cv;
   }
   return true;
}

const nir_cpu_value*
nir_cpu_machine::eval_src(nir_src src, string& error)
{
   auto it = ssa.find(src.ssa);
   if (it == ssa.end()) {
      error = "interpreter: value not evaluated (unsupported instruction)";
      return nullptr;
   }
   return &it->second;
}

bool
nir_cpu_machine::resolve_deref(nir_deref_instr* deref, cell*& out_cell, int& out_component, string& error)
{
   nir_variable* var = nir_deref_instr_get_variable(deref);
   auto rit = roots.find(var);
   if (rit == roots.end()) {
      error = std::string("interpreter: deref of unknown variable ") +
              (var && var->name ? var->name : "(unnamed)");
      return false;
   }
   // Collect the chain root -> leaf so cell descent follows declaration order.
   std::vector<nir_deref_instr*> chain;
   for (nir_deref_instr* p = deref; p; p = nir_deref_instr_parent(p))
      chain.push_back(p);
   std::reverse(chain.begin(), chain.end());

   cell* c = rit->second;
   int component = -1;
   for (size_t k = 1; k < chain.size(); k++) {
      nir_deref_instr* p = chain[k];
      if (p->deref_type == nir_deref_type_struct) {
         if (p->strct.index >= c->kids.size()) { error = "interpreter: struct field OOB"; return false; }
         c = c->kids[p->strct.index];
      } else if (p->deref_type == nir_deref_type_array) {
         if (c->leaf) {
            component = (int)nir_src_as_int(p->arr.index); // vector component
         } else {
            const nir_cpu_value* idx = eval_src(p->arr.index, error);
            if (!idx) return false;
            unsigned at = idx->v[0].u32;
            if (at >= c->kids.size()) { error = "interpreter: array index out of range"; return false; }
            c = c->kids[at];
         }
      }
   }
   out_cell = c;
   out_component = component;
   return true;
}

bool
nir_cpu_machine::eval_instr(nir_instr* instr, flow& out_flow, string& error)
{
   out_flow = FLOW_NORMAL;
   if (instr->type == nir_instr_type_deref)
      return true; // pointers are resolved on demand

   if (instr->type == nir_instr_type_jump) {
      switch (nir_instr_as_jump(instr)->type) {
      case nir_jump_return: out_flow = FLOW_RETURN; return true;
      case nir_jump_break: out_flow = FLOW_BREAK; return true;
      case nir_jump_continue: out_flow = FLOW_CONTINUE; return true;
      default: error = "interpreter: unsupported jump"; return false;
      }
   }

   if (instr->type == nir_instr_type_load_const) {
      nir_load_const_instr* lc = nir_instr_as_load_const(instr);
      nir_cpu_value val;
      val.components = lc->def.num_components;
      val.bit_size = lc->def.bit_size;
      for (unsigned i = 0; i < val.components; i++)
         val.v[i] = lc->value[i];
      ssa[&lc->def] = val;
      return true;
   }

   if (instr->type == nir_instr_type_undef) {
      nir_cpu_value val;
      val.components = nir_instr_as_undef(instr)->def.num_components;
      val.bit_size = nir_instr_as_undef(instr)->def.bit_size;
      ssa[&nir_instr_as_undef(instr)->def] = val;
      return true;
   }

   if (instr->type == nir_instr_type_alu) {
      nir_alu_instr* alu = nir_instr_as_alu(instr);
      nir_const_value* srcs[NIR_ALU_MAX_INPUTS] = {};
      nir_const_value src_storage[NIR_ALU_MAX_INPUTS][NIR_MAX_VEC_COMPONENTS];
      unsigned ninputs = nir_op_infos[alu->op].num_inputs;
      for (unsigned i = 0; i < ninputs; i++) {
         const nir_cpu_value* sv = eval_src(alu->src[i].src, error);
         if (!sv) return false;
         unsigned ncomp = nir_ssa_alu_instr_src_components(alu, i);
         for (unsigned j = 0; j < ncomp; j++) {
            unsigned swz = alu->src[i].swizzle[j];
            src_storage[i][j] = sv->v[swz < sv->components ? swz : 0];
         }
         srcs[i] = src_storage[i];
      }
      nir_cpu_value val;
      val.components = alu->def.num_components;
      val.bit_size = alu->def.bit_size;
      nir_const_value dest[NIR_MAX_VEC_COMPONENTS] = {};
      nir_eval_const_opcode(alu->op, dest, NULL, val.components, val.bit_size, srcs,
                            nir->info.float_controls_execution_mode);
      for (unsigned i = 0; i < val.components; i++)
         val.v[i] = dest[i];
      ssa[&alu->def] = val;
      return true;
   }

   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr* in = nir_instr_as_intrinsic(instr);
      switch (in->intrinsic) {
      case nir_intrinsic_decl_reg: {
         reg_slot r;
         r.components = nir_intrinsic_num_components(in);
         r.bit_size = nir_intrinsic_bit_size(in);
         unsigned elems = nir_intrinsic_num_array_elems(in);
         if (elems == 0)
            elems = 1;
         r.elems.assign(elems, std::vector<nir_const_value>(r.components, nir_const_value{}));
         regs[&in->def] = std::move(r);
         return true;
      }
      case nir_intrinsic_load_reg:
      case nir_intrinsic_load_reg_indirect: {
         const nir_def* handle = in->src[0].ssa;
         auto rit = regs.find(handle);
         if (rit == regs.end()) { error = "interpreter: load of unknown register"; return false; }
         reg_slot& r = rit->second;
         unsigned elem = 0;
         if (in->intrinsic == nir_intrinsic_load_reg_indirect) {
            const nir_cpu_value* idx = eval_src(in->src[1], error);
            if (!idx) return false;
            elem = idx->v[0].u32;
         } else {
            elem = nir_intrinsic_base(in);
         }
         nir_cpu_value val;
         val.components = r.components;
         val.bit_size = r.bit_size;
         for (unsigned i = 0; i < r.components; i++)
            val.v[i] = r.elems[elem][i];
         ssa[&in->def] = val;
         return true;
      }
      case nir_intrinsic_store_reg:
      case nir_intrinsic_store_reg_indirect: {
         const nir_cpu_value* data = eval_src(in->src[0], error);
         if (!data) return false;
         const nir_def* handle = in->src[1].ssa;
         auto rit = regs.find(handle);
         if (rit == regs.end()) { error = "interpreter: store to unknown register"; return false; }
         reg_slot& r = rit->second;
         unsigned elem = in->intrinsic == nir_intrinsic_store_reg_indirect
                             ? eval_src(in->src[2], error)->v[0].u32
                             : nir_intrinsic_base(in);
         unsigned mask = nir_intrinsic_write_mask(in);
         for (unsigned i = 0; i < r.components; i++)
            if (mask & (1u << i))
               r.elems[elem][i] = data->v[i < data->components ? i : 0];
         return true;
      }
      case nir_intrinsic_glslopt_precision: {
         const nir_cpu_value* v = eval_src(in->src[0], error);
         if (!v) return false;
         ssa[&in->def] = *v;
         return true;
      }
      case nir_intrinsic_load_deref: {
         nir_deref_instr* d = nir_src_as_deref(in->src[0]);
         cell* c;
         int component;
         if (!resolve_deref(d, c, component, error))
            return false;
         nir_cpu_value val;
         if (component >= 0) {
            val.components = 1;
            val.bit_size = c->bit_size;
            val.v[0] = c->vals[component];
         } else {
            val.components = (unsigned)c->vals.size();
            val.bit_size = c->bit_size;
            for (unsigned i = 0; i < val.components; i++)
               val.v[i] = c->vals[i];
         }
         ssa[&in->def] = val;
         return true;
      }
      case nir_intrinsic_store_deref: {
         nir_deref_instr* d = nir_src_as_deref(in->src[0]);
         const nir_cpu_value* data = eval_src(in->src[1], error);
         if (!data) return false;
         cell* c;
         int component;
         if (!resolve_deref(d, c, component, error))
            return false;
         unsigned mask = nir_intrinsic_write_mask(in);
         if (component >= 0) {
            if (mask & 1u)
               c->vals[component] = data->v[0];
         } else {
            for (unsigned i = 0; i < c->vals.size(); i++)
               if (mask & (1u << i))
                  c->vals[i] = data->v[i < data->components ? i : 0];
         }
         return true;
      }
      default:
         error = "interpreter: unsupported intrinsic";
         return false;
      }
   }

   error = "interpreter: unsupported instruction";
   return false;
}

nir_cpu_machine::flow
nir_cpu_machine::exec_block(nir_block* block)
{
   flow f = FLOW_NORMAL;
   nir_foreach_instr_safe(instr, block) {
      string err;
      if (!eval_instr(instr, f, err)) {
         unsupported_reason = err;
         return FLOW_RETURN; // signal stop; run() reports the error
      }
      if (f != FLOW_NORMAL)
         return f;
   }
   return FLOW_NORMAL;
}

nir_cpu_machine::flow
nir_cpu_machine::exec_if(nir_if* if_stmt)
{
   const nir_cpu_value* cond = eval_src(if_stmt->condition, unsupported_reason);
   if (!cond)
      return FLOW_RETURN;
   bool taken = cond->v[0].b;
   return taken ? exec_cf_list(&if_stmt->then_list) : exec_cf_list(&if_stmt->else_list);
}

nir_cpu_machine::flow
nir_cpu_machine::exec_loop(nir_loop* loop)
{
   static const int kMaxIterations = 1000000;
   for (int it = 0; it < kMaxIterations; it++) {
      flow f = exec_cf_list(&loop->body);
      if (f == FLOW_BREAK || f == FLOW_RETURN)
         return f == FLOW_BREAK ? FLOW_NORMAL : f;
      if (f == FLOW_CONTINUE)
         continue;
   }
   unsupported_reason = "interpreter: loop iteration limit exceeded";
   return FLOW_RETURN;
}

nir_cpu_machine::flow
nir_cpu_machine::exec_cf_list(const exec_list* list)
{
   foreach_list_typed(nir_cf_node, node, node, list) {
      flow f = FLOW_NORMAL;
      switch (node->type) {
      case nir_cf_node_block:
         f = exec_block(nir_cf_node_as_block(node));
         break;
      case nir_cf_node_if:
         f = exec_if(nir_cf_node_as_if(node));
         break;
      case nir_cf_node_loop:
         f = exec_loop(nir_cf_node_as_loop(node));
         break;
      default:
         unsupported_reason = "interpreter: unsupported control-flow node";
         return FLOW_RETURN;
      }
      if (f == FLOW_RETURN)
         return FLOW_RETURN;
      if (f == FLOW_BREAK || f == FLOW_CONTINUE)
         return f;
   }
   return FLOW_NORMAL;
}

bool
nir_cpu_machine::run(string& error)
{
   ssa.clear();
   regs.clear();
   unsupported_reason.clear();
   string why;
   if (unsupported(&why)) {
      error = why;
      return false;
   }
   flow f = exec_cf_list(&nir_shader_get_entrypoint(nir)->body);
   if (f == FLOW_RETURN && !unsupported_reason.empty()) {
      error = unsupported_reason;
      return false;
   }
   return true;
}

bool
nir_cpu_machine::get_output(const string& name, nir_cpu_value& out, string& error) const
{
   cell* c = root_cell(name);
   if (!c || !c->leaf) {
      error = "interpreter: unknown output " + name;
      return false;
   }
   out.components = (unsigned)c->vals.size();
   out.bit_size = c->bit_size;
   for (unsigned i = 0; i < out.components; i++)
      out.v[i] = c->vals[i];
   return true;
}
