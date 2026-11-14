// Test-only access to the optimized NIR. This is intentionally NOT part of the
// public glsl_optimizer.h API and lives with the tests: the CPU reference
// interpreter executes the same IR the printers consume, so specialization
// tests can compare a specialized shader against the unspecialized original
// numerically. The symbol itself is provided by the optimizer library.
#pragma once

struct nir_shader;
struct glslopt_shader;

// Valid until glslopt_shader_delete; never NULL for a successful result.
nir_shader* glslopt_test_get_nir(glslopt_shader* shader);
