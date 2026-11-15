#!/bin/bash
set -euo pipefail
mkdir -p .task-build
: > .task-build/objects.rsp
# Original production units, excluding only the SDL window/event/barrier units.
# Newly added src/*.cpp and nested .cpp files join this same compilation/link.
while IFS= read -r file; do
  case "$file" in src/Main.cpp|src/Events.cpp|src/ThreadBarrier.cpp) continue;; esac
  object=.task-build/$(echo "$file" | tr '/.' '__').o
  c++ -std=c++14 -O2 -pthread -Isrc -I. -c "$file" -o "$object"
  printf '%s\n' "$object" >> .task-build/objects.rsp
done < <(find src -type f -name '*.cpp' | sort)
for file in src/third-party/lz4.c src/third-party/tribox3.c src/third-party/plyfile.c; do
  object=.task-build/$(echo "$file" | tr '/.' '__').o
  cc -std=c99 -O2 -Isrc -c "$file" -o "$object"
  printf '%s\n' "$object" >> .task-build/objects.rsp
done
test -s .task-build/objects.rsp
c++ -std=c++14 -O2 -pthread -Isrc -I. task_support/baseline.cpp @.task-build/objects.rsp -o .task-build/baseline
