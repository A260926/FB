#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/selection.c; fi
mkdir -p .task-build
cc -std=c99 -O2 -I. -Iinclude src/gl_api.c src/raster.c src/textures.c src/vbo.c src/lists.c task_support/baseline.c -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/selection.h
test -s src/selection.c
test -s examples/select.c
cc -std=c99 -O2 -I. -Iinclude -include src/selection.h src/gl_api.c src/raster.c src/textures.c src/vbo.c src/lists.c src/selection.c task_support/driver.c tests/selection.c -lm -o .task-build/target
cc -std=c99 -O2 -I. -Iinclude -include src/selection.h src/gl_api.c src/raster.c src/textures.c src/vbo.c src/lists.c src/selection.c examples/select.c -lm -o .task-build/select_example
.task-build/select_example
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in geometry records replay; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
