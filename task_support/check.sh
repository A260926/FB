#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s task_support/target.c
sh task_support/baseline.sh

# Property suite: driver + target + the split generator/oracle translation units.
cc -std=c99 -O2 -Wall -Wextra -Itask_support \
    task_support/driver.c task_support/target.c \
    task_support/task_model.c task_support/task_harness.c \
    -I. -I.task-build .task-build/libz.a -o .task-build/task_target
.task-build/task_target

# Predetermined logic-difference simulator: perturbing the oracle must make
# TASK_CHECK fail on the first case, and the run must stop there. The upstream
# library is untouched; only the test-side oracle is altered.
cc -std=c99 -O2 -Wall -Wextra -Itask_support -DTASK_FAULT_MAIN \
    task_support/target.c task_support/task_model.c task_support/task_harness.c \
    -I. -I.task-build .task-build/libz.a -o .task-build/task_fault
if .task-build/task_fault >/dev/null 2>.task-build/fault.log; then
    echo "fault simulator unexpectedly passed" >&2
    exit 1
fi
grep -q "FAULT_SIM first case: program 0" .task-build/fault.log
grep -q "FAIL" .task-build/fault.log
echo "FAULT_SIMULATOR_OK caught on first case"
