#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/control_variate.cpp; fi
bash task_support/build.sh "$mode"
if [[ $mode == target ]]; then
  c++ -std=c++20 -pthread -I. -Iinclude/luz -include include/luz/Renderer/ControlVariate.hpp task_support/driver.c task_tests/control_variate.cpp .task-build/core/CMakeFiles/luz_core.dir/src/renderer/ControlVariate.cpp.o .task-build/core/libluz_core.a -o .task-build/target
c++ -std=c++20 -pthread -I. -Iinclude/luz examples/control_variate.cpp .task-build/core/CMakeFiles/luz_core.dir/src/renderer/ControlVariate.cpp.o .task-build/core/libluz_core.a -o .task-build/example
rm -f .task-build/control_variate.ppm
.task-build/example .task-build/control_variate.ppm
test -s .task-build/control_variate.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in harmonics transport epochs variance; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
