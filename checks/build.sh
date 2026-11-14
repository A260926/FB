#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cmake -S . -B .task-build/lib -DBUILD_SHARED=OFF -DBUILD_STATIC=ON -DBUILD_TESTING=OFF -DLIBXMP_DISABLE_DEPACKERS=ON -DLIBXMP_DISABLE_PROWIZARD=ON
cmake --build .task-build/lib --parallel 4
: > .task-build/lib/all_tests.c
