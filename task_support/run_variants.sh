#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
for numeric in float fixed; do
  status=0
  ".task-build/target-$numeric" > ".task-build/target-$numeric.log" 2>&1 || status=$?
  cat ".task-build/target-$numeric.log"
  [[ $status == 0 ]] || exit "$status"
  for group in encoder decoder recursive; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" ".task-build/target-$numeric.log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' ".task-build/target-$numeric.log"
done
