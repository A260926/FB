#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/tile_pass.cpp; fi
mkdir -p .task-build
objects=()
for source in src/*.cpp; do
  object=".task-build/$(basename "$source" .cpp).o"
  "${CXX:-c++}" -std=c++17 -O1 -Wno-unused-value -Wno-deprecated-declarations -Isrc -iquote . -c "$source" -o "$object"
  objects+=("$object")
done
"${CXX:-c++}" -std=c++17 -O1 -Isrc -iquote . task_support/baseline.cpp "${objects[@]}" -o .task-build/baseline
if [[ $mode == target ]]; then
  "${CXX:-c++}" -std=c++17 -O1 -Isrc -iquote . -x c++ task_support/driver.c task_tests/tile_pass.cpp -x none "${objects[@]}" -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in ordering depth reuse; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
