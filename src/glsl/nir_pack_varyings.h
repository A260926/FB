// Cross-stage varying packing for paired vertex/fragment shaders.
//
// Given two independently optimized NIR shaders (desktop GLSL, out of the
// regular glslopt pipeline), match the fragment shader's user inputs against
// the vertex shader's user outputs by name and pack scalars/vectors sharing
// the same base type and interpolation qualifiers into four-component varying
// variables. Both stages are rewritten to use identical internal variable
// names and component swizzles, so the pair stays linkable.
//
// Vertex outputs the fragment does not read are removed together with their
// stores, while the expressions that computed the stored values stay in place
// (their side effects are preserved).
#pragma once

#include "compiler/nir/nir.h"

// Analyze and rewrite the pair. On success returns NULL. On failure returns a
// static, human-readable English reason; neither shader is modified in that
// case (the caller marks both results as failed). Only GLSL 330-core style,
// non-array float/int/uint scalar and vector varyings are accepted.
// vertexSource/fragmentSource are the original sources, used only to make the
// generated names avoid every user identifier in either stage.
const char* glslopt_pack_varying_interface(nir_shader* vertex, nir_shader* fragment,
                                            const char* vertexSource, const char* fragmentSource);
