#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s task_support/target.c
sh task_support/baseline.sh
cc -std=c99 -O2 -Itask_support \
  task_support/driver.c task_support/target.c \
  task_support/task_rng_ast.c task_support/task_oracle.c \
  task_support/task_utf.c task_support/task_engine.c \
  task_support/task_run8.c task_support/task_run16.c task_support/task_run32.c \
  -I.task-build .task-build/libpcre2-8.a .task-build/libpcre2-16.a .task-build/libpcre2-32.a \
  -o .task-build/task_target
.task-build/task_target
