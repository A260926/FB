#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
cc_bin=${CC:-cc}
"$cc_bin" -std=c99 -O1 -Iinc -I. checks/old_shader.c src/*.c src/ext/*.c -lm -o .task-build/old_shader
.task-build/old_shader
"$cc_bin" -std=c99 -O1 -Iinc -I. checks/constants_baseline.c src/*.c src/ext/*.c -lm -o .task-build/constants_baseline
.task-build/constants_baseline
echo BASELINE_OK
