#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  test -s src/replay.ts
  test -s checks/replay.test.js
  test -s examples/replay.js
fi
mkdir -p .task-build
export TASK_BUILD
TASK_BUILD=$(mktemp -d "$PWD/.task-build/compiled.XXXXXX")
node node_modules/typescript/lib/tsc.js -p . --outDir "$TASK_BUILD"
if [[ $mode == target ]]; then test -s "$TASK_BUILD/src/replay.js"; fi
output_log=$(mktemp "$PWD/.task-build/run-output.XXXXXX")
set +e
node task_support/run.js "$mode" >"$output_log" 2>&1
run_code=$?
set -e
cat "$output_log"
[[ $run_code == 0 ]] || exit "$run_code"
if [[ $mode == target ]]; then
 for group in stages priming protocol; do
  grep -Eq "^GROUP ${group} checks=[1-9][0-9]* COMPLETE$" "$output_log"
 done
 grep -qx 'FIXED_API_CONSUMER_COMPLETE' "$output_log"
 grep -qx 'TARGET_COMPLETE' "$output_log"
fi
