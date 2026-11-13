#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=c99 -O2 -Isrc -Itask_support task_support/driver.c task_support/target_cases.c src/foxen-flac.c -o "$b/target"
"$b/target" > "$b/output" 2>&1 || { rc=$?; cat "$b/output"; exit "$rc"; }
cat "$b/output"
grep -Eq "^GROUP samples [1-9][0-9]* COMPLETE$" "$b/output"
grep -Eq "^GROUP stereo [1-9][0-9]* COMPLETE$" "$b/output"
grep -Eq "^GROUP stream [1-9][0-9]* COMPLETE$" "$b/output"
grep -qx "FOXEN_32_OK" "$b/output"
bash task_support/baseline.sh
