#!/bin/bash
set -euo pipefail
mkdir -p .task-build
for src in buffer parse unicode_codecvt; do
 c++ -std=c++14 -O1 -Ipe-parser-library/include -c "pe-parser-library/src/$src.cpp" -o ".task-build/$src.o"
done
ar rcs .task-build/libpe.a .task-build/buffer.o .task-build/parse.o .task-build/unicode_codecvt.o
c++ -std=c++14 -O1 -iquote . -Ipe-parser-library/include task_support/baseline.cpp .task-build/libpe.a -o .task-build/baseline
