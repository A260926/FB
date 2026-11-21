#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/surfels.cpp; fi
mkdir -p .task-build
production_sources=(src/math.cpp src/model.cpp)
while IFS= read -r production_path; do
  case "$production_path" in src/math.cpp|src/model.cpp|src/rasterizer.cpp) ;; *) production_sources+=("$production_path");; esac
done < <(find src -type f -name '*.cpp' | LC_ALL=C sort)
c++ -std=c++20 -O2 -DRASTERIZER_HEADLESS -I. -include task_support/compat.h task_support/baseline.cpp "${production_sources[@]}" -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/surfels.h
test -s src/surfels.cpp
test -s src/pipeline.h
test -s examples/surfels.cpp
c++ -std=c++20 -O2 -DRASTERIZER_HEADLESS -I. -include task_support/compat.h -include src/surfels.h -include src/pipeline.h task_support/driver.c tests/surfels.cpp src/rasterizer.cpp "${production_sources[@]}" -o .task-build/target
c++ -std=c++20 -O2 -DRASTERIZER_HEADLESS -I. -include task_support/compat.h -include src/surfels.h -include src/pipeline.h examples/surfels.cpp src/rasterizer.cpp "${production_sources[@]}" -o .task-build/example
example_dir=$(mktemp -d)
.task-build/example "$example_dir/surfels.ppm"
test -s "$example_dir/surfels.ppm"
rm -rf "$example_dir"
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in footprint reconstruction integration; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
