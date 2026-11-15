#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/skin_fit_target.c; fi
mkdir -p .task-build
production_sources=(ufbx.c)
if [[ -d src ]]; then
  while IFS= read -r -d '' source_file; do production_sources+=("$source_file"); done < <(find src -type f -name '*.c' -print0)
fi
production_objects=()
source_number=0
for source_file in "${production_sources[@]}"; do
  object_file=".task-build/production${source_number}.o"
  cc -std=c99 -O0 -I. -c "$source_file" -o "$object_file"
  production_objects+=("$object_file")
  source_number=$((source_number+1))
done
cc -std=c99 -O0 -I. checks/baseline.c "${production_objects[@]}" -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -O0 -I. test/skin_fit_target.c task_support/driver.c "${production_objects[@]}" -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in optimization scene_binding ownership; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
