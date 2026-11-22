#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/bounded_test.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O2 -Itask_support -Iinclude -Isource/SoundTouch -c task_support/baseline.cpp -o .task-build/baseline.o
sources=(source/SoundTouch/AAFilter.cpp source/SoundTouch/FIFOSampleBuffer.cpp source/SoundTouch/FIRFilter.cpp source/SoundTouch/InterpolateCubic.cpp source/SoundTouch/InterpolateLinear.cpp source/SoundTouch/InterpolateShannon.cpp source/SoundTouch/RateTransposer.cpp source/SoundTouch/SoundTouch.cpp source/SoundTouch/TDStretch.cpp source/SoundTouch/cpu_detect_x86.cpp)
if [[ $mode == target ]]; then
  test -s source/SoundTouch/BoundedPipeline.cpp
  test -s source/SoundTouch/BoundedPipeline.h
  test -s examples/bounded.cpp
  sources+=(source/SoundTouch/BoundedPipeline.cpp)
  c++ -std=c++17 -O2 -Itask_support -Iinclude -Isource/SoundTouch -include source/SoundTouch/BoundedPipeline.h -c task_support/header_consumer.cpp -o .task-build/header_consumer.o
fi
objects=()
for source in "${sources[@]}"; do
  object=.task-build/$(basename "$source" .cpp).o
  c++ -std=c++17 -O2 -Itask_support -Iinclude -Isource/SoundTouch -c "$source" -o "$object"
  objects+=("$object")
done
ar rcs .task-build/libtask.a "${objects[@]}"
c++ .task-build/baseline.o .task-build/libtask.a -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -O2 -I. -Itask_support -Iinclude -Isource/SoundTouch tests/bounded_test.cpp task_support/driver.c .task-build/header_consumer.o .task-build/BoundedPipeline.o .task-build/libtask.a -o .task-build/target
c++ -std=c++17 -O2 -Itask_support -Iinclude -Isource/SoundTouch task_support/api_consumer.cpp .task-build/BoundedPipeline.o .task-build/libtask.a -o .task-build/api_consumer
.task-build/api_consumer
c++ -std=c++17 -O2 -Itask_support -Iinclude -Isource/SoundTouch examples/bounded.cpp .task-build/BoundedPipeline.o .task-build/libtask.a -o .task-build/example
rm -f .task-build/bounded.pcm
.task-build/example
test -s .task-build/bounded.pcm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in pcm pressure finish lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
