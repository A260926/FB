#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/obj
find include libsrc -type f -exec cksum {} + >.task-build/current
cksum task_support/build.sh >>.task-build/current
if [[ ! -s .task-build/libtrimesh.a || ! -f .task-build/built ]] || ! cmp -s .task-build/current .task-build/built; then
 rm -f .task-build/obj/*.o .task-build/libtrimesh.a
 for source_file in libsrc/*.cc; do
  case "$source_file" in libsrc/GLCamera.cc|libsrc/GLManager.cc) continue;; esac
  c++ -std=c++11 -O1 -Iinclude -c "$source_file" -o ".task-build/obj/$(basename "${source_file%.cc}").o"
 done
 ar rcs .task-build/libtrimesh.a .task-build/obj/*.o
 cp .task-build/current .task-build/built
fi
c++ -std=c++11 -O1 -Iinclude task_support/baseline.cc .task-build/libtrimesh.a -o .task-build/baseline
