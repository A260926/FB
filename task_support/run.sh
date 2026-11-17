#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s src/test/java/org/jcodec/movtool/MultiSourceFlattenTest.java; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  javac --release 8 -encoding UTF-8 -cp .task-build/classes -d .task-build/classes src/test/java/org/jcodec/movtool/MultiSourceFlattenTest.java
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  java -cp .task-build/classes task_support.Driver > "$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in route onTop processor rejectCompat; do
    grep -Eq "^GROUP ${group} checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Fxq 'TARGET_COMPLETE groups=4' "$output_log"
  java -cp .task-build/classes task_support.Baseline
else
  java -cp .task-build/classes task_support.Baseline --observe-gap
fi
