#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
build=$(mktemp -d "${TMPDIR:-/tmp}/tinyspline-intersect.XXXXXX")
trap 'rm -rf "$build"' EXIT
cc=${CC:-cc}
cxx=${CXX:-c++}
base=(-std=c99 -O2 -Isrc)
"$cc" "${base[@]}" -c src/tinyspline.c -o "$build/tinyspline.o"
"$cc" "${base[@]}" -c src/parson.c -o "$build/parson.o"
objects=("$build/tinyspline.o" "$build/parson.o")
upstream() {
  "$cc" "${base[@]}" -Itest -Itest/cutest "${objects[@]}" task_support/upstream.c test/testutils.c test/cutest/CuTest.c test/c/eval.c test/c/to_beziers.c test/c/insert_knot.c test/c/derive.c -lm -o "$build/upstream"
  "$build/upstream" | tee "$build/upstream.log"
  grep -Eq '^UPSTREAM_SELECTED_OK cases=[1-9][0-9]*$' "$build/upstream.log"
}
case "$mode" in
  baseline)
    "$cc" "${base[@]}" "${objects[@]}" task_support/baseline.c -lm -o "$build/baseline"
    "$build/baseline"
    upstream
    ;;
  target)
    for f in src/tinyspline_intersect.h src/tinyspline_intersect.c regress/intersections.cpp; do
      test -s "$f" || { echo "missing or empty: $f" >&2; exit 2; }
    done
    "$cc" "${base[@]}" -c src/tinyspline_intersect.c -o "$build/intersect.o"
    "$cxx" -std=c++11 -O2 -Isrc -Itask_support src/tinysplinecxx.cxx task_support/main.cpp regress/intersections.cpp "${objects[@]}" "$build/intersect.o" -lm -o "$build/target"
    "$build/target" | tee "$build/target.log"
    for group in isolated overlap integration; do
      grep -Eq "^PASS $group cases=[1-9][0-9]* checks=[1-9][0-9]*$" "$build/target.log"
    done
    grep -Eq '^INTERSECTIONS_ALL_DONE cases=[1-9][0-9]*$' "$build/target.log"
    upstream
    ;;
  *) echo 'use baseline or target' >&2; exit 2;;
esac
