#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/surface_correspondence.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O1 -I include -I external/eigen task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s include/cinolib/surface_correspondence.h
test -s include/cinolib/surface_correspondence.cpp
test -s examples/correspondence_lod.cpp
c++ -std=c++17 -O1 -I . -I include -I external/eigen -x c++ task_support/driver.c tests/surface_correspondence.cpp include/cinolib/surface_correspondence.cpp -o .task-build/target
c++ -std=c++17 -O1 -I include -I external/eigen examples/correspondence_lod.cpp include/cinolib/surface_correspondence.cpp -o .task-build/example
example_dir=$(mktemp -d .task-build/example.XXXXXX)
.task-build/example "$example_dir"
for artifact in original.obj level1.obj level2.obj points.csv; do test -s "$example_dir/$artifact"; done
for artifact in original.obj level1.obj level2.obj; do grep -q '^v ' "$example_dir/$artifact"; grep -q '^f ' "$example_dir/$artifact"; done
test "$(wc -l <"$example_dir/points.csv")" -gt 1
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in maps composition compaction rejection; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
