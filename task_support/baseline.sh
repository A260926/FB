#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .task-build
cc -std=c99 -O1 -I. task_support/baseline.c -lm -o .task-build/baseline
.task-build/baseline
