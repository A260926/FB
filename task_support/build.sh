#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cmake -S ports/cmake -B .task-build/cmake -DBUILD_SHARED_LIBS=OFF -DBUILD_PROGRAMS=OFF -DDEFAULT_OUTPUT_MODULE=dummy -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-DHAVE_MMAP=1 -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=TRUE -DCMAKE_DISABLE_FIND_PACKAGE_ALSA=TRUE > .task-build/configure.log 2>&1 || { tail -30 .task-build/configure.log; exit 1; }
cmake --build .task-build/cmake --target libout123 -j 2 > .task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
cc -std=c99 -Isrc/include task_support/wave_probe.c .task-build/cmake/src/libout123/libout123.a .task-build/cmake/src/libout123/modules/libdefaultmodule.a -lm -o .task-build/wave-probe
