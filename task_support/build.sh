#!/bin/bash
set -euo pipefail
mkdir -p .task-build
make -f task_support/build.mk -j4 archive
case $(uname -s) in
 Darwin) link_flags=-Wl,-dead_strip ;;
 *) link_flags=-Wl,--gc-sections ;;
esac
c++ -std=c++11 -DNDEBUG -DPBRT_PROBES_NONE -Isrc -Isrc/core -I. task_support/baseline.cpp .task-build/libproduction.a "$link_flags" -pthread -o .task-build/baseline
