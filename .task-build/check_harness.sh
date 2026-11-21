#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
out="$TMPDIR/ovm_output.log"
.task-build/target >"$out" 2>&1
rc=$?
cat "$out" | grep -E '^(GROUP|TARGET_COMPLETE)'
[ "$rc" -eq 0 ]
for group in closure geometry corners; do
  grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$out"
done
grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$out"
echo "HARNESS OUTPUT CHECKS PASSED"
