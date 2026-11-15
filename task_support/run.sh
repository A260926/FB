#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/asc_blocks.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O2 -I GTE task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s GTE/Mathematics/ASCBlockMesh.h
test -s GTE/Mathematics/ASCBlockMesh.cpp
test -s examples/asc_blocks.cpp
c++ -std=c++17 -O2 -I . -I GTE -x c++ task_support/driver.c tests/asc_blocks.cpp GTE/Mathematics/ASCBlockMesh.cpp -o .task-build/target
c++ -std=c++17 -O2 -I GTE examples/asc_blocks.cpp GTE/Mathematics/ASCBlockMesh.cpp -o .task-build/example
example_dir=$(mktemp -d .task-build/example.XXXXXX)
.task-build/example "$example_dir/blocks.obj"
test -s "$example_dir/blocks.obj"
grep -q '^v ' "$example_dir/blocks.obj"
grep -q '^f ' "$example_dir/blocks.obj"
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in faces junctions adaptivity compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline --legacy-root; else .task-build/baseline; fi
