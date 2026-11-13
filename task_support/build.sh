#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
b="${TASK_BUILD_DIR:-.build}"
cmake -S . -B "$b" -DBUILD_SHARED_LIB=OFF -DBUILD_STATIC_LIB=ON -DBUILD_TOOLS=OFF -DBUILD_DOC=OFF -DBUILD_UNIT_TEST=OFF -DLIBAAF_VERSION=v0.0.0-fixed
cmake --build "$b" --target aaf-static --parallel 2
