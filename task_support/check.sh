#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s task_support/target.c
sh task_support/baseline.sh
cc -std=c99 -O2 -Wall -Wextra -Itask_support \
  task_support/driver.c \
  task_support/target.c \
  task_support/task_ast.c \
  task_support/task_enc.c \
  task_support/task_gen.c \
  task_support/task_render.c \
  task_support/task_oracle.c \
  task_support/task_engine.c \
  task_support/task_meta.c \
  task_support/task_harness.c \
  -I.task-build \
  .task-build/libpcre2-8.a .task-build/libpcre2-16.a .task-build/libpcre2-32.a \
  -o .task-build/task_target
.task-build/task_target
