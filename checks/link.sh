#!/bin/bash
set -euo pipefail
output=$1
shift
"${CXX:-c++}" -std=c++20 -O1 -I. -Irasterizer -include algorithm -include limits "$@" .task-build/renderer.o .task-build/utility.o -o "$output"
