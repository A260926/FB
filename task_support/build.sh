#!/bin/bash
set -euo pipefail
cmake -S . -B .task-build/cmake -DBUILD_CXXLIBS=OFF -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF -DWITH_OGG=OFF -DINSTALL_MANPAGES=OFF -DBUILD_SHARED_LIBS=OFF -DENABLE_MULTITHREADING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build/cmake --target FLAC --clean-first --parallel 4
cc -std=c99 -O2 -Iinclude task_support/baseline.c .task-build/cmake/src/libFLAC/libFLAC.a -lm -o .task-build/baseline
if [[ ${1:-baseline} == target ]]; then
  test -s include/FLAC/stream_encoder_checkpoint.h
  test -s src/libFLAC/stream_encoder_checkpoint.c
  ar -t .task-build/cmake/src/libFLAC/libFLAC.a | grep -Fx 'stream_encoder_checkpoint.c.o'
fi
