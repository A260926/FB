#!/bin/bash
set -euo pipefail
mkdir -p .task-build
flags=(-std=c++11 -O0 -g -pthread -I. -Isrc -Isrc/net -Isrc/xop -Isrc/3rdpart)
{
 "${CXX:-c++}" --version
 cksum task_support/build.sh
 while IFS= read -r path; do cksum "$path"; done < <(find src -type f | LC_ALL=C sort)
} > .task-build/input-now
if ! cmp -s .task-build/input-now .task-build/input-built; then
 rm -rf .task-build/obj-new
 mkdir -p .task-build/obj-new
 for source in src/net/*.cpp src/xop/*.cpp; do
  object=.task-build/obj-new/$(echo "$source" | tr / _).o
  "${CXX:-c++}" "${flags[@]}" -c "$source" -o "$object"
 done
 rm -rf .task-build/obj
 mv .task-build/obj-new .task-build/obj
 cp .task-build/input-now .task-build/input-built
fi
"${CXX:-c++}" "${flags[@]}" task_support/baseline.cpp .task-build/obj/*.o -o .task-build/baseline
if [[ ${1:-baseline} == target ]]; then
 "${CXX:-c++}" "${flags[@]}" -x c++ -c task_support/driver.c -o .task-build/driver.o
 "${CXX:-c++}" "${flags[@]}" tests/timed_replay.cpp .task-build/driver.o .task-build/obj/*.o -o .task-build/target
fi
