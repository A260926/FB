#!/bin/bash
set -euo pipefail
mkdir -p .task-build
rm -f .task-build/*.o .task-build/libcoverage.a .task-build/baseline .task-build/api-probe .task-build/target .task-build/coverage_replay
c++ -std=c++17 -O0 -Iinclude task_support/baseline.cpp -o .task-build/baseline
if [[ ${1:-baseline} == target ]]; then
 test -s include/fuzzcover/coverage.hpp
 test -s src/coverage.cpp
 c++ -std=c++17 -O0 -Iinclude -include include/fuzzcover/coverage.hpp -c src/coverage.cpp -o .task-build/coverage.o
 ar rcs .task-build/libcoverage.a .task-build/coverage.o
 mkdir -p .task-build/mod-a .task-build/mod-b
 rm -f .task-build/mod-a/probe.so .task-build/mod-b/probe.so
 if [[ $(uname -s) == Darwin ]]; then
  clang++ -std=c++17 -O0 -fPIC -dynamiclib -Wl,-undefined,dynamic_lookup -fsanitize-coverage=trace-pc-guard -DVARIANT=1 task_support/covered_fixture.cpp -o .task-build/mod-a/probe.so
  clang++ -std=c++17 -O0 -fPIC -dynamiclib -Wl,-undefined,dynamic_lookup -fsanitize-coverage=trace-pc-guard -DVARIANT=2 task_support/covered_fixture.cpp -o .task-build/mod-b/probe.so
 else
  clang++ -std=c++17 -O0 -fPIC -shared -fsanitize-coverage=trace-pc-guard -DVARIANT=1 task_support/covered_fixture.cpp -o .task-build/mod-a/probe.so
  clang++ -std=c++17 -O0 -fPIC -shared -fsanitize-coverage=trace-pc-guard -DVARIANT=2 task_support/covered_fixture.cpp -o .task-build/mod-b/probe.so
 fi
fi
