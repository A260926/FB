// Test-only read bridge; the included file is the current production compiler.
#include "../src/glsl/glsl_optimizer.cpp"
#include "inspect.h"
const nir_shader* task_shader_nir(const glslopt_shader* shader) {
    return shader && shader->shader ? shader->shader->nir : nullptr;
}
