#!/bin/bash
set -euo pipefail
.task-build/decode_probe task_support/fixtures/mono16.h265 task_support/fixtures/mono16.yuv
.task-build/decode_probe task_support/fixtures/44416.h265 task_support/fixtures/44416.yuv
test -s examples/extended_precision.cc
c++ -std=c++17 -I. -I.task-build examples/extended_precision.cc .task-build/libde265/libde265.a -pthread -o .task-build/extended_precision
rm -f .task-build/extended-example.yuv
.task-build/extended_precision task_support/fixtures/44416.h265 .task-build/extended-example.yuv
test -s .task-build/extended-example.yuv
cmp .task-build/extended-example.yuv task_support/fixtures/44416.yuv
c++ -std=c++17 -I. -I.task-build tests/extended_precision_task.cc task_support/driver.c .task-build/libde265/libde265.a -pthread -o .task-build/target
