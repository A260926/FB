#!/bin/bash
set -euo pipefail
out=$1
shift
if [[ $(uname -s) == Darwin ]]; then
  exec "${CXX:-c++}" -std=c++17 -Wno-narrowing -O1 -Icore -I. "$@" .task-build/libproduction.a -Wl,-dead_strip -o "$out"
else
  exec "${CXX:-c++}" -std=c++17 -Wno-narrowing -O1 -Icore -I. "$@" .task-build/libproduction.a -Wl,--gc-sections -o "$out"
fi
