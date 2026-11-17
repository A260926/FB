#!/bin/bash
set -euo pipefail
export CCACHE_DISABLE=1
mkdir -p .task-build
cmake -S . -B .task-build/library -DBUILD_TESTING=OFF -DQAWS_BUILD_EXAMPLES=OFF -DQAWS_ENABLE_EXACT=OFF -DQAWS_INSTALL=OFF -DQAWS_WARNINGS_AS_ERRORS=OFF -DCMAKE_BUILD_TYPE=Debug '-DCMAKE_C_FLAGS_DEBUG=-O0 -g0' -DQAWS_DEBUG_POSTFIX=
cmake --build .task-build/library --target qaws --parallel 4
${CC:-cc} -std=c11 -O0 -g0 -DQAWS_SCALAR_IS_FLOAT=0 -Isrc/qaws task_support/baseline.c .task-build/library/libqaws.a -lm -o .task-build/baseline
