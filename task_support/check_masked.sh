#!/usr/bin/env bash
#
# Build and verification for the masked quantization entry points.
# Toolchain: cc, ar and libm only.
set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p .task-build/objects-hook .task-build/objects
hook_objects=()
objects=()
for file in src/lib/*.c; do
  base="$(basename "${file%.c}")"
  hook_object=".task-build/objects-hook/${base}.o"
  object=".task-build/objects/${base}.o"
  cc -std=c99 -O2 -Wall -DSHINE_TEST_HOOKS -Isrc/lib -c "$file" \
    -o "$hook_object"
  cc -std=c99 -O2 -Wall -Isrc/lib -c "$file" -o "$object"
  hook_objects+=("$hook_object")
  objects+=("$object")
done
ar rcs .task-build/libshine-hooks.a "${hook_objects[@]}"
ar rcs .task-build/libshine.a "${objects[@]}"

# The new entry points must be exported by the normal ABI build.
# (Mach-O symbol names carry a leading underscore.)
for sym in shine_encode_buffer_masked \
           shine_encode_buffer_interleaved_masked; do
  if ! nm .task-build/libshine.a | grep -Eq " T _?${sym}\$"; then
    echo "missing exported symbol: $sym" >&2
    exit 1
  fi
done

# Instrumented build: full masked quantization verification.
cc -std=c99 -O2 -Wall -Wextra -DSHINE_TEST_HOOKS -Isrc/lib \
  tests/masked_quantization.c .task-build/libshine-hooks.a -lm \
  -o .task-build/masked_quantization
.task-build/masked_quantization

# The normal (no hook) build must keep the legacy byte output and ABI.
cc -std=c99 -O2 -Isrc/lib task_support/baseline.c .task-build/libshine.a \
  -lm -o .task-build/baseline
.task-build/baseline
