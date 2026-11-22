#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s libmikmod/tests/retrigger_checks.c; fi
mkdir -p .task-build
cmake -S libmikmod -B .task-build/lib -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DENABLE_COREAUDIO=OFF -DENABLE_ALSA=OFF -DENABLE_PULSE=OFF -DENABLE_OSS=OFF -DENABLE_THREADS=OFF -DENABLE_DL=OFF -DENABLE_SNDIO=OFF -DENABLE_STDOUT=OFF -DENABLE_PIPE=OFF -DENABLE_AIFF=OFF -DENABLE_WAV=OFF -DENABLE_RAW=OFF -DENABLE_SIMD=OFF -DCMAKE_BUILD_TYPE=Release > .task-build/configure.log 2>&1
cmake --build .task-build/lib --target mikmod-static --parallel 2 > .task-build/build.log 2>&1
cc -std=c99 -Ilibmikmod/include -I.task-build/lib -Itask_support task_support/baseline.c .task-build/lib/libmikmod.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -Werror=implicit-function-declaration -Ilibmikmod/include -I.task-build/lib -I. task_support/api_consumer.c .task-build/lib/libmikmod.a -lm -o .task-build/api_consumer
.task-build/api_consumer
cc -std=c99 -Ilibmikmod/include -I.task-build/lib -I. libmikmod/tests/retrigger_checks.c task_support/driver.c .task-build/lib/libmikmod.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in waveform ownership lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
