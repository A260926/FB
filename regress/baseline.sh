#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .build
cc -std=c11 -DPLUTOVG_BUILD_STATIC -Iinclude -Isource regress/probe.c source/*.c -lm -o .build/probe
.build/probe
