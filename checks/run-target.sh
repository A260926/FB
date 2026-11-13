#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
"${CXX:-c++}" -std=c++17 -O1 -I. -Ieigenlib checks/target_main.cpp tests/volume_preservation.cpp -o .task-build/volume_tests
.task-build/volume_tests
bash checks/install.sh
bash checks/baseline.sh > .task-build/default-baseline.txt
diff -u checks/default-baseline.txt .task-build/default-baseline.txt
cat .task-build/default-baseline.txt
printf "%s\n" VCG_VOLUME_TARGET_OK
