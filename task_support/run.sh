#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/inline_source.cpp; fi
rm -rf .task-build
mkdir -p .task-build
for file in src/*.cpp; do c++ -std=c++17 -O0 -Isrc -I. -c "$file" -o ".task-build/$(basename "${file%.cpp}").o"; done
c++ -std=c++17 -O0 -Isrc -I. task_support/baseline.cpp .task-build/*.o -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/PDB_InlineSource.cpp
c++ -std=c++17 -O0 -Isrc -I. -x c++ task_support/driver.c tests/inline_source.cpp -x none .task-build/*.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in annotations module_identity nested_ranges; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
