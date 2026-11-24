#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/clip_contracts.cpp; fi
bash task_support/build.sh "$mode"
if [[ $mode == target ]]; then
  c++ -std=c++17 -O0 -I. tests/clip_contracts.cpp -x c++ task_support/driver.c -x none .task-build/obj/*.o -o .task-build/target
c++ -std=c++17 -O0 -I. task_support/api_consumer.cpp .task-build/obj/*.o -o .task-build/api_consumer
.task-build/api_consumer
test -s examples/clip_surface.cpp
c++ -std=c++17 -O0 -I. examples/clip_surface.cpp .task-build/obj/*.o -o .task-build/example
rm -f .task-build/clipped.obj
.task-build/example .task-build/clipped.obj
test -s .task-build/clipped.obj
awk '$1=="v"{v++} $1=="vt"{t++} $1=="vn"{n++} $1=="f"{f++} END{exit !(v>=3 && t>=3 && n>=3 && f>=1)}' .task-build/clipped.obj
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in bounds topology attributes; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
