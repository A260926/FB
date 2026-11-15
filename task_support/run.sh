#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/test_region_extrude.cpp; fi
mkdir -p .task-build
flags=(-std=c++17 -O0 -DNDEBUG -DPOLYMESH_ENABLE_ASSERTIONS -I. -Isrc -Itask_support/vendor)
rm -f .task-build/core-*.o
index=0
while IFS= read -r source || [[ -n "$source" ]]; do
  [[ -z "$source" ]] && continue
  if [[ "$mode" == baseline && "$source" == src/polymesh/algorithms/region_extrude.cc ]]; then continue; fi
  test -s "$source"
  c++ "${flags[@]}" -c "$source" -o ".task-build/core-${index}.o"
  index=$((index+1))
done < task_support/core-sources.txt
[[ $index -gt 0 ]]
c++ "${flags[@]}" task_support/baseline.cpp .task-build/core-*.o -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/polymesh/algorithms/region_extrude.hh
test -s src/polymesh/algorithms/region_extrude.cc
grep -Fxq src/polymesh/algorithms/region_extrude.cc task_support/core-sources.txt
test -s examples/region_extrude.cpp
c++ "${flags[@]}" examples/region_extrude.cpp .task-build/core-*.o -o .task-build/example
rm -f .task-build/extruded.obj
.task-build/example .task-build/extruded.obj
test -s .task-build/extruded.obj
grep -q '^f ' .task-build/extruded.obj
c++ "${flags[@]}" -c task_support/driver.c -o .task-build/driver.o
c++ "${flags[@]}" checks/test_region_extrude.cpp .task-build/driver.o .task-build/core-*.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in topology attributes persistence; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
