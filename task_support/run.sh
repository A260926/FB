#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  test -s include/FastBVH/SceneGraph.h
  c++ -std=c++14 -pthread -Iinclude -I. -x c++ -fsyntax-only include/FastBVH/SceneGraph.h
fi
if [[ $mode == target ]]; then test -s tests/scene_graph_cases.cpp; fi
mkdir -p .task-build
compile_link() {
  output_file=$1; shift
  while IFS= read -r source_file; do set -- "$@" "$source_file"; done < <(find include src -name '*.cpp' | sort)
  c++ -std=c++14 -O2 -pthread -Iinclude -I. "$@" -o "$output_file"
}
compile_link .task-build/baseline task_support/baseline.cpp
if [[ $mode == target ]]; then
  compile_link .task-build/target task_support/driver.c tests/scene_graph_cases.cpp
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in dag paths updates render; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
