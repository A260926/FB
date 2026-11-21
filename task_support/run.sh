#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/singularity_field.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O0 -w -I. -Iinclude -Iexternal/eigen task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s include/cinolib/singularity_field.h
test -s include/cinolib/singularity_field.cpp
test -s examples/singularity_field.cpp
c++ -std=c++17 -O0 -w -I. -Iinclude -Iexternal/eigen -include cinolib/singularity_field.h -x c++ task_support/driver.c tests/singularity_field.cpp include/cinolib/singularity_field.cpp -o .task-build/target
c++ -std=c++17 -O0 -w -I. -Iinclude -Iexternal/eigen -include cinolib/singularity_field.h examples/singularity_field.cpp include/cinolib/singularity_field.cpp -o .task-build/example
rm -f .task-build/directions.field
.task-build/example .task-build/directions.field
test -s .task-build/directions.field
grep -Eq '^VECTOR_FIELD [1-9][0-9]*$' .task-build/directions.field
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in connection singularities periods reuse; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
