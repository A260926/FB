#!/usr/bin/env bash
set -euo pipefail
mkdir -p .baseline
mode=${1:-baseline}
case "$mode" in baseline|witness) ;; *) exit 2 ;; esac
${CC:-cc} -std=c99 -O0 -Itask_support -Isrc -Isrc/common -Isrc/libzzuf task_support/"$mode".c src/common/fd.c src/common/fuzz.c src/common/random.c src/common/ranges.c -lm -o .baseline/"$mode"
.baseline/"$mode"
