#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_support/InitializationContract.java; fi
rm -rf .task-build/classes
mkdir -p .task-build/classes
find src -name '*.java' > .task-build/production
javac --release 8 -d .task-build/classes @.task-build/production
find task_support -name '*.java' ! -name 'InitializationContract.java' > .task-build/sources
if [[ $mode == target ]]; then echo task_support/InitializationContract.java >> .task-build/sources; fi
javac --release 17 -cp .task-build/classes -d .task-build/classes @.task-build/sources
if [[ $mode == target ]]; then
 output_log=$(mktemp "${TMPDIR:-/tmp}/task-target.XXXXXXXX")
 trap 'rm -f "$output_log"' EXIT
 set +e
 java -ea -cp .task-build/classes task.TaskCheck >"$output_log" 2>&1
 status=$?
 set -e
 cat "$output_log"
 [[ $status == 0 ]] || exit "$status"
 for group in aliases flow compatibility; do
  grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
 done
 grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
java -ea -cp .task-build/classes task.Baseline
