#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/test_parameter_domain.cpp; fi
make -s -j2 -f task_support/Makefile core
if [[ $mode == target ]]; then
  test -s include/libnurbs/Surface/SurfaceMesh.hpp
test -s src/Surface/SurfaceMesh.cpp
test -s examples/domain_mesh.cpp
make -s -f task_support/Makefile target-link
rm -f .task-build/domain_mesh.obj
.task-build/domain_mesh .task-build/domain_mesh.obj
test -s .task-build/domain_mesh.obj
grep -q '^v ' .task-build/domain_mesh.obj
grep -q '^f ' .task-build/domain_mesh.obj
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in domain editing mesh; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
