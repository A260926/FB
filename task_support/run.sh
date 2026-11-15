#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  test -s projective_baker.h
  test -s projective_baker.cpp
  c++ -std=c++20 -I. -x c++ -fsyntax-only projective_baker.h
  c++ -std=c++20 -I. -x c++ -fsyntax-only projective_baker.cpp
fi
if [[ $mode == target ]]; then test -s tests/projective_baker_cases.cpp; fi
rm -rf .task-build
mkdir -p .task-build
for source_file in *.cpp; do
  [[ $source_file == main.cpp ]] && continue
  c++ -std=c++20 -O2 -I. -c "$source_file" -o ".task-build/${source_file%.cpp}.o"
done
c++ -std=c++20 -O2 -I. task_support/baseline.cpp .task-build/*.o -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++20 -O2 -I. task_support/driver.c tests/projective_baker_cases.cpp .task-build/*.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in projection visibility lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
