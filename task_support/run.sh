#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/target.cpp; fi
mkdir -p .task-build
objects=()
while IFS= read -r -d '' source; do
  [[ $source == src/Main.cpp ]] && continue
  object=".task-build/${source//\//_}.o"
  c++ -std=c++11 -O0 -I. -Isrc -c "$source" -o "$object"
  objects+=("$object")
done < <(find src -name '*.cpp' -print0)
(( ${#objects[@]} > 0 ))
rm -f .task-build/libshader.a
ar rcs .task-build/libshader.a "${objects[@]}"
c++ -std=c++11 -I. -Isrc task_support/baseline.cpp .task-build/libshader.a -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++11 -I. -Isrc task_support/driver.c checks/target.cpp .task-build/libshader.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in read write flow; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
