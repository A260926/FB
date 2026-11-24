#!/bin/bash
set -euo pipefail
mkdir -p .task-build
# Remove only objects and executables produced by this entry.
rm -f .task-build/*.o .task-build/libcmpcov.a .task-build/baseline .task-build/target .task-build/api-probe .task-build/receipt_merge .task-build/receipt_replay
for name in common modules tokenizer traces cmpcov receipt; do
 c++ -std=c++17 -O0 -g -pthread -I. -c "source/$name.cc" -o ".task-build/$name.o"
done
if [[ ${1:-baseline} == target ]]; then
 test -s source/receipt.h
 test -s source/receipt.cc
 ar rcs .task-build/libcmpcov.a .task-build/common.o .task-build/modules.o .task-build/tokenizer.o .task-build/traces.o .task-build/cmpcov.o .task-build/receipt.o
else
 ar rcs .task-build/libcmpcov.a .task-build/common.o .task-build/modules.o .task-build/tokenizer.o .task-build/traces.o .task-build/cmpcov.o .task-build/receipt.o
fi
c++ -std=c++17 -O0 -g -pthread -I. task_support/baseline.cpp .task-build/libcmpcov.a -o .task-build/baseline
