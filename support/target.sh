#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
bash support/build.sh "$b"
cc -std=c99 -Isupport -Isrc/lib support/driver.c support/target_cases.c "$b/libshine.a" -lm -o "$b/cases"
"$b/cases" | tee "$b/output"
awk -v style=A -v groups="windows bitstream state" -f support/verify_output.awk "$b/output"
bash support/baseline.sh
