#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/mirror_target.cpp; fi
mkdir -p .task-build
c++ -std=c++11 -O1 -include cstring -I. -Ivendor/glm -c task_support/driver.c -o .task-build/driver.o
objects=()
for source in SkeletalAnimation/*.cpp; do object=".task-build/$(basename "$source" .cpp).o"; c++ -std=c++11 -O1 -include cstring -I. -Ivendor/glm -c "$source" -o "$object"; objects+=("$object"); done
c++ -std=c++11 -O1 -include cstring -I. -Ivendor/glm task_support/baseline.cpp "${objects[@]}" -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++11 -O1 -include cstring -I. -Ivendor/glm tests/mirror_target.cpp "${objects[@]}" .task-build/driver.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in spaces mixed_hierarchy lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
