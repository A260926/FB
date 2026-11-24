#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/reyes.c; fi
rm -rf .task-build
mkdir .task-build
cc -std=c99 -O1 -I. checks/baseline.c -lm -o .task-build/baseline
if [[ $mode == target ]]; then
 for required in mini3d.h reyes.h reyes.c reyes_grid.c examples/reyes.c; do test -s "$required"; done
 cc -std=c99 -O1 -DMINI3D_HEADLESS -I. -c mini3d.c -o .task-build/mini3d.o
 cc -std=c99 -O1 -I. -include mini3d.h -include reyes.h -c reyes.c -o .task-build/reyes.o
 cc -std=c99 -O1 -I. -include mini3d.h -include reyes.h -c reyes_grid.c -o .task-build/reyes_grid.o
fi
if [[ $mode == target ]]; then
  cc -std=c99 -O1 -I. task_support/api_probe.c .task-build/*.o -lm -o .task-build/api-probe
.task-build/api-probe
cc -std=c99 -O1 -I. examples/reyes.c .task-build/*.o -lm -o .task-build/example
mkdir .task-build/demo
.task-build/example .task-build/demo
for artifact in culled.ppm reference.ppm stats.txt; do test -s ".task-build/demo/$artifact"; done
cmp .task-build/demo/culled.ppm .task-build/demo/reference.ppm
cc -std=c99 -O1 -I. checks/reyes.c task_support/driver.c .task-build/*.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in bounds seams visibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
