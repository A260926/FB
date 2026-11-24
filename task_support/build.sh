#!/bin/bash
set -euo pipefail
mkdir -p .task-build
export GIT_CEILING_DIRECTORIES="$PWD"
make -B -j4 USE_ASM=No HAVE_GTEST=No HAVE_GMP_API=No CFLAGS_OPT=-O1 libopenh264.a >.task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
c++ -std=c++17 -Icodec/api/wels -Itask_support task_support/baseline.cpp libopenh264.a -pthread -o .task-build/baseline
