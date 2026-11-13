#!/bin/sh
set -eu
mkdir -p .task-build
c++ -std=c++17 -O2 -I. -Itask_support/vendor/linear/include task_support/baseline.cpp -o .task-build/baseline
.task-build/baseline
