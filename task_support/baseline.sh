#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/legacy-out
rm -f .task-build/legacy-out/results.txt .task-build/legacy-out/results-summary.txt .task-build/legacy-out/suppress.txt
perl -c drmemory/postprocess.pl
perl drmemory/postprocess.pl -aggregate -x benign-simulator -nodefault_suppress -batch -l .task-build/legacy-out task_support/fixtures/DrMemory-sim >.task-build/legacy-command.log 2>&1
grep -Eq '2 unique,[[:space:]]+2 total unaddressable access\(es\)' .task-build/legacy-out/results.txt
grep -q 'Error #1: UNADDRESSABLE ACCESS' .task-build/legacy-out/results.txt
grep -q 'Error #2: UNADDRESSABLE ACCESS' .task-build/legacy-out/results.txt
printf '%s\n' 'LEGACY_POSTPROCESS errors=2 unique=2 total=2 no-target-execution' 'BASELINE_COMPLETE native-log-parser legacy-summary'
