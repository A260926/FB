#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/precise_target.cpp; fi
mkdir -p .task-build
cmake -S . -B .task-build/optimizer -DBUILD_TESTING=OFF -DGLSLOPT_BUILD_TOOLS=OFF -DGLSLOPT_TEST_OPENGL=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER=
cmake --build .task-build/optimizer --target glsl_optimizer --parallel 4
${CXX:-c++} -std=c++17 -O1 -Isrc/glsl task_support/baseline.cpp .task-build/optimizer/libglsl_optimizer.a -pthread -o .task-build/baseline
if [[ $mode == target ]]; then
  ${CXX:-c++} -std=c++17 -O1 -I. -Isrc/glsl -Iinclude -Isrc -Isrc/mesa -Isrc/gallium/include -Isrc/compiler -Isrc/compiler/glsl tests/precise_target.cpp task_support/inspect.cpp task_support/driver.c .task-build/optimizer/libglsl_optimizer.a -pthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in propagation emission compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
