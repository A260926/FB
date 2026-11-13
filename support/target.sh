#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
bash support/build.sh "$b"
cc -std=c99 -Isupport -Iinclude -I"$b/include" support/driver.c support/target_cases.c "$b/libsndfile.a" -lm -o "$b/cases"
(cd "$b"; ./cases) | tee "$b/output"
awk -v style=A -v groups="numeric stream compat" -f support/verify_output.awk "$b/output"
bash support/baseline.sh
