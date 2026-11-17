#!/bin/bash
set -euo pipefail
mkdir -p .task-build
objects=()
while IFS= read -r src; do
  obj=".task-build/${src//\//_}.o"
  cc -std=gnu11 -O1 -I. -Ilibs/libsacd -Ilibs/libcommon -Ilibs/libdstdec -Ilibs/libid3 -c "$src" -o "$obj"
  objects+=("$obj")
done < <(find libs -name '*.c' -type f | LC_ALL=C sort)
ar rcs .task-build/production.a "${objects[@]}"
extra=()
if [[ $(uname) == Darwin ]]; then extra+=(-liconv); fi
cc -std=gnu11 -O1 -I. -Ilibs/libsacd -Ilibs/libcommon -Ilibs/libdstdec -Ilibs/libid3 task_support/baseline.c .task-build/production.a "${extra[@]}" -o .task-build/baseline
