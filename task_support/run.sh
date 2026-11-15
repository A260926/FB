#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  test -s mini3d.h
  test -s caustics.h
  test -s caustics.c
  cc -std=c99 -DMINI3D_HEADLESS -I. -x c -fsyntax-only mini3d.h
  cc -std=c99 -DMINI3D_HEADLESS -I. -x c -fsyntax-only caustics.h
  cc -std=c99 -DMINI3D_HEADLESS -I. -x c -fsyntax-only caustics.c
fi
if [[ $mode == target ]]; then test -s tests/caustics_target.c; fi
mkdir -p .task-build
rm -f .task-build/core-*.o
for unit in ./*.c; do
  cc -std=c99 -O2 -DMINI3D_HEADLESS -I. -c "$unit" -o ".task-build/core-$(basename "${unit%.c}").o"
done
cc -std=c99 -O2 -I. support/baseline.c -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -I. task_support/driver.c tests/caustics_target.c .task-build/core-*.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in capture flux camera; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
