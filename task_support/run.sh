#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/path_reuse.cpp; fi
if [[ $mode == target ]]; then
 test -s src/PathReuse.hpp
 test -s src/PathReuse.cpp
 test -s examples/reuse_room.cpp
fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  c++ -std=c++17 -Isrc -I. -include src/PathReuse.hpp task_support/driver.c tests/path_reuse.cpp src/PathReuse.cpp .task-build/libproduction.a -o .task-build/target
c++ -std=c++17 -Isrc -I. examples/reuse_room.cpp src/PathReuse.cpp .task-build/libproduction.a -o .task-build/reuse-example
mkdir -p .task-build/reuse-output
rm -f .task-build/reuse-output/reuse.ppm .task-build/reuse-output/linear.txt .task-build/reuse-output/stats.txt
.task-build/reuse-example .task-build/reuse-output
for artifact in reuse.ppm linear.txt stats.txt; do test -s ".task-build/reuse-output/$artifact"; done
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in measure transport passes; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
