#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/release_contract.cpp; fi
bash task_support/build.sh "$mode"
if [[ $mode == target ]]; then
  test -s examples/release_contract.cpp
cc=${CC:-cc}
cxx=${CXX:-c++}
"$cc" -std=gnu99 -I. -c task_support/driver.c -o .task-build/driver.o
"$cc" -std=gnu99 -I.task-build/production -I. -include allocation-contract.h -c task_support/probe.c -o .task-build/probe.o
"$cxx" -std=c++17 -fsized-deallocation -I.task-build/production -I. -include allocation-contract.h -c dmallocc.cc -o .task-build/production/dmallocc.o
"$cxx" -std=c++17 -fsized-deallocation -I.task-build/production -I. -include allocation-contract.h task_support/probe.cpp .task-build/probe.o .task-build/production/dmallocc.o .task-build/production/libdmallocth.a -pthread -o .task-build/probe
.task-build/probe
rm -f .task-build/example .task-build/example.log
"$cxx" -std=c++17 -fsized-deallocation -I.task-build/production -I. -include allocation-contract.h examples/release_contract.cpp .task-build/production/dmallocc.o .task-build/production/libdmallocth.a -pthread -o .task-build/example
.task-build/example > .task-build/example.log
cat .task-build/example.log
grep -Fxq RELEASE_CONTRACT_EXAMPLE_OK .task-build/example.log
"$cxx" -std=c++17 -fsized-deallocation -I.task-build/production -I. -include allocation-contract.h checks/release_contract.cpp .task-build/driver.o .task-build/production/dmallocc.o .task-build/production/libdmallocth.a -pthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in classification alignment cpp_entry; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
