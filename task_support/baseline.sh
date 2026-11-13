#!/bin/sh
set -eu
mkdir -p .task-build
cc -std=c99 -O2 -I. task_support/baseline.c -lm -o .task-build/baseline
.task-build/baseline
