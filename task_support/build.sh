#!/bin/bash
set -euo pipefail
mkdir -p .task-build
find src.cmd -maxdepth 1 -name '*.cpp' ! -name Main.cpp -print > .task-build/production-sources.txt
while IFS= read -r source_file; do
  c++ -std=c++11 -O2 -I. -c "$source_file" -o ".task-build/production-$(basename "$source_file").o"
done < .task-build/production-sources.txt
set -- .task-build/production-*.o
if [[ ! -f $1 ]]; then set --; fi
c++ -x c++ -std=c++11 -I. -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++11 -O2 -I. task_support/baseline.cpp "$@" -o .task-build/baseline
c++ -std=c++11 -O2 -I. src.cmd/Main.cpp "$@" -o .task-build/simplify
