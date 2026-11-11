#!/usr/bin/env bash
set -euo pipefail
# Both sources and dedicated build targets must be added by the change.
test -s tests/ordered/ordered_match_test.cpp
test -s tests/ordered/ordered_c_smoke.c
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_TOOLS=OFF -DBUILD_TESTS=ON -DFFT_LIB=kissfft -DUSE_INTERNAL_AVRESAMPLE=ON
cmake --build "$build" --parallel 2 --target ordered_tests ordered_c_smoke all_tests
"$build/tests/ordered_tests"
"$build/tests/ordered_c_smoke"
ctest --test-dir "$build" --output-on-failure
