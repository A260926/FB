#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/latm_other_data.c; fi
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_PROGRAMS=OFF > .task-config.log 2>&1
cmake --build .task-build --target fdk-aac --parallel 4 > .task-compile.log 2>&1
cc -std=c99 -O2 -I. -Icpp/libAACenc/include -Icpp/libAACdec/include -Icpp/libSYS/include -c task_support/baseline.c -o .task-build/baseline.o
c++ .task-build/baseline.o .task-build/upstream/libfdk-aac.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -I. -Icpp/libAACenc/include -Icpp/libAACdec/include -Icpp/libSYS/include -Icpp/libMpegTPEnc/include -c tests/latm_other_data.c -o .task-build/contract.o
cc -std=c99 -O2 -Itask_support -c task_support/driver.c -o .task-build/driver.o
c++ .task-build/driver.o .task-build/contract.o .task-build/upstream/libfdk-aac.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in Binding Bitstream Audio; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
