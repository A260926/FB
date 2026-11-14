#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/task_periodic_texture.cpp; fi
mkdir -p .task-build
c++ -std=c++11 -w -Iinclude -Iinclude/noise -Iexamples src/*.cpp src/module/*.cpp src/model/*.cpp examples/noiseutils.cpp task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++11 -w -I. -Iinclude -Iinclude/noise -Iexamples src/*.cpp src/module/*.cpp src/model/*.cpp examples/noiseutils.cpp -x c++ task_support/driver.c tests/task_periodic_texture.cpp -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in periodic pyramid regions; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
