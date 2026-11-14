#!/bin/bash
set -euo pipefail
mkdir -p .task-build
rm -f .task-build/libwebvtt.a
objects=()
for source_file in src/libwebvtt/*.c; do
  object_file=.task-build/$(basename "${source_file%.c}").o
  cc -std=c99 -DWEBVTT_STATIC=1 -Iinclude -Isrc/libwebvtt -c "$source_file" -o "$object_file"
  objects+=("$object_file")
done
ar rcs .task-build/libwebvtt.a "${objects[@]}"
