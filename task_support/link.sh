#!/bin/bash
set -euo pipefail
out=$1; shift
exec "${CXX:-c++}" -std=c++20 -O0 -DPMP_SCALAR_TYPE_64 -Isrc -Iexternal/eigen-5.0.1 -I. "$@" .task-build/libproduction.a -o "$out"
