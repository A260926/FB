#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/heightfield_target.cpp; fi
bash task_support/build.sh "$mode"
if [[ $mode == target ]]; then
  test -s examples/heightfield_render.cpp
${CXX:-c++} -std=c++17 -O1 -DTINSEL_TARGET_BUILD -I. -Isrc -include task_support/compat.h -include src/heightfield.h task_support/driver.c tests/heightfield_target.cpp .task-build/production.a -o .task-build/target
${CXX:-c++} -std=c++17 -O1 -DTINSEL_TARGET_BUILD -I. -Isrc -include task_support/compat.h -include src/heightfield.h examples/heightfield_render.cpp .task-build/production.a -o .task-build/example
mkdir -p .task-build/example-output
rm -f .task-build/example-output/before.ppm .task-build/example-output/after.ppm .task-build/example-output/retained.ppm
.task-build/example .task-build/example-output
for image in before after retained; do test -s ".task-build/example-output/$image.ppm"; done
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in intersections updates lighting; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
