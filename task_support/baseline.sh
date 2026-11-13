#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
cc -std=c99 -O2 -Itask_support task_support/baseline.c -I.task-build .task-build/libpcre2-8.a .task-build/libpcre2-16.a .task-build/libpcre2-32.a -o .task-build/task_baseline
.task-build/task_baseline
