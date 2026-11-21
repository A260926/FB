#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/test_normal_baker.cpp; fi
mkdir -p .task-build
flags=(-std=c++17 -O0 -DNDEBUG -DPOLYMESH_ENABLE_ASSERTIONS -I. -Isrc -Itask_support/vendor)
rm -f .task-build/core-*.o
index=0
while IFS= read -r source || [[ -n "$source" ]]; do
  [[ -z "$source" ]] && continue
  test -s "$source"
  c++ "${flags[@]}" -c "$source" -o ".task-build/core-${index}.o"
  index=$((index+1))
done < task_support/core-sources.txt
[[ $index -gt 0 ]]
c++ "${flags[@]}" task_support/baseline.cpp .task-build/core-*.o -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/polymesh/algorithms/normal_baker.hh
test -s src/polymesh/algorithms/normal_baker.cc
test -s src/polymesh/detail/segment_index.hh
test -s src/polymesh/detail/segment_index.cc
grep -Fxq src/polymesh/algorithms/normal_baker.cc task_support/core-sources.txt
grep -Fxq src/polymesh/detail/segment_index.cc task_support/core-sources.txt
test -s examples/normal_baker.cpp
c++ "${flags[@]}" examples/normal_baker.cpp .task-build/core-*.o -o .task-build/example
rm -f .task-build/normal.ppm
.task-build/example .task-build/normal.ppm
test -s .task-build/normal.ppm
test "$(head -n 1 .task-build/normal.ppm)" = P3
c++ "${flags[@]}" -c task_support/driver.c -o .task-build/driver.o
c++ "${flags[@]}" -include src/polymesh/algorithms/normal_baker.hh -include src/polymesh/detail/segment_index.hh checks/test_normal_baker.cpp .task-build/driver.o .task-build/core-*.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in projection invalidation locality; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
