#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
sh task_support/build.sh
cc -std=c99 -Wall -Wextra -Isource task_support/baseline.c .build/liblexbor_static.a -lm -o .build/baseline
.build/baseline
