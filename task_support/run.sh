#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/test_unsubdivide.cpp; fi
mkdir -p .task-build
cc -std=c99 -O1 -DCC_DISABLE_UV -I. -c task_support/upstream.c -o .task-build/upstream.o
c++ -std=c++17 -O1 -DCC_DISABLE_UV -I. checks/baseline.cpp .task-build/upstream.o -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s cc_Unsubdivide.h
test -s src/cc_Unsubdivide.cpp
test -s examples/unsubdivide.cpp
c++ -std=c++17 -O1 -DCC_DISABLE_UV -I. -include cc_Unsubdivide.h -c src/cc_Unsubdivide.cpp -o .task-build/unsubdivide.o
c++ -std=c++17 -O1 -DCC_DISABLE_UV -I. -include cc_Unsubdivide.h -c checks/test_unsubdivide.cpp -o .task-build/cases.o
c++ -x c++ -std=c++17 -O1 -I. -c task_support/driver.c -o .task-build/driver.o
c++ .task-build/unsubdivide.o .task-build/upstream.o .task-build/cases.o .task-build/driver.o -lm -o .task-build/target
c++ -std=c++17 -O1 -DCC_DISABLE_UV -I. -include cc_Unsubdivide.h examples/unsubdivide.cpp .task-build/unsubdivide.o .task-build/upstream.o -lm -o .task-build/example
rm -f .task-build/recovered.ccm
.task-build/example .task-build/recovered.ccm
test -s .task-build/recovered.ccm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in topology inverse rejection; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
