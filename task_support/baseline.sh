#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
cc -std=c99 -O1 -Isrc -Itask_support src/*.c task_support/fixture.c task_support/baseline.c -o .task-build/baseline
.task-build/baseline "$@"
