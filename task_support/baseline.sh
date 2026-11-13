#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=c99 -O2 -Isrc task_support/baseline.c src/foxen-flac.c -o "$b/base"
"$b/base"
