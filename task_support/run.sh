#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/region_cases.c; fi
bash task_support/build.sh "$mode"
if [[ $mode == target ]]; then
  test -s examples/region_preview.c
cc -std=c99 -O2 -UNDEBUG -I. -Iinclude -Ilib -Itask_support/vendor/ogg/include task_support/api_probe.c .task-build/libcodec.a -lm -o .task-build/api_probe
.task-build/api_probe
cc -std=c99 -O2 -UNDEBUG -I. -Iinclude -Ilib -Itask_support/vendor/ogg/include examples/region_preview.c .task-build/libcodec.a -lm -o .task-build/region_preview
rm -f .task-build/preview.yuv
.task-build/region_preview .task-build/preview.yuv
test -s .task-build/preview.yuv
cc -std=c99 -O2 -UNDEBUG -I. -Iinclude -Ilib -Itask_support/vendor/ogg/include -c task_support/driver.c -o .task-build/driver.o
cc -std=c99 -O2 -UNDEBUG -I. -Iinclude -Ilib -Itask_support/vendor/ogg/include -c tests/region_cases.c -o .task-build/cases.o
cc .task-build/driver.o .task-build/cases.o .task-build/libcodec.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in Pixels Dependencies Lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then
 cc -std=c99 -O2 -UNDEBUG -I. -Iinclude -Ilib -Itask_support/vendor/ogg/include task_support/baseline.c .task-build/libcodec.a -lm -o .task-build/baseline
 .task-build/baseline
fi
