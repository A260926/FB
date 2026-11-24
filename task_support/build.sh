#!/bin/bash
set -euo pipefail
rm -rf .task-build
mkdir -p .task-build
for variant in plain threaded; do
  build_dir=".task-build/$variant"
  mkdir -p "$build_dir"
  cp -R src "$build_dir/src"
  if [[ $variant == threaded ]]; then
    sed 's/#undef ANIM_THREAD_SAFE/#define ANIM_THREAD_SAFE/' src/config.h > "$build_dir/src/config.h"
  fi
  find "$build_dir/src" -type f -name '*.c' | LC_ALL=C sort > "$build_dir/sources.txt"
  test -s "$build_dir/sources.txt"
  while IFS= read -r source_file; do
    test -s "$source_file"
    object_file="$source_file.o"
    cc -std=c99 -D_POSIX_C_SOURCE=200809L -O1 -I"$build_dir/src" -pthread -c "$source_file" -o "$object_file"
  done < "$build_dir/sources.txt"
  find "$build_dir/src" -type f -name '*.c.o' | LC_ALL=C sort > "$build_dir/objects.txt"
  test -s "$build_dir/objects.txt"
  objects=()
  while IFS= read -r object_file; do objects+=("$object_file"); done < "$build_dir/objects.txt"
  ar rcs "$build_dir/libanim.a" "${objects[@]}"
  cc -std=c99 -D_POSIX_C_SOURCE=200809L -O1 -I"$build_dir/src" task_support/baseline.c "$build_dir/libanim.a" -pthread -lm -o "$build_dir/baseline"
done
