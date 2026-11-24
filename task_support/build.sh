#!/bin/bash
set -euo pipefail
mkdir -p .task-build
rm -f .task-build/unit-*.o .task-build/libproduction.a .task-build/baseline
find liveMedia groupsock UsageEnvironment BasicUsageEnvironment -maxdepth 1 -type f \( -name '*.cpp' -o -name '*.c' \) | LC_ALL=C sort > .task-build/sources.txt
test -s .task-build/sources.txt
xargs -P 8 -n 1 bash task_support/compile-one.sh < .task-build/sources.txt
ar rcs .task-build/libproduction.a .task-build/unit-*.o
c++ -std=c++20 -pthread -DNO_OPENSSL=1 -DSOCKLEN_T=socklen_t -I. -IliveMedia/include -Igroupsock/include -IUsageEnvironment/include -IBasicUsageEnvironment/include task_support/baseline.cpp .task-build/libproduction.a -o .task-build/baseline
