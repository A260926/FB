#!/usr/bin/env bash
# Copyright (C) 2015-2025 Jonathan Müller and foonathan/memory contributors
# SPDX-License-Identifier: Zlib

# Builds and runs the memory_stack AddressSanitizer checks in two configurations:
#   * FOONATHAN_MEMORY_ASAN=ON  -> full poison-lifecycle and fault checks
#   * FOONATHAN_MEMORY_ASAN=OFF -> the original layout/contract baseline and a
#                                  guarantee that no sanitizer symbol is required
#
# Only the system compiler's ASan runtime is used; nothing is downloaded and no
# test framework is involved.  C++11, CMake and plain shell only.

set -eu

cd "$(dirname "$0")/.."

ROOT="$PWD"
ON_DIR="$ROOT/build-stack-asan"
OFF_DIR="$ROOT/build-stack-baseline"
COMMON_CMAKE=(
    -DFOONATHAN_MEMORY_BUILD_TESTS=OFF
    -DFOONATHAN_MEMORY_BUILD_EXAMPLES=OFF
    -DFOONATHAN_MEMORY_BUILD_TOOLS=OFF
)

# Leak checking is irrelevant to these scenarios and the fault children abort on
# purpose, so keep the runtime focused on the memory-access reporting.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}:abort_on_error=1"
# Guard against a stray colon when ASAN_OPTIONS was already set.
export ASAN_OPTIONS="${ASAN_OPTIONS#:}"

echo "=== [1/4] configure + build library with FOONATHAN_MEMORY_ASAN=ON (Debug) ==="
cmake -S . -B "$ON_DIR" -DCMAKE_BUILD_TYPE=Debug \
      -DFOONATHAN_MEMORY_ASAN=ON "${COMMON_CMAKE[@]}"
cmake --build "$ON_DIR" --target foonathan_memory -j2

ON_LIB="$ON_DIR/src/libfoonathan_memory-0.7.4-dbg.a"
grep -q "define FOONATHAN_MEMORY_ASAN 1" "$ON_DIR/src/config_impl.hpp"

echo "=== [2/4] build and run ASan stack checks (instrumented) ==="
c++ -std=c++11 -fsanitize=address -fno-omit-frame-pointer \
    -Iinclude -I"$ON_DIR/src" \
    checks/stack_asan.cpp "$ON_LIB" -o "$ON_DIR/stack_asan"
"$ON_DIR/stack_asan"

echo "=== [3/4] configure + build library with FOONATHAN_MEMORY_ASAN=OFF (Release) ==="
cmake -S . -B "$OFF_DIR" -DCMAKE_BUILD_TYPE=Release \
      -DFOONATHAN_MEMORY_ASAN=OFF "${COMMON_CMAKE[@]}"
cmake --build "$OFF_DIR" --target foonathan_memory -j2

OFF_LIB="$OFF_DIR/src/libfoonathan_memory-0.7.4.a"
grep -q "define FOONATHAN_MEMORY_ASAN 0" "$OFF_DIR/src/config_impl.hpp"

# The feature-off library must not require any manual-poisoning interface symbol.
if nm -u "$OFF_LIB" 2>/dev/null | grep -Eq "__asan_(poison|unpoison|region_is_poisoned|address_is_poisoned)"; then
    echo "ERROR: FOONATHAN_MEMORY_ASAN=OFF library depends on ASan interface symbols" >&2
    exit 1
fi
echo "  feature-off library has no manual ASan interface dependency"

echo "=== [4/4] build and run the non-instrumented baseline contract check ==="
# No -fsanitize here: this also proves the public headers compile cleanly with
# the feature disabled and without any sanitizer runtime present.
c++ -std=c++11 -Iinclude -I"$OFF_DIR/src" \
    checks/stack_asan.cpp "$OFF_LIB" -o "$OFF_DIR/stack_asan_baseline"
"$OFF_DIR/stack_asan_baseline"

echo "=== stack_asan.sh: all checks passed ==="
