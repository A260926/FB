#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/impostor.cpp; fi
mkdir -p .task-build/production
rm -f .task-build/production/*.o
sources=(src/TriangleDrawer.cpp src/Triangle.cpp src/Graphics3D.cpp src/LineDrawer.cpp src/ClipSpace.cpp)
if [[ $mode == target ]]; then
  test -s src/Impostor.h
  test -s src/Impostor.cpp
  test -s examples/impostor_preview.cpp
  sources+=(src/Impostor.cpp)
fi
for source in "${sources[@]}"; do
  object=.task-build/production/${source//\//_}.o
  if [[ $mode == target ]]; then
    c++ -std=c++17 -O2 -I. -Isrc -include src/Impostor.h -c "$source" -o "$object"
  else
    c++ -std=c++17 -O2 -I. -Isrc -c "$source" -o "$object"
  fi
done
c++ -std=c++17 -O2 -I. -Isrc task_support/baseline.cpp .task-build/production/*.o -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -O2 -I. -Isrc tests/impostor.cpp task_support/driver.c .task-build/production/*.o -o .task-build/target
c++ -std=c++17 -O2 -I. -Isrc -include src/Impostor.h examples/impostor_preview.cpp .task-build/production/*.o -o .task-build/example
cc -std=c99 -O2 task_support/check_ppm.c -o .task-build/check_ppm
example_dir=$(mktemp -d)
trap 'rm -rf "$example_dir"' EXIT
.task-build/example "$example_dir"
.task-build/check_ppm "$example_dir/view0.ppm"
.task-build/check_ppm "$example_dir/view1.ppm"
rm -rf "$example_dir"
trap - EXIT
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in capture reprojection composition; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
