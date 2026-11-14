#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
cc -std=c99 -O1 -Isrc -c src/tinyspline.c -o .task-build/tinyspline.o
cc -std=c99 -O1 -Isrc -c src/parson.c -o .task-build/parson.o
c++ -std=c++11 -O1 -Isrc -c src/tinysplinecxx.cxx -o .task-build/tinysplinecxx.o
cc -std=c99 -O1 -Isrc -Itest -Itest/cutest task_support/upstream.c test/c/interpolation.c test/testutils.c test/cutest/CuTest.c .task-build/tinyspline.o .task-build/parson.o -lm -o .task-build/upstream
c++ -std=c++11 -O1 -Isrc task_support/baseline.cpp .task-build/tinysplinecxx.o .task-build/tinyspline.o .task-build/parson.o -lm -o .task-build/baseline
