#!/bin/bash
set -euo pipefail
c++ -std=c++11 -O1 -Icodec/api/wels task_support/baseline.cpp libopenh264.a -pthread -lm -o .task-build/baseline
.task-build/baseline
