#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
b="${TASK_BUILD_DIR:-.build}"
cmake -S . -B "$b" -DBUILD_APPS=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build "$b" --target ap4 --parallel 2
