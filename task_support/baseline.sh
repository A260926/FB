#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=c99 -O1 -I. one.c task_support/baseline.c -lm -o "$b/baseline"
"$b/baseline"
