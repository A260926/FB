#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  test -s src/OpenMesh/Tools/Utils/BoundaryBridgeT.hh
  c++ -std=c++11 -DOM_STATIC_BUILD -I. -Isrc -x c++ -fsyntax-only src/OpenMesh/Tools/Utils/BoundaryBridgeT.hh
fi
if [[ $mode == target ]]; then test -s tests/boundary_bridge.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  c++ -DOM_STATIC_BUILD -std=c++11 -O0 -w -I. -Isrc tests/boundary_bridge.cpp task_support/driver.c .task-build/core.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in optimality topology attributes; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
