#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/causal_target.cpp; fi
mkdir -p .task-build
core=(src/mesh.cpp src/bvh.cpp src/scene.cpp src/render.cpp src/perlin.cpp src/platform.cpp)
extra=()
if [[ $mode == target ]]; then test -s src/causal.h; test -s src/causal.cpp; test -s examples/causal_render.cpp; extra=(src/causal.cpp); fi
c++ -std=c++17 -O2 -w -include cstring -include cstdio -I. "${core[@]}" ${extra[@]+"${extra[@]}"} task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -O2 -w -include cstring -include cstdio -I. "${core[@]}" "${extra[@]}" tests/causal_target.cpp task_support/driver.c -o .task-build/target
c++ -std=c++17 -O2 -w -include cstring -include cstdio -I. "${core[@]}" "${extra[@]}" examples/causal_render.cpp -o .task-build/example
example_dir=$(mktemp -d)
.task-build/example "$example_dir"
test -s "$example_dir/causal.ppm"
rm -r "$example_dir"
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in intersections density transport; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
