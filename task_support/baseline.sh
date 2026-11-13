#!/bin/sh
set -eu
mkdir -p .task-build
cc -std=c99 -O2 -I. sonic.c task_support/baseline.c -lm -o .task-build/baseline
.task-build/baseline
cc -std=c99 -O2 -I. sonic.c tests/runtests.c tests/sonic_api_test.c tests/input_clamping_test.c tests/genwave.c -lm -o .task-build/old-tests
.task-build/old-tests
