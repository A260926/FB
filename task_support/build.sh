#!/usr/bin/env bash
set -euo pipefail
out=$1; shift
cc -std=c99 -O2 -DHAVE_STRING_H=1 -I. -Ilibudis86 -Itask_support/generated -Itask_support/generated/libudis86 libudis86/*.c task_support/generated/libudis86/itab.c "$@" -o "$out"
