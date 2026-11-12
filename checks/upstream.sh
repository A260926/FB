#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .checks-build
"${CC:-cc}" -std=c99 -D_DEFAULT_SOURCE -D_GNU_SOURCE -O2 -I ebur128 -I ebur128/queue \
  -Dmalloc=my_malloc -Dcalloc=my_calloc -c ebur128/ebur128.c -o .checks-build/fuzz-library.o
"${CXX:-c++}" -std=c++11 -O2 -I ebur128 test/fuzzer.cpp checks/upstream_driver.cpp \
  .checks-build/fuzz-library.o -lm -o .checks-build/upstream
.checks-build/upstream
