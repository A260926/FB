#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .output
cc -std=c99 -O2 support/impl.c support/baseline.c -lm -o .output/baseline
.output/baseline "$@"
bash support/legacy-example.sh
