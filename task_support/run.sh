#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in
  baseline) target=OFF ;;
  target)
    test -s src/draco/compression/mesh/mesh_decode_session.h
    test -s src/draco/compression/mesh/mesh_decode_session.cc
    target=ON ;;
  *) echo "usage: bash task_support/run.sh baseline|target" >&2; exit 2 ;;
esac
cmake -S task_support -B .task-build -DTASK_BUILD_TARGET="$target" -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build --target baseline --parallel 4
# Retain the baseline exit status. No optional test/example or stale artifact.
.task-build/baseline
