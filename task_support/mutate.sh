#!/bin/sh
#
# mutate.sh - exercise the predetermined logic-difference simulator.
#
# The oracle in target.c can be put into a deliberately wrong state with
# TASK_MUTATE=1: its expected plaintext first byte is flipped while the real
# zlib code and the upstream library sources are untouched.  The oracle must
# then fail on the very first mismatching case and stop (TASK_CHECK exits the
# process immediately, so no later case can mask the failure).
set -eu
cd "$(dirname "$0")/.."
cc -std=c99 -O2 -Itask_support task_support/driver.c task_support/target.c task_support/model.c -I. -I.task-build .task-build/libz.a -o .task-build/task_mutate
if TASK_MUTATE=1 .task-build/task_mutate >.task-build/mutate.out 2>&1; then
    echo "MUTATION_ESCAPED: oracle accepted a wrong expectation" >&2
    exit 1
fi
grep -q "FAIL" .task-build/mutate.out
echo "--- first retained failing case ---"
sed -n '1,4p' .task-build/mutate.out
echo "MUTATION_CAUGHT"
