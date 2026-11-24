#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/adts_seal_cases.cpp; fi
cmake -S . -B .task-build/fdk -DBUILD_SHARED_LIBS=OFF -DBUILD_PROGRAMS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build/fdk --target fdk-aac -j 4
if [[ $mode == target ]]; then
  test -s task_support/require_adts_seal_api.cpp
  c++ -std=c++11 -I cpp/libAACenc/include -I cpp/libSYS/include task_support/require_adts_seal_api.cpp .task-build/fdk/libfdk-aac.a -lm -o .task-build/required-adts-seal-api
  .task-build/required-adts-seal-api
fi
c++ -std=c++11 -I cpp/libAACenc/include -I cpp/libAACdec/include -I cpp/libSYS/include task_support/baseline.cpp .task-build/fdk/libfdk-aac.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s examples/adts_seal_example.cpp
cc -std=c11 -I . -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++11 -include task_support/cxx_bridge.h -I . -I cpp/libAACenc/include -I cpp/libAACdec/include -I cpp/libSYS/include tests/adts_seal_cases.cpp .task-build/driver.o .task-build/fdk/libfdk-aac.a -lm -o .task-build/target
c++ -std=c++11 -I cpp/libAACenc/include -I cpp/libAACdec/include -I cpp/libSYS/include examples/adts_seal_example.cpp .task-build/fdk/libfdk-aac.a -lm -o .task-build/adts-seal-example
rm -f .task-build/adts-seal-example.aac
.task-build/adts-seal-example .task-build/adts-seal-example.aac
test -s .task-build/adts-seal-example.aac
  output_log=$(mktemp .task-build/target-output.XXXXXX)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in framing transaction reservoir audio; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
