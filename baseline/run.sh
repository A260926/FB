#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=c99 -O0 -I. baseline/start.c -lm -o "$b/start"
"$b/start"
