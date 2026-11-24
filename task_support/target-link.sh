#!/bin/bash
set -euo pipefail
for file in source/receipt.h source/receipt.cc tools/receipt_merge.cc examples/receipt_replay.cpp; do test -s "$file"; done
c++ -std=c++17 -O0 -g -pthread -I. -include source/receipt.h task_support/api_probe.cpp .task-build/receipt.o .task-build/libcmpcov.a -o .task-build/api-probe
.task-build/api-probe
c++ -std=c++17 -O0 -g -pthread -I. tools/receipt_merge.cc .task-build/receipt.o .task-build/libcmpcov.a -o .task-build/receipt_merge
c++ -std=c++17 -O0 -g -pthread -I. examples/receipt_replay.cpp .task-build/receipt.o .task-build/libcmpcov.a -o .task-build/receipt_replay
mkdir -p .task-build/replay-output
rm -f .task-build/replay-output/transcript.txt
.task-build/receipt_replay .task-build/replay-output
test -s .task-build/replay-output/transcript.txt
c++ -x c++ -std=c++17 -O0 -I. -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++17 -O0 -g -pthread -I. checks/receipt_tests.cpp .task-build/driver.o .task-build/receipt.o .task-build/libcmpcov.a -o .task-build/target
