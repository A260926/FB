#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/grin.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O1 task_support/image_probe.cpp -o .task-build/image-probe
if [[ $mode == target ]]; then
 test -s src/grin.cpp
 test -s minpt_grin.h
 test -s minpt_scene.h
 test -s examples/grin.cpp
 c++ -std=c++17 -O1 -pthread -I. -Itask_support/compat -include minpt_scene.h -include minpt_grin.h -c src/grin.cpp -o .task-build/grin.o
 c++ -std=c++17 -O1 -pthread -I. -Itask_support/compat -Dmain=minpt_cli_entry -c minpt.cpp -o .task-build/scene.o
 c++ -std=c++17 -O1 -pthread -I. -Itask_support/compat minpt.cpp .task-build/grin.o -o .task-build/minpt
else
 c++ -std=c++17 -O1 -pthread -Itask_support/compat minpt.cpp -o .task-build/minpt
fi
if [[ $mode == target ]]; then
  c++ -std=c++17 -O1 -pthread -I. -Itask_support/compat -include minpt_scene.h -include minpt_grin.h checks/grin.cpp task_support/driver.c .task-build/scene.o .task-build/grin.o -o .task-build/target
c++ -std=c++17 -O1 -pthread -I. -Itask_support/compat -include minpt_scene.h -include minpt_grin.h examples/grin.cpp .task-build/scene.o .task-build/grin.o -o .task-build/grin-example
rm -f .task-build/straight.ppm .task-build/bent.ppm
.task-build/grin-example .task-build
test -s .task-build/straight.ppm
test -s .task-build/bent.ppm
! cmp -s .task-build/straight.ppm .task-build/bent.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in trajectory intersection transport; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
rm -f .task-build/baseline.pfm
.task-build/minpt task_support/fixtures/scene.obj '' '' .task-build/baseline.pfm 1 1 0 8 8 0 0 0 0 0 -1 60 1 0 1
.task-build/image-probe .task-build/baseline.pfm
