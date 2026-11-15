#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/rsm_target.cpp; fi
if [[ $mode == target ]]; then
 test -s include/microgl/rsm/Rsm.h
 printf '#include "microgl/rsm/Rsm.h"\n' | c++ -std=c++17 -include new -I. -Iinclude -x c++ -fsyntax-only -
fi
mkdir -p .task-build
c++ -std=c++17 -include new -O1 -Iinclude task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  if [[ -d src/rsm ]]; then find src/rsm -type f -name '*.cpp' -print | LC_ALL=C sort > .task-build/rsm-sources; else : > .task-build/rsm-sources; fi
c++ -std=c++17 -include new -O1 -I. -Iinclude task_support/driver.c tests/rsm_target.cpp $(cat .task-build/rsm-sources) -o .task-build/target
test -s examples/headless_rsm.cpp
 c++ -std=c++17 -include new -O1 -I. -Iinclude examples/headless_rsm.cpp $(cat .task-build/rsm-sources) -o .task-build/rsm-example
rm -f .task-build/rsm.ppm
 .task-build/rsm-example
 test -s .task-build/rsm.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in capture transport camera lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
