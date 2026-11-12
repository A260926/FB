#!/usr/bin/env bash
# Legato test driver: rerun the classic baseline, then build and run
# the seven named legato C cases.  Any missing file or failed case
# aborts the script; success prints FLUIDLITE_LEGATO_OK cases=7.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

test -f include/fluidlite.h
test -f src/fluid_legato.c
test -f tests/legato.c

build=$(mktemp -d "${TMPDIR:-/tmp}/fluidlite-legato.XXXXXX")
trap 'rm -rf "$build"' EXIT

echo "== baseline =="
bash repro/baseline.sh

echo "== build =="
cmake -S . -B "$build" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DCMAKE_BUILD_TYPE=Release -DFLUIDLITE_BUILD_SHARED=OFF \
      -DENABLE_SF3=OFF >/dev/null
cmake --build "$build" --target fluidlite-static -j4 >/dev/null

cc -std=c99 -UNDEBUG -Iinclude -Isrc tests/legato.c \
   "$build/libfluidlite.a" -lm -o "$build/legato"

echo "== legato cases =="
"$build/legato" | tee "$build/results.txt"

grep -q '^FLUIDLITE_LEGATO_OK cases=7$' "$build/results.txt"
