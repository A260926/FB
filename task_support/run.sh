#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/morph_pipeline.cpp; fi
mkdir -p .task-build
production_sources=()
while IFS= read -r -d '' source_file; do
  case "$source_file" in */cal3d_wrapper.cpp|*/calxmlbindings.cpp) continue;; esac
  production_sources+=("$source_file")
done < <(find src/cal3d -name '*.cpp' -print0)
${CXX:-c++} -std=c++11 -O1 -DTIXML_USE_STL -I. -Isrc "${production_sources[@]}" task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  ${CXX:-c++} -std=c++11 -O1 -DTIXML_USE_STL -I. -Isrc "${production_sources[@]}" -x c++ task_support/driver.c test/morph_pipeline.cpp -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in representation skinning cache roundtrip; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
