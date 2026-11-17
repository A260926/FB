#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/mesh_binding_target.cpp; fi
mkdir -p .task-build
CXX=${CXX:-c++}
flags=(-std=c++11 -w -include cstring -I. -Ivendor/glm)
objects=()
for src in SkeletalAnimation/*.cpp; do obj=.task-build/$(basename "$src").o; "$CXX" "${flags[@]}" -c "$src" -o "$obj"; objects+=("$obj"); done
"$CXX" "${flags[@]}" task_support/baseline.cpp "${objects[@]}" -o .task-build/baseline
if [[ $mode == target ]]; then
  "$CXX" "${flags[@]}" -x c++ -c task_support/driver.c -o .task-build/driver.o
"$CXX" "${flags[@]}" tests/mesh_binding_target.cpp .task-build/driver.o "${objects[@]}" -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in palettes assets lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
