#!/bin/bash
set -euo pipefail
mkdir -p .task-build
c++ -std=c++17 -O1 -pthread -I. -Iincludes -Iexternal/rtm/includes task_support/baseline.cpp -o .task-build/baseline
