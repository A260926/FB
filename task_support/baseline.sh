#!/bin/bash
set -euo pipefail
c++ -std=c++11 -O1 -I. -I.task-build task_support/baseline.cpp .task-build/libvpxrc.a .task-build/libvpx.a -pthread -lm -o .task-build/baseline
.task-build/baseline
