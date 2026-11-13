#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in baseline|target) ;; *) exit 2;; esac
build=$(mktemp -d "${TMPDIR:-/tmp}/ufbx-link-modes.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
"${CC:-cc}" -std=c99 -O1 -c ufbx.c -o "$build/ufbx.o"
"${CXX:-c++}" -std=c++17 -O1 -I. task_support/baseline.cpp "$build/ufbx.o" -lm -o "$build/baseline"
"$build/baseline"
"${CC:-cc}" -std=c99 -O1 test/runner.c "$build/ufbx.o" -lm -lpthread -o "$build/upstream"
for name in blender_279_sausage maya_dual_quaternion blender_293_half_skinned; do
    "$build/upstream" --test "$name" --data data --no-fuzz
    printf 'UPSTREAM_OK %s\n' "$name"
done
printf 'BASELINE_OK\n'
if [ "$mode" = target ]; then
    if [ ! -s test/link_modes_target.cpp ]; then
        printf 'MISSING_TARGET test/link_modes_target.cpp\n' >&2
        exit 2
    fi
    "${CXX:-c++}" -std=c++17 -O1 -I. -Itask_support task_support/driver.cpp test/link_modes_target.cpp "$build/ufbx.o" -lm -o "$build/target"
    "$build/target"
fi
