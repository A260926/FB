#!/bin/bash
set -euo pipefail
out=$1; shift
b=$(mktemp -d); trap 'rm -rf "$b"' EXIT
objects=()
for f in src/tinyspline.c src/parson.c src/tinyspline_projection.c; do
 if [[ -f "$f" ]]; then
  o="$b/$(basename "$f").o"
  cc -std=c99 -O1 -Isrc -c "$f" -o "$o"
  objects+=("$o")
 fi
done
c++ -std=c++17 -O1 -pthread -Isrc -Ichecks src/tinysplinecxx.cxx "${objects[@]}" "$@" -lm -o "$out"
