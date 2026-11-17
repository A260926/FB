#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

build=.task-build/hook
mkdir -p "$build/objects"
objects=()
for file in src/lib/*.c; do
  object="$build/objects/$(basename "${file%.c}").o"
  cc -std=c99 -O2 -Wall -Wextra -Isrc/lib -DSHINE_TEST_HOOKS -c "$file" \
    -o "$object"
  objects+=("$object")
done
ar rcs "$build/libshine-hooks.a" "${objects[@]}"

cc -std=c99 -O2 -Wall -Wextra -Isrc/lib -DSHINE_TEST_HOOKS \
  tests/masked_quantization.c "$build/libshine-hooks.a" -lm \
  -o "$build/masked_quantization"

"$build/masked_quantization"
