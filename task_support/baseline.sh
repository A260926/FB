#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .task-build
cc -std=c99 -O1 -g -DMG_ENABLE_LOG=0 -I. mongoose.c task_support/baseline.c -o .task-build/baseline
.task-build/baseline
