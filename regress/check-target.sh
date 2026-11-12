#!/bin/sh
set -eu
test -s tests/radial_contract.c || { echo 'missing target: tests/radial_contract.c' >&2; exit 1; }
build=$(mktemp -d)
trap 'rm -r "$build"' EXIT
cc -std=c99 -O2 tests/radial_contract.c -lm -o "$build/target"
"$build/target" > "$build/target.log" 2>&1 || { cat "$build/target.log"; exit 1; }
cat "$build/target.log"
for marker in RADIAL_FOCUS_OK RADIAL_INHERIT_OK RADIAL_COORDINATES_OK RADIAL_REUSE_OK; do grep -Fx "$marker" "$build/target.log" >/dev/null; done
