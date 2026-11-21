#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/lighttree_transport.cpp; fi
mkdir -p .task-build
c++ -std=c++11 -O2 -DNO_OMP -I . task-baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/lighttree.hxx
test -s examples/lighttree_demo.cpp
c++ -std=c++11 -O2 -DNO_OMP -I . -x c++ -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++11 -O2 -DNO_OMP -I . -include cstdio -include cstdlib -include src/lighttree.hxx tests/lighttree_transport.cpp .task-build/driver.o -o .task-build/target
c++ -std=c++11 -O2 -DNO_OMP -I . -include cstdio -include cstdlib -include src/lighttree.hxx examples/lighttree_demo.cpp -o .task-build/demo
rm -rf .task-build/demo-output
mkdir -p .task-build/demo-output
.task-build/demo .task-build/demo-output
test -s .task-build/demo-output/lighttree.pfm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in distribution mis render; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
