#!/bin/bash
set -euo pipefail
output=$1
shift
if [[ $(uname -s) == Darwin ]]; then
 "${CXX:-c++}" -arch x86_64 -std=c++17 -O1 -pthread -include cstddef -I. "$@" .task-build/moc.o .task-build/avx2.o .task-build/avx512.o .task-build/pool.o -o "$output"
else
 "${CXX:-c++}" -std=c++17 -O1 -pthread -include cstddef -I. "$@" .task-build/moc.o .task-build/avx2.o .task-build/avx512.o .task-build/pool.o -o "$output"
fi
