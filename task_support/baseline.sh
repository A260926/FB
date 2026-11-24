#!/bin/bash
set -euo pipefail
mkdir -p .task-build
printf 'AxxxxA\n' > .task-build/normal.in
rm -f .task-build/normal.out
.task-build/afl-tmin -i .task-build/normal.in -o .task-build/normal.out -m none -t 1000 -- .task-build/benign-target > .task-build/normal-tool.log 2>&1
test -s .task-build/normal.out
test "$(wc -c < .task-build/normal.out | tr -d ' ')" = 1
test "$(cat .task-build/normal.out)" = A
printf 'normal minimization input_bytes=7 output_bytes='
wc -c < .task-build/normal.out | tr -d ' '
printf 'retained='
cat .task-build/normal.out
printf '\n'
