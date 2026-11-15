#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build
if [[ $mode == target ]]; then
 test -s src/render/SceneInstanceCollector.js
 test -s tests/scene-instance-batches.test.mjs
 test -s examples/headless-scene-batches.mjs
 node --no-warnings --loader ./task_support/glsl-loader.mjs task_support/bindings.mjs
 rm -f .task-build/scene-batches.json
 node --no-warnings --loader ./task_support/glsl-loader.mjs examples/headless-scene-batches.mjs .task-build/scene-batches.json
 test -s .task-build/scene-batches.json
 output_log=$(mktemp)
 trap 'rm -f "$output_log"' EXIT
 set +e
 node --no-warnings --loader ./task_support/glsl-loader.mjs task_support/driver.mjs > "$output_log" 2>&1
 status=$?
 set -e
 cat "$output_log"
 [[ $status == 0 ]] || exit "$status"
 for group in membership submission integration; do
  grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
 done
 grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
elif [[ $mode != baseline ]]; then
 exit 2
fi
node --no-warnings --loader ./task_support/glsl-loader.mjs task_support/baseline.mjs
node --no-warnings --loader ./task_support/glsl-loader.mjs task_support/native-draw-baseline.mjs
