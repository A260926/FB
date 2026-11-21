#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/hidden_lines.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O2 -I. -Isr task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s sr/PreparedMesh.hpp
test -s sr/HiddenLines.hpp
test -s examples/hidden_lines.cpp
c++ -std=c++17 -O2 -I. -Isr -include sr/PreparedMesh.hpp -include sr/HiddenLines.hpp task_support/driver.c tests/hidden_lines.cpp -o .task-build/target
c++ -std=c++17 -O2 -I. -Isr -include sr/PreparedMesh.hpp -include sr/HiddenLines.hpp examples/hidden_lines.cpp -o .task-build/example
example_dir=$(mktemp -d)
.task-build/example "$example_dir"
test -s "$example_dir/faces.ppm"
test -s "$example_dir/lines.svg"
rm -rf "$example_dir"
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in geometry visibility reuse; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
