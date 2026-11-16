#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/framebuffer_target.c; fi
mkdir -p .task-build
rm -f .task-build/prod-*.o
for src_file in src/*.c; do
 cc -std=c99 -O1 -Iinclude -Isrc -include task_support/build_config.h -c "$src_file" -o ".task-build/prod-$(basename "$src_file" .c).o"
done
cc -std=c99 -O1 -Iinclude -Isrc -include task_support/build_config.h task_support/baseline.c .task-build/prod-*.o -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -O1 -I. -Iinclude -Isrc -include task_support/build_config.h task_support/driver.c tests/framebuffer_target.c .task-build/prod-*.o -lm -o .task-build/target
test -s examples/headless_framebuffer.c
cc -std=c99 -O1 -I. -Iinclude -Isrc -include task_support/build_config.h examples/headless_framebuffer.c .task-build/prod-*.o -lm -o .task-build/framebuffer-example
rm -f .task-build/framebuffer.ppm
.task-build/framebuffer-example
test -s .task-build/framebuffer.ppm
  output_log=$(mktemp "${TMPDIR:-/tmp}/tinygl-target.XXXXXX")
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in routing aliasing state lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
