#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/pool_undo.c; fi
bash task_support/build.sh "$mode"
if [[ $mode == target ]]; then
  test -s core/pool_undo.c
test -s examples/pool_undo.c
cc -std=c99 -Werror=implicit-function-declaration -I. task_support/api_consumer.c core/pool_undo.o liblsmash.a -lm -o .task-build/api-consumer
.task-build/api-consumer
cc -std=c99 -Werror=implicit-function-declaration -I. examples/pool_undo.c core/pool_undo.o liblsmash.a -lm -o .task-build/example
rm -f .task-build/pool-undo-example.mp4
.task-build/example
test -s .task-build/pool-undo-example.mp4
cc -std=c99 -Werror=implicit-function-declaration -I. task_support/check_example.c core/pool_undo.o liblsmash.a -lm -o .task-build/check-example
.task-build/check-example
cc -std=c99 -Werror=implicit-function-declaration -I. task_support/driver.c tests/pool_undo.c core/pool_undo.o liblsmash.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in tables boundaries recovery; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc -std=c99 -Werror=implicit-function-declaration -I. task_support/baseline.c liblsmash.a -lm -o .task-build/baseline
rm -f .task-build/baseline.mp4
.task-build/baseline
