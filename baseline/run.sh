#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=c99 -O0 -Isrc src/*.c baseline/start.c -o "$b/start"
"$b/start"
