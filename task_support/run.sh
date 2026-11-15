#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/metaball_target.c; fi
mkdir -p .task-build
find .task-build -name '*.o' -delete
for unit in c-ray-fast/*.c; do
  base=$(basename "$unit" .c)
  cc -std=gnu99 -O1 -I. -Dmain=c_ray_cli_main -c "$unit" -o ".task-build/prod-$base.o"
done
extra_sources=$(find c-ray-fast -maxdepth 1 -name '*.c' ! -name 'c-ray-fast.c' -print)
cc -std=gnu99 -O1 -I. task_support/baseline.c $extra_sources -lm -lpthread -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=gnu99 -O1 -I. -c task_support/driver.c -o .task-build/driver.o
cc -std=gnu99 -O1 -I. -c tests/metaball_target.c -o .task-build/test.o
cc .task-build/prod-*.o .task-build/driver.o .task-build/test.o -lm -lpthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in roots acceleration render; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
