#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash checks/build.sh
cc -std=c99 -O2 -Ichecks -Ilibtwolame checks/baseline.c .task-build/libtwolame.a -lm -o .task-build/baseline
.task-build/baseline
