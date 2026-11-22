#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/rewind_stream.c; fi
bash task_support/build.sh "$mode"
codec_link="-Wl,--whole-archive .task-build/libcodec.a -Wl,--no-whole-archive"
if [[ $(uname -s) == Darwin ]]; then codec_link="-Wl,-force_load,.task-build/libcodec.a"; fi
if [[ $mode == target ]]; then
  test -s examples/rewind_stream.c
rm -f .task-build/api-consumer .task-build/rewind-demo .task-build/demo.log
cc -std=c99 -O1 -Iinclude -Ilib -Itask_support/include -Itask_support/vendor/ogg/include -I. task_support/fixture.c task_support/api_consumer.c $codec_link -lm -o .task-build/api-consumer
.task-build/api-consumer
cc -std=c99 -O1 -Iinclude -Ilib -Itask_support/include -Itask_support/vendor/ogg/include -I. task_support/fixture.c examples/rewind_stream.c $codec_link -lm -o .task-build/rewind-demo
.task-build/rewind-demo > .task-build/demo.log
cat .task-build/demo.log
grep -qx VORBIS_REWIND_DEMO_OK .task-build/demo.log
cc -std=c99 -O1 -Iinclude -Ilib -Itask_support/include -Itask_support/vendor/ogg/include -I. task_support/fixture.c task_support/driver.c test/rewind_stream.c $codec_link -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in window replay lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc -std=c99 -O1 -Iinclude -Ilib -Itask_support/include -Itask_support/vendor/ogg/include -I. task_support/fixture.c task_support/baseline.c $codec_link -lm -o .task-build/baseline
.task-build/baseline
