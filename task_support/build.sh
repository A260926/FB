#!/bin/bash
set -euo pipefail
mkdir -p .task-build
rebuild=0
if [[ ! -s .task-build/libfujiyama.a || ! -f .task-build/compile.stamp ]]; then
  rebuild=1
elif [[ -n "$(find src -type f -newer .task-build/compile.stamp -print -quit)" ]]; then
  rebuild=1
fi
if [[ $rebuild == 1 ]]; then
  shopt -s nullglob
  objects=()
  for source in src/*.cc src/*.cpp; do
    object=".task-build/$(basename "${source%.*}").o"
    c++ -std=c++17 -O1 -pthread -I. -Isrc -c "$source" -o "$object"
    objects+=("$object")
  done
  test -s .task-build/fj_volume.o
  rm -f .task-build/libfujiyama.a
  ar rcs .task-build/libfujiyama.a "${objects[@]}"
  touch .task-build/compile.stamp
fi
if [[ $mode == baseline ]]; then
  c++ -std=c++17 -O1 -pthread -I. -Isrc task_support/baseline.cpp .task-build/libfujiyama.a -o .task-build/baseline
fi
