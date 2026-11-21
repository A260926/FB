#!/bin/bash
set -euo pipefail
mkdir -p .task-build/obj
objects=()
for source in src/*.cpp; do
  if [[ $source == src/main.cpp ]]; then continue; fi
  test -s "$source"
  object=".task-build/obj/$(basename "${source%.cpp}").o"
  if [[ ! -s "$object" || "$source" -nt "$object" ]] || [[ -n "$(find include -type f -newer "$object" -print -quit 2>/dev/null)" ]]; then
    c++ -std=c++17 -O0 -pthread -Iinclude -I. -c "$source" -o "$object"
  fi
  objects+=("$object")
done
test "${#objects[@]}" -gt 0
test -s .task-build/obj/Camera.o
printf '%s\n' "${objects[@]}" > .task-build/objects.list
c++ -std=c++17 -O0 -pthread -Iinclude -I. task_support/baseline.cpp "${objects[@]}" -o .task-build/baseline
