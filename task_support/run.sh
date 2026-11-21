#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/cpu_frame.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O2 -pthread -I. task_support/baseline.cpp Octree.cpp Octant.cpp Camera.cpp Options.cpp math.cpp voxeldisplays.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s headers/CPUFrame.h
test -s CPUFrame.cpp
test -s examples/cpu_frame.cpp
c++ -std=c++17 -O2 -pthread -I. -include headers/CPUFrame.h -c CPUFrame.cpp -o .task-build/cpu-frame.o
c++ -std=c++17 -O2 -pthread -I. task_support/driver.c tests/cpu_frame.cpp .task-build/cpu-frame.o Octree.cpp Octant.cpp Camera.cpp Options.cpp math.cpp voxeldisplays.cpp -o .task-build/target
c++ -std=c++17 -O2 -pthread -I. -include headers/CPUFrame.h examples/cpu_frame.cpp .task-build/cpu-frame.o Octree.cpp Octant.cpp Camera.cpp Options.cpp math.cpp voxeldisplays.cpp -o .task-build/example
rm -f .task-build/cpu-frame.ppm
.task-build/example .task-build/cpu-frame.ppm
test -s .task-build/cpu-frame.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in ownership queries frames; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
