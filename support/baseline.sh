#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
bash support/build.sh "$b"
cc -std=c99 -Isrc/lib support/baseline.c "$b/libshine.a" -lm -o "$b/baseline"
"$b/baseline"
