#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/async_pose.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O1 iqm.cpp -o .task-build/iqm
new_sources=$(find demo -maxdepth 1 -name '*.cpp' ! -name demo.cpp ! -name gpu-demo.cpp ! -name texture.cpp -print)
c++ -std=c++17 -O1 -pthread -DIQM_HEADLESS -I. -Idemo task_support/baseline.cpp $new_sources -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s demo/async_pose.h
test -s demo/async_pose.cpp
test -s example/async_pose.cpp
c++ -std=c++17 -I. -c task_support/driver.c -o .task-build/driver.o
new_sources=$(find demo -maxdepth 1 -name '*.cpp' ! -name demo.cpp ! -name gpu-demo.cpp ! -name texture.cpp -print)
test -n "$new_sources"
c++ -std=c++17 -O1 -pthread -DIQM_HEADLESS -I. -Idemo -include demo/async_pose.h $new_sources tests/async_pose.cpp .task-build/driver.o -o .task-build/target
c++ -std=c++17 -O1 -pthread -DIQM_HEADLESS -I. -Idemo $new_sources example/async_pose.cpp -o .task-build/async-example
.task-build/async-example
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in numerics publication lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
rm -f .task-build/normal.iqm
.task-build/iqm .task-build/normal.iqm task_support/normal.iqe
.task-build/baseline .task-build/normal.iqm
