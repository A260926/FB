#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/projective_target.c; fi
mkdir -p .task-build
rm -f .task-build/prod-*.o .task-build/cray .task-build/target .task-build/example
if [[ $mode == target ]]; then
  test -s c-ray-fast/projective.c
  test -s c-ray-fast/projective.h
  test -s examples/projective.c
fi
obj_index=0
while IFS= read -r prod_source; do
  obj_index=$((obj_index+1))
  if [[ $mode == target ]]; then
    cc -std=c99 -O1 -pthread -I. -Ic-ray-fast -include c-ray-fast/projective.h -Dmain=task_cli_entry -c "$prod_source" -o ".task-build/prod-$obj_index.o"
  else
    cc -std=c99 -O1 -pthread -I. -Ic-ray-fast -Dmain=task_cli_entry -c "$prod_source" -o ".task-build/prod-$obj_index.o"
  fi
done < <(find c-ray-fast -name '*.c' -type f | LC_ALL=C sort)
test "$obj_index" -gt 0
cc -std=c99 task_support/cli.c .task-build/prod-*.o -pthread -lm -o .task-build/cray
cc -std=c99 task_support/ppm_check.c -o .task-build/ppm-check
if [[ $mode == target ]]; then
  cc -std=c99 -O1 -I. -Ic-ray-fast task_support/driver.c tests/projective_target.c .task-build/prod-*.o -pthread -lm -o .task-build/target
cc -std=c99 -O1 -I. -Ic-ray-fast examples/projective.c .task-build/prod-*.o -pthread -lm -o .task-build/example
rm -f .task-build/example.ppm
.task-build/example .task-build/example.ppm
.task-build/ppm-check .task-build/example.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in parameter hierarchy render; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
rm -f .task-build/normal.ppm
.task-build/cray -s 16x16 -t 1 -r 1 -i c-ray-fast/simple.scn -o .task-build/normal.ppm
.task-build/ppm-check .task-build/normal.ppm
printf 'CRAY_NORMAL_RENDER_OK\n'
