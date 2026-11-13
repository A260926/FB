#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s task_support/target_cases.c || { echo 'Missing task_support/target_cases.c' >&2; exit 1; }
mkdir -p .task-build
cc -std=c99 -O1 -Isrc -Itask_support src/*.c task_support/fixture.c task_support/driver.c task_support/target_cases.c -o .task-build/target
status=0
.task-build/target > .task-build/target.stdout 2> .task-build/target.stderr || status=$?
cat .task-build/target.stdout
cat .task-build/target.stderr >&2
if [ "$status" -ne 0 ]; then exit "$status"; fi
for group in lists prediction filter; do
    count=$(grep -Ec "^GROUP $group checks=[1-9][0-9]* COMPLETE$" .task-build/target.stdout || true)
    [ "$count" -eq 1 ] || { echo "Incomplete group $group" >&2;exit 1; }
done
[ "$(grep -cx 'H264BSD_B_EXPLICIT_OK' .task-build/target.stdout || true)" -eq 1 ] || exit 1
bash task_support/baseline.sh
