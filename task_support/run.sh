#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/shared_pose_target.cpp; fi
sh task_support/build.sh
if [[ $mode == target ]]; then
  test -s examples/shared_pose.cpp
c++ -std=c++23 -O1 -DEELY_DEBUG -Ilibs/eely/include -Iexternal/gsl/include -Iexternal/acl/include -I. task_support/driver.c tests/shared_pose_target.cpp $(cat .task-build/object-list) -o .task-build/target
c++ -std=c++23 -O1 -DEELY_DEBUG -Ilibs/eely/include -Iexternal/gsl/include -Iexternal/acl/include -I. examples/shared_pose.cpp $(cat .task-build/object-list) -o .task-build/shared_pose_example
.task-build/shared_pose_example
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in sharing ownership rollback; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
