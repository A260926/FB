#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s fuzz/ownership_tests.c; fi
mkdir -p .task-build
cc -std=c11 -D_GNU_SOURCE -O0 -g -I. quickjs.c libregexp.c libunicode.c dtoa.c task_support/baseline.c -lm -lpthread -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s fuzz/ownership_model.c
test -s fuzz/ownership_model.h
test -s fuzz/ownership_replay.c
test -s fuzz/ownership_replay.h
test -s fuzz/ownership_fuzz.c
test -s examples/ownership_demo.c
cc -std=c11 -D_GNU_SOURCE -O0 -g -fsanitize=address,undefined -I. -include fuzz/ownership_model.h -include fuzz/ownership_replay.h quickjs.c libregexp.c libunicode.c dtoa.c fuzz/ownership_model.c fuzz/ownership_replay.c fuzz/ownership_fuzz.c task_support/api_consumer.c -lm -lpthread -o .task-build/api-consumer
.task-build/api-consumer
cc -std=c11 -D_GNU_SOURCE -O0 -g -fsanitize=address,undefined -I. -include fuzz/ownership_model.h -include fuzz/ownership_replay.h quickjs.c libregexp.c libunicode.c dtoa.c fuzz/ownership_model.c fuzz/ownership_replay.c fuzz/ownership_fuzz.c examples/ownership_demo.c -lm -lpthread -o .task-build/demo
demo_log=$(mktemp)
.task-build/demo >"$demo_log"
cat "$demo_log"
grep -q '^OWNERSHIP_DEMO_OK$' "$demo_log"
cc -std=c11 -D_GNU_SOURCE -O0 -g -fsanitize=address,undefined -I. -include fuzz/ownership_model.h -include fuzz/ownership_replay.h quickjs.c libregexp.c libunicode.c dtoa.c fuzz/ownership_model.c fuzz/ownership_replay.c fuzz/ownership_fuzz.c fuzz/ownership_tests.c task_support/driver.c -lm -lpthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in transfer exceptions replay diagnostics; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
