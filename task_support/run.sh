#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/tone_target.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O2 -I. task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s tone_map.h
test -s tone_map.cpp
test -s examples/tone_map.cpp
c++ -std=c++17 -O2 -I. -include tone_map.h -c tone_map.cpp -o .task-build/tone_map.o
c++ -std=c++17 -O2 -I. task_support/consumer.cpp .task-build/tone_map.o -o .task-build/consumer
.task-build/consumer
c++ -std=c++17 -O2 -I. examples/tone_map.cpp .task-build/tone_map.o -o .task-build/example
c++ -std=c++17 -O2 task_support/check_images.cpp -o .task-build/check_images
rm -f .task-build/capture.ppm .task-build/compressed.ppm
.task-build/example .task-build
.task-build/check_images .task-build/capture.ppm .task-build/compressed.ppm
c++ -std=c++17 -O2 -I. task_support/driver.c tests/tone_target.cpp .task-build/tone_map.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in capture pyramid poisson; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
