#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++11 -O0 -Ielf -Idwarf -Itask_support elf/*.cc dwarf/*.cc task_support/baseline.cc -o "$b/baseline"
"$b/baseline"
