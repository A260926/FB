#!/bin/bash
set -euo pipefail
mkdir -p .task-build
includes=(-I. -IlibMachO)
while IFS= read -r dir; do includes+=("-I$dir"); done < <(find libMachO -type d | sort)
flags=(-std=gnu11 -fblocks -O0 -g -Wno-deprecated-declarations -Wno-unused-value)
objects=()
while IFS= read -r src; do
  obj=".task-build/${src//\//_}.o"
  if [[ ! -f $obj || $src -nt $obj ]]; then clang "${flags[@]}" "${includes[@]}" -c "$src" -o "$obj"; fi
  objects+=("$obj")
done < <(find libMachO -name '*.c' | sort)
rm -f .task-build/libMachO.a
ar rcs .task-build/libMachO.a "${objects[@]}"
clang "${flags[@]}" "${includes[@]}" task_support/baseline.c .task-build/libMachO.a -o .task-build/baseline
