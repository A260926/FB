#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/tiny_scene_deform_test.cpp; fi
mkdir -p .task-build
c++ -std=c++20 -x c++ -I. -Itask_support -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++20 -O0 -DTINYBVH_NO_SIMD -I. -Iexternal -Itask_support -pthread -c task_support/production.cpp -o .task-build/production.o
extra_objects=(.task-build/production.o)
if [[ -d task_src ]]; then
 source_index=0
 while IFS= read -r source_file; do
  object_file=".task-build/extra_${source_index}.o"
  c++ -std=c++20 -O0 -DTINYBVH_NO_SIMD -I. -Iexternal -Itask_support -pthread -c "$source_file" -o "$object_file"
  extra_objects+=("$object_file")
  source_index=$((source_index+1))
 done < <(find task_src -type f -name '*.cpp' | LC_ALL=C sort)
fi
if [[ $mode == target ]]; then
  c++ -std=c++20 -O0 -DTINYBVH_NO_SIMD -I. -Iexternal -Itask_support -pthread tests/tiny_scene_deform_test.cpp .task-build/driver.o "${extra_objects[@]}" -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in stages instances render; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
c++ -std=c++20 -O0 -DTINYBVH_NO_SIMD -I. -Iexternal -Itask_support -pthread task_support/baseline.cpp "${extra_objects[@]}" -o .task-build/baseline
.task-build/baseline
