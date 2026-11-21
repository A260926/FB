#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/vertex_cm.cpp; fi
bash task_support/build.sh "$mode"
if [[ $mode == target ]]; then
  vcm_object=$(find .task-build/core/CMakeFiles/task_core.dir -name vertex-cm.cpp.o -type f)
test -s "$vcm_object"
c++ -std=c++20 -pthread -I. -Ilib/glm -Ilib/nlohmann -include source/integrator/vertex-cm/vertex-cm.hpp task_support/driver.c task_tests/vertex_cm.cpp "$vcm_object" .task-build/core/libtask_core.a -o .task-build/target
c++ -std=c++20 -pthread -I. -Ilib/glm -Ilib/nlohmann examples/vertex_cm.cpp "$vcm_object" .task-build/core/libtask_core.a -o .task-build/example
rm -f .task-build/vertex_cm.ppm
.task-build/example .task-build/vertex_cm.ppm
test -s .task-build/vertex_cm.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in densities strategies merging rendering; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
