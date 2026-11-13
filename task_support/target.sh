#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s cli/cutplan.c
test -s cli/cutplan.h
test -s tests/target_cases.c
sh task_support/build.sh
cc -std=c99 -O1 -I. cli/remuxer.c cli/cutplan.c cli/cli.c liblsmash.a -lm -o .task-build/remuxer_target
cc -std=c99 -O1 -I. tests/target_cases.c task_support/target_main.c liblsmash.a -lm -o .task-build/target
.task-build/target
sh task_support/baseline.sh
