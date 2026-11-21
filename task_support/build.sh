#!/bin/bash
set -euo pipefail
mkdir -p .task-build
sig=$(find . -maxdepth 1 -type f \( -name 'tiny_*.h' -o -name 'tiny_bvh_c.cpp' \) -exec shasum -a 256 {} + | LC_ALL=C sort | shasum -a 256)
if [[ ! -f .task-build/signature || $(cat .task-build/signature) != "$sig" ]]; then
 c++ -std=c++20 -O1 -pthread -DTINYBVH_NO_SIMD -Dtinybvh=task_capi_tinybvh -I. -c tiny_bvh_c.cpp -o .task-build/capi.o
 c++ -std=c++20 -O1 -pthread -DTINYBVH_NO_SIMD -I. -c task_support/library.cpp -o .task-build/library.o
 printf '%s' "$sig" > .task-build/signature
fi
c++ -std=c++20 -O1 -pthread -DTINYBVH_NO_SIMD -I. task_support/baseline.cpp .task-build/library.o -o .task-build/baseline
