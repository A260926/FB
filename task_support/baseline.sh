#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .task-build
cc -std=c99 -O1 -U__aarch64__ -U__ARM_NEON -I. task_support/baseline.c -o .task-build/baseline
.task-build/baseline
