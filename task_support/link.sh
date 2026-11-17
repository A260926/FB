#!/bin/bash
set -euo pipefail
includes=(-I. -IlibMachO)
while IFS= read -r dir; do includes+=("-I$dir"); done < <(find libMachO -type d | sort)
clang -std=gnu11 -fblocks -O0 -g "${includes[@]}" task_support/driver.c tests/file_map_regression.c .task-build/libMachO.a -o .task-build/target
