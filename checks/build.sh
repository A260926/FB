#!/bin/bash
set -euo pipefail
mkdir -p .task-build
arch_args=()
if [[ $(uname -s) == Darwin ]]; then arch_args=(-arch x86_64); fi
# Bash 3 cannot expand an empty array with nounset.
compile() {
  if [[ $(uname -s) == Darwin ]]; then
    "${CXX:-c++}" -arch x86_64 -std=c++17 -O1 -pthread -I. -include checks/macos_compat.h "$@"
  else
    "${CXX:-c++}" -std=c++17 -O1 -pthread -I. "$@"
  fi
}
compile -msse4.1 -c MaskedOcclusionCulling.cpp -o .task-build/moc.o
compile -mavx2 -mfma -mbmi -mbmi2 -mlzcnt -c MaskedOcclusionCullingAVX2.cpp -o .task-build/avx2.o
compile -mavx2 -mfma -msse4.1 -c MaskedOcclusionCullingAVX512.cpp -o .task-build/avx512.o
compile -c CullingThreadpool.cpp -o .task-build/pool.o
