# Robust buffer access (`CompilerGLSL::Options::robust_buffer_access`)

This directory contains the checks for the optional `robust_buffer_access` GLSL backend option.

## What it does

When enabled, every supported scalar/vector load and store made through an ordinary
`OpAccessChain` (including chains built from other chains) into a logical `StorageBuffer` block is
emitted with an explicit, per-level bounds guard:

- an out-of-bounds **read** evaluates to a zero of the result type;
- an out-of-bounds **write** is dropped without touching any element;
- the guard replaces the usual reliance on a driver implicitly clamping the index.

The option defaults to **off**. With it off, generated GLSL and resource reflection are byte-for-byte
unchanged.

## Protection boundary

Only this combination is guarded:

- desktop (non-ES) **GLSL 430**, `GLCompute` entry points;
- logical addressing (no physical storage buffer pointers);
- 32-bit `int`, `uint` and `float`, scalar or vector (up to `vec4`) values;
- blocks containing structs, fixed-length arrays and a trailing runtime array, in any nesting that
  ends in such a scalar/vector.

Every array layer is checked independently (a flattened-offset check is intentionally not used), and a
dynamic vector component index is checked against the vector width. A runtime array is bounded by its
actual `.length()`, so an empty runtime array rejects every access. Struct member indices are static
constants and need no runtime test. A constant index into a fixed-length array is still checked,
because a constant can exceed the type's declared length.

Indices are normalized to `uint` before an unsigned `<` comparison. A negative signed index therefore
wraps to a large unsigned value and fails, as does a genuinely huge unsigned index, with no
overflow-based escape.

### Rejected rather than partially guarded

If the option is on and the module uses an SSBO access that cannot be guarded this way, compilation
fails with a `CompilerError` instead of silently emitting an unguarded access. This covers:

- any non-desktop/ES target, GLSL below 430, or a non-compute entry point;
- physical storage buffer pointers;
- `OpPtrAccessChain`, cross-function SSBO pointer arguments, and pointer `OpPhi` / `OpSelect`;
- `OpCopyMemory` (and whole-struct / whole-array copies);
- atomic and cooperative-matrix load/store opcodes;
- whole-struct or other non scalar/vector SSBO loads and stores.

Function-local (private) arrays and non-SSBO resources are never affected.

## Evaluation-order guarantees

- Building an access chain alone never guards or evaluates anything; the check happens at the load or
  store.
- Each dynamic index is evaluated once, in SPIR-V chain order, and reused by both the condition and the
  address, so helper side effects and evaluation counts are preserved.
- A store evaluates its right-hand side (including side-effecting helper calls) before the address
  guard, so a dropped out-of-bounds store still runs the helper exactly once.
- A guarded load is materialized into a temporary at its source position, so it cannot be forwarded
  past a later store and read a new value.
- Guards are emitted inside the loop/branch that owns the access and are never hoisted out of scope.

## Layout

- `spirv_builder.hpp` - dependency-free SPIR-V 1.3 word constructor used to build valid test modules.
- `modules.hpp` - the SPIR-V modules for each scenario (runtime/fixed/nested arrays, vectors, helper
  side effects, loops/branches, atomic and whole-struct rejections).
- `cpu_model.hpp` - the bounded storage model: sentinel memory on both sides, event logging and hard
  violations for any out-of-range access, with GLSL-compatible scalar/vector/array types.
- `check-robust.cpp` - compiles each module with the option on and off, translates the generated GLSL
  function bodies to C++ against the bounded model, and asserts on values and access events.

Run from the repository root:

```sh
sh checks/check-robust.sh
```

The final line reports `CASES n ASSERTIONS m FAILURES 0` on success. The test only needs a C++11
compiler; no glslang or SPIR-V tools are required.
