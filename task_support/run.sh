#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/task_timecode.cpp; fi
mkdir -p .task-build
if [[ $mode == target ]]; then
  test -s src/mp4timecode.cpp
  test -s include/mp4v2/timecode.h
  test -s examples/timecode.cpp
  task_libdir=.task-build/target-lib/production
  cmake -S task_support -B .task-build/target-lib -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  cmake --build .task-build/target-lib --target mp4v2 -j4
else
  task_libdir=.task-build/base-lib
  cmake -S . -B "$task_libdir" -DBUILD_SHARED=OFF -DBUILD_UTILS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  cmake --build "$task_libdir" --target mp4v2 -j4
fi
c++ -std=c++11 task_support/fixture.cpp -o .task-build/fixture
rm -f .task-build/simple.mov
.task-build/fixture .task-build/simple.mov
test -s .task-build/simple.mov
c++ -std=c++11 -Iinclude -I"$task_libdir/include" task_support/baseline.cpp "$task_libdir/libmp4v2.a" -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++11 -Iinclude -I"$task_libdir/include" task_support/api_probe.cpp "$task_libdir/libmp4v2.a" -o .task-build/api-probe
.task-build/api-probe
c++ -std=c++11 -Iinclude -I"$task_libdir/include" examples/timecode.cpp "$task_libdir/libmp4v2.a" -o .task-build/timecode
rm -f .task-build/timecode.txt
.task-build/timecode .task-build/simple.mov 1 80 > .task-build/timecode.txt
test -s .task-build/timecode.txt
grep -F '01:00:00:02' .task-build/timecode.txt
c++ -std=c++11 -I. -Iinclude -I"$task_libdir/include" task_support/driver.c tests/task_timecode.cpp "$task_libdir/libmp4v2.a" -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in association projection counter reject; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == target ]]; then .task-build/baseline .task-build/simple.mov normal; else .task-build/baseline .task-build/simple.mov; fi
