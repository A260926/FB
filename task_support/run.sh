#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/test_offset.cpp; fi
make -s -j2 -f task_support/Makefile base
if [[ $mode == target ]]; then
  for required in include/libnurbs/Surface/OffsetFit.hpp include/libnurbs/Algorithm/OffsetBounds.hpp src/Surface/OffsetFit.cpp src/Algorithm/OffsetBounds.cpp examples/offset_obj.cpp; do test -s "$required"; done
make -s -j2 -f task_support/Makefile target
(
set -e
example_dir=$(mktemp -d)
trap 'rm -rf "$example_dir"' EXIT
.task-build/offset-example "$example_dir/offset.obj"
test -s "$example_dir/offset.obj"
awk '/^v /{v++}/^vn /{n++}/^f /{f++} END {exit !(v>0&&n>0&&f>0)}' "$example_dir/offset.obj"
)
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in certificates fitting budgets; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
