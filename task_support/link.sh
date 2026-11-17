#!/bin/bash
set -euo pipefail
c++ -std=c++20 -O1 -pthread -DCUBIQUITY_USE_POOLSTL -I. -Isrc/library -Isrc/application/external -x c++ task_support/driver.c tests/combine_translated_target.cpp -x none .task-build/libtask.a -o .task-build/target
