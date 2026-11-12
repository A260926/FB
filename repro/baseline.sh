#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d "${TMPDIR:-/tmp}/fluidlite-baseline.XXXXXX")
trap 'rm -rf "$build"' EXIT
cmake -S . -B "$build" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release -DFLUIDLITE_BUILD_SHARED=OFF -DENABLE_SF3=OFF >/dev/null
cmake --build "$build" --target fluidlite-static -j4 >/dev/null
cc -std=c99 -UNDEBUG -Iinclude repro/baseline.c "$build/libfluidlite.a" -lm -o "$build/baseline"
"$build/baseline"
