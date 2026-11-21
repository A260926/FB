#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/material_replay.cpp; fi
if [[ $mode == target ]]; then
 test -s src/integrators/material_replay.h
 test -s src/integrators/material_replay.cpp
 test -s examples/material_fit.cpp
fi
case $(uname -s) in Darwin) export TASK_LINK_FLAGS=-Wl,-dead_strip ;; *) export TASK_LINK_FLAGS=-Wl,--gc-sections ;; esac
bash task_support/build.sh
if [[ $mode == target ]]; then
  c++ -std=c++11 -DNDEBUG -DPBRT_PROBES_NONE -Isrc -Isrc/core -I. -include src/integrators/material_replay.h task_support/driver.c task_tests/material_replay.cpp src/integrators/material_replay.cpp .task-build/libproduction.a $TASK_LINK_FLAGS -pthread -o .task-build/target
c++ -std=c++11 -DNDEBUG -DPBRT_PROBES_NONE -Isrc -Isrc/core -I. examples/material_fit.cpp src/integrators/material_replay.cpp .task-build/libproduction.a $TASK_LINK_FLAGS -pthread -o .task-build/material-example
mkdir -p .task-build/material-output
rm -f .task-build/material-output/fit.ppm .task-build/material-output/loss.txt .task-build/material-output/parameters.txt
.task-build/material-example .task-build/material-output
for artifact in fit.ppm loss.txt parameters.txt; do test -s ".task-build/material-output/$artifact"; done
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in primal gradient ownership; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
