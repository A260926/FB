#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d)
trap 'find "$b" -type f -delete; rmdir "$b"' EXIT
cc -O2 -std=gnu11 tests/cts.c -o "$b/cts" -lm
"$b/cts"
printf 'minimp4 cts: ok\n'
