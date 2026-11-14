// A small CPU interpreter for the optimized, post-SSA NIR produced by this
// library. It is a test bridge: specialization tests execute the same IR the
// GLSL printer emits, both for the original shader (with runtime uniform
// values) and for the specialized one, and compare the captured outputs.
//
// Scope is intentionally narrow: 32-bit float/int/uint and bool scalars and
// vectors, struct/array derefs, registers, if/loop control flow. Texture
// operations are not modeled - tests that exercise texture removal assert on
// reflection/stats instead of executing a sampler.
#pragma once

#include "glsl_optimizer.h"
#include "compiler/nir/nir.h"
#include <string>
#include <unordered_map>
#include <vector>

struct nir_cpu_value {
   unsigned components = 0;
   unsigned bit_size = 0;
   nir_const_value v[4] = {};
};

class nir_cpu_machine {
public:
   explicit nir_cpu_machine(nir_shader* nir);

   // True when the shader uses an instruction the bridge cannot execute
   // (textures, atomics, ...). Such shaders must be checked by other means.
   bool unsupported(std::string* reason) const;

   // Write a scalar/vector leaf of a default uniform or shader input. The path
   // uses the same shape as a specialization binding: a struct-field step
   // carries the field name, an array step its constant index.
   bool set_leaf(const std::string& root,
                 const std::vector<glslopt_path_segment>& path,
                 glslopt_value_type type, const void* component_data,
                 int components, std::string& error);

   // Whether the optimized IR still declares this variable (a fully removed
   // uniform or input is simply absent).
   bool has_root(const std::string& name) const { return root_cell(name) != nullptr; }

   // Execute the entry point once over the values supplied so far.
   bool run(std::string& error);

   // Read a captured scalar/vector shader output by variable name.
   bool get_output(const std::string& name, nir_cpu_value& out, std::string& error) const;

private:
   struct cell {
      const glsl_type* type = nullptr;
      bool leaf = false;
      std::vector<nir_const_value> vals; // leaf components
      unsigned bit_size = 32;
      std::vector<cell*> kids;           // array elements or struct fields
   };

   cell* build_cells(const glsl_type* type);
   cell* root_cell(const std::string& name) const;

   nir_shader* nir;
   std::unordered_map<const nir_variable*, cell*> roots;
   std::vector<cell*> owned;
   std::string unsupported_reason;

   // execution state
   std::unordered_map<const nir_def*, nir_cpu_value> ssa;
   struct reg_slot {
      unsigned components;
      unsigned bit_size;
      std::vector<std::vector<nir_const_value>> elems; // per array element
   };
   std::unordered_map<const nir_def*, reg_slot> regs;

   enum flow { FLOW_NORMAL, FLOW_BREAK, FLOW_CONTINUE, FLOW_RETURN };
   flow exec_cf_list(const struct exec_list* list);
   flow exec_block(nir_block* block);
   flow exec_if(nir_if* if_stmt);
   flow exec_loop(nir_loop* loop);
   bool eval_instr(nir_instr* instr, flow& out_flow, std::string& error);
   const nir_cpu_value* eval_src(nir_src src, std::string& error);
   // Resolve a deref chain (root to leaf) to a storage cell and, for vector
   // component derefs, the component index (-1 otherwise).
   bool resolve_deref(nir_deref_instr* deref, cell*& out_cell, int& out_component,
                      std::string& error);
   cell* deref_cell(nir_deref_instr* deref, std::string& error) const;
};
