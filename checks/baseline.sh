#!/bin/bash
set -euo pipefail
if [[ ! -f .task-build/lib/libxmp.a ]]; then bash checks/build.sh; fi
cc -std=c99 -Iinclude checks/baseline.c .task-build/lib/libxmp.a -lm -o .task-build/baseline
.task-build/baseline
