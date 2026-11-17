#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/obj
objects=()
for source in src/*.c; do
  [[ "$source" = src/pdbparse.c ]] && continue
  object=".task-build/obj/$(basename "${source%.c}").o"
  if [[ ! -f "$object" || "$source" -nt "$object" ]] || find include src -name '*.h' -newer "$object" -print -quit 2>/dev/null | read -r _; then
    "${CC:-cc}" -std=c11 -D__unix__ -DPDB_ENABLE_ASSERTIONS=1 -O0 -I. -Iinclude -Isrc -c "$source" -o "$object"
  fi
  objects+=("$object")
done
ar rcs .task-build/libpdb.a "${objects[@]}"
"${CC:-cc}" -std=c11 -D__unix__ -DPDB_ENABLE_ASSERTIONS=1 -O0 -I. -Iinclude task_support/baseline.c task_support/image.c .task-build/libpdb.a -o .task-build/baseline
sed -e 's/@PROJECT_NAME@/libpdb/g' -e 's/@PROJECT_VERSION@/0.1.0/g' -e 's/@PROJECT_VERSION_MAJOR@/0/g' -e 's/@PROJECT_VERSION_MINOR@/1/g' -e 's/@PROJECT_VERSION_PATCH@/0/g' src/config.h.in > .task-build/config.h
"${CC:-cc}" -std=c11 -D_GNU_SOURCE -D__unix__ -I. -Iinclude -I.task-build src/pdbparse.c .task-build/libpdb.a -o .task-build/pdbparse
