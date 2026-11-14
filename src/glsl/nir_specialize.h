// Partial uniform specialization: substitute a few fully-known scalar/vector
// leaves of default uniforms with constants, while every other element keeps
// its runtime value. The rewrite is deliberately local to each uniform load,
// so dynamic indices and the side effects that compute them stay exactly where
// the source program placed them; the regular NIR pipeline then folds the
// constants and removes the branches and texture operations they make dead.
#pragma once

#include "glsl_optimizer.h"
#include "compiler/nir/nir.h"
#include <string>
#include <vector>

struct glslopt_spec_leaf {
   struct step {
      bool is_struct;   // false: fixed-size array element
      unsigned index;   // struct field index or constant array index
   };
   nir_variable* var = nullptr;
   std::vector<step> path; // steps below the variable, outermost first
   const glsl_type* type = nullptr; // scalar or vector leaf type
   unsigned components = 0;
   unsigned bit_size = 0;
   nir_const_value values[4] = {};
};

// Resolves and type-checks every binding against the declarations as they are
// before optimization (unused uniforms are accepted). On any error, fills
// error with a message naming the offending path and returns false without
// touching the shader.
bool glslopt_spec_build_leaves(nir_shader* nir,
                               const glslopt_specialized_uniform* bindings,
                               int bindingCount,
                               std::vector<glslopt_spec_leaf>& leaves,
                               std::string& error);

// Rewrites uniform loads in the already-lowered, single-entrypoint shader.
// Aggregates with remaining runtime reads keep their shape and name.
void glslopt_spec_rewrite_loads(nir_shader* nir,
                                const std::vector<glslopt_spec_leaf>& leaves);
