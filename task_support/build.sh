#!/bin/bash
set -euo pipefail
mkdir -p .task-build
flags=(-std=c11 -D_POSIX_C_SOURCE=200809L -O0 -g -I. -Itask_support/compat)
objects=()
for src in ./*.c; do
  [[ $src == ./test.c ]] && continue
  obj=".task-build/$(basename "${src%.c}").o"
  if [[ ! -f $obj || $src -nt $obj ]]; then cc "${flags[@]}" -c "$src" -o "$obj"; fi
  objects+=("$obj")
done
rm -f .task-build/libarmdisasm.a
ar rcs .task-build/libarmdisasm.a "${objects[@]}"
cc "${flags[@]}" task_support/baseline.c .task-build/libarmdisasm.a -o .task-build/baseline
cc "${flags[@]}" test.c .task-build/libarmdisasm.a -o .task-build/original-tests
