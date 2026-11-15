#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/test_patch_editor.cpp; fi
make -s -j2 -f task_support/Makefile core
if [[ $mode == target ]]; then
  test -s include/tinynurbs/core/patch_editor.h
test -s src/patch_editor.cpp
test -s src/patch_mesh.cpp
test -s examples/patch_editor.cpp
make -s -f task_support/Makefile target-link
rm -f .task-build/patch_editor.obj
.task-build/patch_editor .task-build/patch_editor.obj
test -s .task-build/patch_editor.obj
grep -q '^v ' .task-build/patch_editor.obj
grep -q '^f ' .task-build/patch_editor.obj
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in continuity projection lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
