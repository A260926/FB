#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/solid_difference_target.cpp; fi
mkdir -p .task-build
c++ -std=c++11 -O0 -g -I. -c trimesh.cpp -o .task-build/trimesh.o
if [[ $mode == target ]]; then
  c++ -std=c++11 -O0 -g -I. task_support/driver.c tests/solid_difference_target.cpp solid_difference.cpp .task-build/trimesh.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in intersections solids provenance validation; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
c++ -std=c++11 -O0 -g -I. example.cpp .task-build/trimesh.o -o .task-build/example
.task-build/example > .task-build/example-output.log
grep -q "neighbors of vertex 0:" .task-build/example-output.log
echo HALFEDGE_OLD_EXAMPLE_OK
c++ -std=c++11 -O0 -g -I. task_support/baseline.cpp .task-build/trimesh.o -o .task-build/baseline
.task-build/baseline
