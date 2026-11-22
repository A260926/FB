#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/record_preroll.c; fi
mkdir -p .task-build
if [[ $mode == target ]]; then
  test -s src/SDL_mixer_recording.c
  test -s examples/record_preroll.c
  flavor=target-build
  enabled=ON
else
  flavor=base
  enabled=OFF
fi
cmake -S task_support -B ".task-build/$flavor" -DCMAKE_BUILD_TYPE=Release -DTASK_TARGET="$enabled" >.task-build/configure.log 2>&1 || { tail -30 .task-build/configure.log; exit 1; }
if [[ $mode == target ]]; then
  find ".task-build/$flavor" -type f \( -name 'record_preroll.c.o' -o -name 'SDL_mixer_recording.c.o' \) -delete
fi
cmake --build ".task-build/$flavor" --parallel 4 >.task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
if [[ $mode == target ]]; then
  cp .task-build/target-build/target .task-build/target
rm -f .task-build/recording.wav
.task-build/target-build/record_demo .task-build/recording.wav >.task-build/demo.log
grep -qx PREROLL_DEMO_OK .task-build/demo.log
test -s .task-build/recording.wav
cc -std=c11 -O2 task_support/wavecheck.c -o .task-build/wavecheck
.task-build/wavecheck .task-build/recording.wav
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in history overrun lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
".task-build/$flavor/baseline"
