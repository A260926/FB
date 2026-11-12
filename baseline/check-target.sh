#!/usr/bin/env bash
set -euo pipefail
test -s tests/capture_values.c
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=c99 -O0 -Isrc src/*.c tests/capture_values.c -lm -o "$b/check"
"$b/check" | tee "$b/out"
grep -Eq '^CAPTURE_VALUES_OK cases=[1-9][0-9]*$' "$b/out"
bash baseline/run.sh
