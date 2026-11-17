#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cc -std=c99 -O1 -DNDEBUG -I. -c src/build_lib.c -o .task-build/libttar.o
cc -std=c99 -O1 -I. task_support/baseline.c .task-build/libttar.o -o .task-build/baseline
