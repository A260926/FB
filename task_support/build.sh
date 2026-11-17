#!/bin/bash
set -euo pipefail
mkdir -p .task-build
: > .task-build/objects.txt
while IFS= read -r source_file; do
  object_file=".task-build/${source_file%.c}.o"
  mkdir -p "$(dirname "$object_file")"
  printf '%s\n' "$object_file" >> .task-build/objects.txt
  if [[ ! -f "$object_file" || "$source_file" -nt "$object_file" ]] || find src include -name '*.h' -newer "$object_file" -print -quit | grep -q .; then
    cc -std=c99 -O1 -D_POSIX_C_SOURCE=200112L -DSTATICALLY_LINKED=0 -I. -Iinclude -Isrc -Isrc/lib -Isrc/common -pthread -c "$source_file" -o "$object_file"
  fi
done < <(find src/lib src/common -name '*.c' | LC_ALL=C sort)
sed 's/@GIT_SHA1@/1fce01f4f8f06e45f1b82f339eb9ab29ee1258dd/g' generated/gitsha1.c.in > .task-build/gitsha1.c
cc -std=c99 -O1 -c .task-build/gitsha1.c -o .task-build/gitsha1.o
rm -f .task-build/libcray.a
ar rcs .task-build/libcray.a $(cat .task-build/objects.txt) .task-build/gitsha1.o
cc -std=c99 -O1 -D_POSIX_C_SOURCE=200112L -I. -Iinclude -Isrc -Isrc/lib -Isrc/common -pthread task_support/baseline.c .task-build/libcray.a -lm -o .task-build/baseline
