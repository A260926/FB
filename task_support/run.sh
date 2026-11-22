#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/sinc_voice.c; fi
mkdir -p .task-build
cmake -S . -B .task-build/lib -DBUILD_SHARED=OFF -DBUILD_STATIC=ON -DBUILD_LITE=OFF -DLIBXMP_DOCS=OFF -DWITH_UNIT_TESTS=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build .task-build/lib --target xmp_static -j 4
cc -std=c99 -O2 -Iinclude task_support/baseline.c .task-build/lib/libxmp.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/mix_sinc.c
test -s src/mix_sinc.h
grep -F '/src/mix_sinc.c' .task-build/lib/compile_commands.json >/dev/null
cc -std=c99 -O2 -I. -Isrc -Iinclude -Itask_support -include src/mix_sinc.h tests/sinc_voice.c task_support/driver.c task_support/sinc_binding.c .task-build/lib/libxmp.a -lm -o .task-build/target
test -s examples/sinc_voice.c
cc -std=c99 -O2 -I. -Isrc -Iinclude -include src/mix_sinc.h examples/sinc_voice.c task_support/sinc_binding.c .task-build/lib/libxmp.a -lm -o .task-build/example
.task-build/example
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in kernel trajectory lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
