#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/retime_target.c; fi
mkdir -p .task-build
cc -std=c99 -O0 -g -I. -c task_support/cgltf_impl.c -o .task-build/cgltf_impl.o
if [[ $mode == target ]]; then
  extra=(.task-build/cgltf_impl.o)
  if test -f examples/cgltf_retime.c; then extra+=(examples/cgltf_retime.c); fi
  cc -std=c99 -O0 -g -I. task_support/driver.c test/retime_target.c "${extra[@]}" -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in cubic rotation_step asset invalid; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash checks/run-baseline.sh
cc -std=c99 -O0 -g -I. task_support/roundtrip.c .task-build/cgltf_impl.o -lm -o .task-build/roundtrip
.task-build/roundtrip
