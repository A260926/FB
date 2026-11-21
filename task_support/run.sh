#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
node task_support/generate.mjs
if [[ ${1:-baseline} == target ]]; then
  for file in checks/ploc.mjs src/core/build/PLOCBuilder.js src/workers/NodePLOCWorker.js src/workers/plocNode.worker.js example/ploc.mjs; do
    test -s "$file"
    node --check "$file"
  done
  target_log=$(mktemp)
  output_dir=$(mktemp -d)
  trap 'rm -f "$target_log"; rm -rf "$output_dir"' EXIT
  node --input-type=module -e "await import('./src/core/build/PLOCBuilder.js'); await import('./src/workers/NodePLOCWorker.js');"
  set +e
  node task_support/driver.mjs >"$target_log" 2>&1
  status=$?
  set -e
  cat "$target_log"
  [[ $status == 0 ]] || exit "$status"
  for group in clustering parallel mapping render; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$target_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$target_log"
  node example/ploc.mjs "$output_dir/ploc.ppm"
  test -s "$output_dir/ploc.ppm"
  node --input-type=module -e "import fs from 'node:fs';import assert from 'node:assert/strict';const b=fs.readFileSync(process.argv[1]);assert.ok(b.length>64&&/^P[36]\\s/.test(b.toString('ascii',0,3)));" "$output_dir/ploc.ppm"
fi
node task_support/baseline.mjs
