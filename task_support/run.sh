#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == baseline ]]; then
    exec node task_support/baseline.mjs
fi
[[ $mode == target ]] || exit 2
test -s src/scene/instanced-skin.js
test -s test/scene/instanced-skin.target.mjs
test -s examples/instanced-skin.mjs
node task_support/production-entry.mjs
mkdir -p .task-build
out_dir=$(mktemp -d .task-build/poses.XXXXXX)
node examples/instanced-skin.mjs "$out_dir"
test -s "$out_dir/poses.json"
node task_support/check-example.mjs "$out_dir/poses.json"
output_log=$(mktemp)
trap 'rm -f "$output_log"' EXIT
set +e
TASK_EXAMPLE_OUTPUT="$out_dir/poses.json" node task_support/driver.mjs >"$output_log" 2>&1
status=$?
set -e
cat "$output_log"
[[ $status == 0 ]] || exit "$status"
for group in palette pipeline lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
done
grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
node task_support/baseline.mjs
