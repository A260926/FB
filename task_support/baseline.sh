#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
bash task_support/build.sh "$b"
cc -std=c11 -O2 -Iinclude task_support/baseline.c "$b/libfaac.a" -lm -o "$b/base"
"$b/base"
