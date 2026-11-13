#!/usr/bin/env bash
set -euo pipefail
mkdir -p .baseline
${CC:-cc} -std=c99 -O1 -Isrc/include -Isrc/vm -Isrc/optional task_support/baseline.c src/vm/*.c src/optional/*.c -lm -o .baseline/wren
.baseline/wren
