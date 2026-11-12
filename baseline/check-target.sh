#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
test -s tests/header_assoc.c
cc -std=c99 -O0 -Isrc src/*.c tests/header_assoc.c -o "$b/target"
"$b/target" | tee "$b/out"
grep -Eq '^HEADER_ASSOC_OK cases=[1-9][0-9]*$' "$b/out"
bash baseline/run.sh
