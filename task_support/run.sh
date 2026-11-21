#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/lightmap.c; fi
mkdir -p .task-build/production
rm -f .task-build/production/*.o
if [[ $mode == target ]]; then
  test -s render/lightmap.h
  test -s render/lightmap.c
  test -s examples/lightmap_preview.c
fi
for source in math/*.c render/*.c; do
  object=.task-build/production/${source//\//_}.o
  if [[ $mode == target ]]; then
    cc -std=c99 -O2 -I. -Irender -include render/lightmap.h -c "$source" -o "$object"
  else
    cc -std=c99 -O2 -I. -Irender -c "$source" -o "$object"
  fi
done
cc -std=c99 -O2 -I. -Irender -Irender/tests -DPINGO_REFERENCE_DIR='"render/tests/references"' render/tests/test_render.c render/tests/memory_backend.c render/tests/golden.c .task-build/production/*.o -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -I. -Irender tests/lightmap.c task_support/driver.c render/tests/memory_backend.c .task-build/production/*.o -lm -o .task-build/target
cc -std=c99 -O2 -I. -Irender -include render/lightmap.h examples/lightmap_preview.c render/tests/memory_backend.c .task-build/production/*.o -lm -o .task-build/example
cc -std=c99 -O2 task_support/check_ppm.c -o .task-build/check_ppm
example_dir=$(mktemp -d)
trap 'rm -rf "$example_dir"' EXIT
.task-build/example "$example_dir"
.task-build/check_ppm "$example_dir/round0.ppm"
.task-build/check_ppm "$example_dir/round2.ppm"
.task-build/check_ppm "$example_dir/camera.ppm"
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
  for group in surface transport integration; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
echo BASELINE_OK
