#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s src/tests/instance_media.cpp; fi
bash task_support/build.sh OFF
if [[ $mode == target ]]; then
  test -s src/media/instancegrid.h
test -s src/media/instancegrid.cpp
test -s scenes/instance-smoke.pbrt
cmake -S . -B .task-build/engine -DTASK_TARGET=ON > .task-build/target-configure.log 2>&1 || { tail -30 .task-build/target-configure.log; exit 1; }
cmake --build .task-build/engine --target pbrt_exe task_target -j 6 > .task-build/target-build.log 2>&1 || { tail -30 .task-build/target-build.log; exit 1; }
rm -f .task-build/instance-smoke.pfm
.task-build/engine/pbrt --nthreads 1 --outfile .task-build/instance-smoke.pfm scenes/instance-smoke.pbrt > .task-build/example.log 2>&1 || { tail -30 .task-build/example.log; exit 1; }
test -s .task-build/instance-smoke.pfm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in boundary metric transport; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode != target ]]; then
 .task-build/baseline
 rm -f .task-build/ordinary-smoke.pfm
 .task-build/engine/pbrt --nthreads 1 --outfile .task-build/ordinary-smoke.pfm task_support/ordinary-smoke.pbrt > .task-build/render.log 2>&1 || { tail -30 .task-build/render.log; exit 1; }
 test -s .task-build/ordinary-smoke.pfm
 echo ORDINARY_SMOKE_RENDER_COMPLETE
else
 .task-build/baseline
fi
