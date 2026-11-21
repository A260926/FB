#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/distance_mis.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s renderer.h
test -s volume_sampling.h
test -s volume_sampling.cpp
test -s examples/volume_beam.cpp
rm -f .task-build/target .task-build/renderer.o .task-build/volume.o .task-build/example
c++ -std=c++17 -O2 -I. -include renderer.h -include volume_sampling.h -Dmain=task_original_main -c tinyraytracer.cpp -o .task-build/renderer.o
c++ -std=c++17 -O2 -I. -include renderer.h -include volume_sampling.h -c volume_sampling.cpp -o .task-build/volume.o
c++ -std=c++17 -O2 -I. -include renderer.h -include volume_sampling.h tests/distance_mis.cpp -x c++ task_support/driver.c -x none .task-build/renderer.o .task-build/volume.o -o .task-build/target
c++ -std=c++17 -O2 -I. -include renderer.h -include volume_sampling.h examples/volume_beam.cpp .task-build/renderer.o .task-build/volume.o -o .task-build/example
mkdir -p .task-build/beam-example
rm -f .task-build/beam-example/beam.ppm
.task-build/example .task-build/beam-example
test -s .task-build/beam-example/beam.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in segments density transport; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
