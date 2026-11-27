// Partial specialization of ordinary uniforms (see glslopt_optimize_specialized
// in glsl_optimizer.h). The table is validated against the pre-optimization
// uniform declarations, then surviving uniform loads are rewritten into
// constants (constant-index accesses) or selects between the constant and the
// original uniform read (dynamic-index accesses), so the existing NIR
// optimization pipeline removes the branches, textures and uniforms that the
// known values make dead.
#pragma once

#include "glsl_optimizer.h"

#include <string>

struct nir_shader;

// Validate and apply a specialization table to nir (already through
// glslopt_lower_nir). Returns an empty string on success and an error message
// naming the offending path on failure; on failure nir is left untouched.
// The table itself is never modified.
std::string glslopt_apply_specialization(nir_shader* nir,
                                         const glslopt_specialized_value* values,
                                         unsigned count);

struct glslopt_shader;
// Test/debug accessor: the optimized NIR backing a result. Valid until the
// result is deleted. Not part of the stable public surface.
nir_shader* glslopt_shader_get_nir_for_test(glslopt_shader* shader);
