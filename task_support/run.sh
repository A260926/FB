#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/mc68000_address_model.cpp; fi
mkdir -p .task-build
sources=()
while IFS= read -r source; do sources+=("$source"); done < task_support/production-sources.txt
while IFS= read -r source; do
  if ! grep -Fxq "$source" task_support/upstream-cpp.txt; then sources+=("$source"); fi
done < <(find src -name '*.cpp' | sort)
objects=()
for source in "${sources[@]}"; do
  object=.task-build/$(printf '%s' "$source" | tr '/.' '__').o
  c++ -std=c++14 -O0 -I. -Isrc -c "$source" -o "$object"
  objects+=("$object")
done
c++ -std=c++14 -O0 -I. -Isrc task_support/baseline.cpp "${objects[@]}" -o .task-build/baseline
c++ -std=c++14 -O0 -I. -Isrc -Itest test/test_dis_mc68000.cpp test/test_dis_helper.cpp test/test_asserter.cpp "${objects[@]}" -o .task-build/upstream
if [[ $mode == target ]]; then
  c++ -std=c++14 -O0 -I. -Isrc -x c++ test/mc68000_address_model.cpp task_support/driver.c -x none "${objects[@]}" -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in structure placement lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
.task-build/upstream > .task-build/upstream.log
printf "UPSTREAM_MC68000_OK\n"
