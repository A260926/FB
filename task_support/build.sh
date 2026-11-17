#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/obj
for source in src/*.c; do
  object=".task-build/obj/$(basename "${source%.c}").o"
  if [[ ! -f "$object" || "$source" -nt "$object" ]] || find include src -name '*.h' -newer "$object" -print -quit 2>/dev/null | read -r _; then
    "${CC:-cc}" -std=c99 -O0 -I. -Iinclude -Isrc -c "$source" -o "$object"
  fi
done
ar rcs .task-build/libfcml.a .task-build/obj/*.o
"${CC:-cc}" -std=c99 -O0 -I. -Iinclude -Isrc task_support/baseline.c .task-build/libfcml.a -o .task-build/baseline
