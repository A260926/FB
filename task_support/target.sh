#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
bash task_support/build.sh "$b"
cc -std=c11 -O2 -include task_support/build_config.h -Iinclude -Ilibfaac -Itask_support task_support/driver.c task_support/target_cases.c "$b/libfaac.a" -lm -o "$b/target"
"$b/target" > "$b/output" 2>&1 || { rc=$?; cat "$b/output"; exit "$rc"; }
cat "$b/output"
grep -Eq "^GROUP layout [1-9][0-9]* COMPLETE$" "$b/output"
grep -Eq "^GROUP config [1-9][0-9]* COMPLETE$" "$b/output"
grep -Eq "^GROUP stream [1-9][0-9]* COMPLETE$" "$b/output"
grep -qx "FAAC_PCE_OK" "$b/output"
bash task_support/baseline.sh
