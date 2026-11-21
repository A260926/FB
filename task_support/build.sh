#!/bin/bash
set -euo pipefail
mkdir -p .task-build/objects
for source in libsrc/*.cc; do
  case "$source" in libsrc/GLCamera.cc|libsrc/GLManager.cc) continue ;; esac
  object=".task-build/objects/$(basename "${source%.cc}").o"
  if [[ ! -f "$object" || "$source" -nt "$object" || task_support/build.sh -nt "$object" ]] || [[ -n "$(find include -type f -newer "$object" -print -quit)" ]]; then
    c++ -std=c++17 -O1 -Iinclude -c "$source" -o "$object"
  fi
done
ar rcs .task-build/libtrimesh.a .task-build/objects/*.o
c++ -std=c++17 -O1 -Iinclude task_support/baseline.cc .task-build/libtrimesh.a -o .task-build/baseline
