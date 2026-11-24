#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_support/render_index_test.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O1 -I. -DLACK_SINCOS -DALREADY_CONFIGURED -DUSE_BUILTIN_FFT -DUSE_BQRESAMPLER -DNO_THREADING -DNO_THREAD_CHECKS -DNO_TIMING -DRUBBERBAND_TASK_FFT_OBSERVE -c single/RubberBandSingle.cpp -o .task-build/rubberband.o
c++ -std=c++17 -O1 -I. -DLACK_SINCOS -DALREADY_CONFIGURED -DUSE_BUILTIN_FFT -DUSE_BQRESAMPLER -DNO_THREADING -DNO_THREAD_CHECKS -DNO_TIMING -DRUBBERBAND_TASK_FFT_OBSERVE -c task_support/fft_observer.cpp -o .task-build/observer.o
c++ -std=c++17 -x c++ -I. -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++17 -O1 -I. -DLACK_SINCOS -DALREADY_CONFIGURED -DUSE_BUILTIN_FFT -DUSE_BQRESAMPLER -DNO_THREADING -DNO_THREAD_CHECKS -DNO_TIMING -DRUBBERBAND_TASK_FFT_OBSERVE task_support/baseline.cpp .task-build/rubberband.o .task-build/observer.o -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s rubberband/R2RenderIndex.h
test -s src/faster/R2RenderIndex.cpp
c++ -std=c++17 -O1 -I. -DLACK_SINCOS -DALREADY_CONFIGURED -DUSE_BUILTIN_FFT -DUSE_BQRESAMPLER -DNO_THREADING -DNO_THREAD_CHECKS -DNO_TIMING -DRUBBERBAND_TASK_FFT_OBSERVE -include rubberband/R2RenderIndex.h -c src/faster/R2RenderIndex.cpp -o .task-build/render-index.o
c++ -std=c++17 -O1 -I. -DLACK_SINCOS -DALREADY_CONFIGURED -DUSE_BUILTIN_FFT -DUSE_BQRESAMPLER -DNO_THREADING -DNO_THREAD_CHECKS -DNO_TIMING -DRUBBERBAND_TASK_FFT_OBSERVE task_support/api_probe.cpp .task-build/rubberband.o .task-build/render-index.o .task-build/observer.o -o .task-build/api-probe
.task-build/api-probe
test -s examples/partition_render.cpp
c++ -std=c++17 -O1 -I. -DLACK_SINCOS -DALREADY_CONFIGURED -DUSE_BUILTIN_FFT -DUSE_BQRESAMPLER -DNO_THREADING -DNO_THREAD_CHECKS -DNO_TIMING -DRUBBERBAND_TASK_FFT_OBSERVE examples/partition_render.cpp .task-build/rubberband.o .task-build/render-index.o .task-build/observer.o -o .task-build/partition-render
c++ -std=c++17 -O1 task_support/check_partition.cpp -o .task-build/check-partition
mkdir -p .task-build/render-output
rm -f .task-build/render-output/reference-mix.f32 .task-build/render-output/p0.f32 .task-build/render-output/p8000.f32 .task-build/render-output/p16000.f32 .task-build/render-output/p24000.f32
.task-build/partition-render prepare .task-build/render-output
test -s .task-build/render-output/reference-mix.f32
.task-build/partition-render worker .task-build/render-output 16000 8000 .task-build/render-output/p16000.f32
.task-build/partition-render worker .task-build/render-output 0 8000 .task-build/render-output/p0.f32
.task-build/partition-render worker .task-build/render-output 8000 8000 .task-build/render-output/p8000.f32
.task-build/partition-render worker .task-build/render-output 24000 6000 .task-build/render-output/p24000.f32
.task-build/check-partition .task-build/render-output
c++ -std=c++17 -O1 -I. -DLACK_SINCOS -DALREADY_CONFIGURED -DUSE_BUILTIN_FFT -DUSE_BQRESAMPLER -DNO_THREADING -DNO_THREAD_CHECKS -DNO_TIMING -DRUBBERBAND_TASK_FFT_OBSERVE -pthread task_support/render_index_test.cpp .task-build/driver.o .task-build/rubberband.o .task-build/render-index.o .task-build/observer.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in phase ranges process; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
