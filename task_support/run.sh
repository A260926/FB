#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-target}
case "$mode" in baseline|target) ;; *) exit 64;; esac
work=$(mktemp -d "${TMPDIR:-/tmp}/theft-guided.XXXXXX")
printf 'BUILD_DIR=%s\n' "$work"
test -s task_support/consumer.c
test -s inc/theft.h
test -s inc/theft_types.h
sources=(src/*.c)
test "${#sources[@]}" -ge 12
for source in "${sources[@]}"; do test -s "$source"; done
cc -std=c99 -D_POSIX_C_SOURCE=200809L -O1 -g -Iinc -Isrc "${sources[@]}" task_support/consumer.c -lm -o "$work/legacy"
"$work/legacy"
if test "$mode" = target; then
    cc -std=c99 -D_POSIX_C_SOURCE=200809L -DGUIDED_API=1 -Werror=implicit-function-declaration -O1 -g -Iinc -Isrc "${sources[@]}" task_support/consumer.c -lm -o "$work/guided"
    "$work/guided"
fi
