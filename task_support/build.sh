#!/bin/bash
set -euo pipefail
mkdir -p .task-build/obj
: > .task-build/objects.list
find src -name '*.cpp' ! -name main.cpp | sort > .task-build/sources.list
while IFS= read -r src; do
 obj=".task-build/obj/${src//\//_}.o"
 if [[ ! -f "$obj" || "$src" -nt "$obj" ]] || find include -type f -newer "$obj" -print -quit 2>/dev/null | grep -q .; then
  c++ -std=c++20 -O0 -pthread -Iinclude/luz -I. -c "$src" -o "$obj"
 fi
 printf '%s\n' "$obj" >> .task-build/objects.list
done < .task-build/sources.list
if [[ ! -s .task-build/objects.list ]]; then exit 3; fi
c++ -std=c++20 -O0 -pthread -Iinclude/luz -I. task_support/baseline.cpp $(cat .task-build/objects.list) -o .task-build/baseline
