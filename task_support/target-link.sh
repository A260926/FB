#!/bin/bash
set -euo pipefail
for file in include/fuzzcover/coverage.hpp src/coverage.cpp examples/coverage_replay.cpp; do test -s "$file"; done
if [[ $(uname -s) == Darwin ]]; then
 export_flag=-Wl,-export_dynamic
 platform_lib=-pthread
else
 export_flag=-rdynamic
 platform_lib=-ldl
fi
c++ -std=c++17 -O0 -I. -Iinclude -include include/fuzzcover/coverage.hpp task_support/api_probe.cpp .task-build/coverage.o .task-build/libcoverage.a "$export_flag" "$platform_lib" -o .task-build/api-probe
.task-build/api-probe
c++ -std=c++17 -O0 -I. -Iinclude examples/coverage_replay.cpp .task-build/coverage.o .task-build/libcoverage.a "$export_flag" "$platform_lib" -o .task-build/coverage_replay
mkdir -p .task-build/replay-output
rm -f .task-build/replay-output/transcript.txt
.task-build/coverage_replay .task-build/replay-output
test -s .task-build/replay-output/transcript.txt
c++ -x c++ -std=c++17 -O0 -I. -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++17 -O0 -I. -Iinclude checks/coverage_tests.cpp .task-build/driver.o .task-build/coverage.o .task-build/libcoverage.a "$export_flag" "$platform_lib" -o .task-build/target
