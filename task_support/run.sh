#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/ufbx-motion.XXXXXXXX")
trap 'rm -rf "$b"' EXIT
mode=${1:-}
[[ "$mode" == baseline || "$mode" == target ]]
if [[ "$mode" == target ]] && [[ ! -s test/motion_target.cpp ]]; then
 printf "MOTION_TARGET_MISSING_OR_EMPTY test/motion_target.cpp\n" >&2; exit 1
fi
cc -std=c99 -O0 -c ufbx.c -o "$b/ufbx.o"
if [[ "$mode" == target ]]; then
 printf "TARGET_COMPILE_BEGIN\n"
 c++ -std=c++17 -O0 -I. test/motion_target.cpp task_support/driver.cpp "$b/ufbx.o" -lm -o "$b/target"
 printf "TARGET_COMPILE_DONE\n"
 target_rc=0
 "$b/target" >"$b/target.out" 2>&1 || target_rc=$?
 cat "$b/target.out"
 printf "TARGET_PROCESS_EXIT %s\n" "$target_rc"
 if [[ "$target_rc" != 0 ]]; then exit "$target_rc"; fi
 for g in curves hierarchy deformation; do grep -Eq "^GROUP $g [1-9][0-9]* [1-9][0-9]* DONE$" "$b/target.out"; done
 grep -Eq '^TARGET_DONE [1-9][0-9]*$' "$b/target.out"
fi
cc -std=c99 -O0 task_support/baseline.c "$b/ufbx.o" -lm -o "$b/base"
"$b/base"
cc -std=c99 -O0 -Wno-switch test/runner.c "$b/ufbx.o" -lm -o "$b/old"
for name in maya_interpolation_modes maya_anim_pivot_rotate blender_279_sausage maya_blend_inbetween; do
 "$b/old" -d data -t "$name" >"$b/$name.out" 2>&1 || { cat "$b/$name.out"; exit 1; }
 cat "$b/$name.out"
 grep -q '^Tests passed: 1/1$' "$b/$name.out"
done
printf 'UFBX_OLD_TESTS_OK tests=4\n'
