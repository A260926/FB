#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/regression_tests.cpp; fi
rm -rf .task-build
mkdir -p .task-build
if [[ $mode == target ]]; then
  for task_src in drmemory/regression.h drmemory/regression.cpp drmemory/regression_store.cpp drmemory/regression_cli.cpp; do test -s "$task_src"; done
  for task_src in drmemory/regression.cpp drmemory/regression_store.cpp drmemory/regression_cli.cpp; do
    clang++ -std=c++17 -O0 -I. -include drmemory/regression.h -c "$task_src" -o ".task-build/$(basename "$task_src" .cpp).o"
  done
  ar rcs .task-build/regression.a .task-build/regression.o .task-build/regression_store.o
  clang++ .task-build/regression_cli.o .task-build/regression.a -o .task-build/regression-engine
fi
if [[ $mode == target ]]; then
  clang++ -std=c++17 -I. task_support/api_probe.cpp .task-build/regression.a -o .task-build/api-probe
.task-build/api-probe
bash task_support/native_probe.sh
test -s examples/regression_replay.cpp
clang++ -std=c++17 -I. examples/regression_replay.cpp .task-build/regression.a -o .task-build/regression-demo
mkdir -p .task-build/demo
rm -f .task-build/demo/transcript.txt
.task-build/regression-demo .task-build/demo
test -s .task-build/demo/transcript.txt
clang++ -std=c++17 -I. -x c++ -c task_support/driver.c -o .task-build/test-driver.o
clang++ -std=c++17 -I. checks/regression_tests.cpp .task-build/test-driver.o .task-build/regression.a -o .task-build/target
  output_log=$(mktemp 2>/dev/null || mktemp .task-build/run-output.XXXXXX)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in timeline consensus resume; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash task_support/baseline.sh
