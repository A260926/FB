#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=${TASK_BUILD_DIR:-build/task}
cmake -S . -B "$build" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" --target pbrt_test pbrt_exe -j "${TASK_JOBS:-6}"
