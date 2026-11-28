#!/bin/bash
set -euo pipefail
test -s tests/mixed_target.c
test -s examples/mixed_crowd.c
bash task_support/link.sh -Werror=implicit-function-declaration -Werror=int-conversion task_support/api_probe.c -o .task-build/api_probe
.task-build/api_probe
bash task_support/link.sh task_support/driver.c tests/mixed_target.c -o .task-build/target
bash task_support/link.sh examples/mixed_crowd.c -o .task-build/example
rm -rf .task-build/example-output
mkdir -p .task-build/example-output
.task-build/example .task-build/example-output > .task-build/example.log
cat .task-build/example.log
grep -qx MIXED_CROWD_DEMO_OK .task-build/example.log
test -s .task-build/example-output/mixed_crowd.obj
awk 'BEGIN {v=0} $1=="v" && NF==4 {v++} END {exit !(v>=2)}' .task-build/example-output/mixed_crowd.obj
