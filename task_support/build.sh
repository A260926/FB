#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
cmake -S . -B .task-build/engine -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release -DTASK_TARGET="${1:-OFF}" > .task-build/configure.log 2>&1 || { tail -30 .task-build/configure.log; exit 1; }
cmake --build .task-build/engine --target pbrt_exe task_baseline -j 6 > .task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
