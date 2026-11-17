#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_support/PackedContract.java; fi
rm -rf .task-build/classes
mkdir -p .task-build/classes
find src/main/java task_support -name '*.java' ! -name 'PackedContract.java' > .task-build/sources
if [[ $mode == target ]]; then echo task_support/PackedContract.java >> .task-build/sources; fi
javac --release 17 -d .task-build/classes @.task-build/sources
if [[ $mode == target ]]; then
 output_log=$(mktemp)
 trap 'rm -f "$output_log"' EXIT
 set +e
 java -ea -cp .task-build/classes net.fornwall.jelf.TaskCheck >"$output_log" 2>&1
 status=$?
 set -e
 cat "$output_log"
 [[ $status == 0 ]] || exit "$status"
 for group in groups ownership compatibility; do
  grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
 done
 grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
java -ea -cp .task-build/classes net.fornwall.jelf.Baseline
