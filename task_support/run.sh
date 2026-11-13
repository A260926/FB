#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/mqtt_property_contract.c; fi
mkdir -p .task-build
cc -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -DMG_ENABLE_LOG=0 -I. -c mongoose.c -o .task-build/mongoose.o
if [[ $mode == target ]]; then
  cc -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I. .task-build/mongoose.o task_support/driver.c test/mqtt_property_contract.c -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in values boundaries framing fuzz; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
sh task_support/baseline.sh
