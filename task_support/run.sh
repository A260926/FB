#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/irregular_shadow.c; fi
rm -rf .task-build
mkdir .task-build
find src -name '*.c' | LC_ALL=C sort > .task-build/production-sources
: > .task-build/production.rsp
source_count=0
while IFS= read -r file; do
 test -s "$file"
 obj=".task-build/production-$source_count.o"
 if [[ "$file" == src/irregular_shadow.c ]]; then
  cc -std=c99 -O1 -Werror=implicit-function-declaration -I. -Iinclude -Isrc -include src/irregular_shadow.h -c "$file" -o "$obj"
 else
  cc -std=c99 -O1 -Werror=implicit-function-declaration -I. -Iinclude -Isrc -c "$file" -o "$obj"
 fi
 printf '%s\n' "$obj" >> .task-build/production.rsp
 source_count=$((source_count+1))
done < .task-build/production-sources
[[ $source_count -ge 5 ]]
cc -std=c99 -O1 -I. -Iinclude -Isrc task_support/baseline.c @.task-build/production.rsp -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  for file in src/irregular_shadow.h src/irregular_shadow.c examples/irregular_shadow.c; do test -s "$file"; done
cc -std=c99 -O1 -Werror=implicit-function-declaration -I. -Iinclude -Isrc task_support/api_consumer.c @.task-build/production.rsp -lm -o .task-build/api-consumer
.task-build/api-consumer
cc -std=c99 -O1 -Werror=implicit-function-declaration -I. -Iinclude -Isrc task_support/driver.c tests/irregular_shadow.c @.task-build/production.rsp -lm -o .task-build/target
cc -std=c99 -O1 -Werror=implicit-function-declaration -I. -Iinclude -Isrc examples/irregular_shadow.c @.task-build/production.rsp -lm -o .task-build/example
rm -f .task-build/irregular_shadow.ppm
.task-build/example .task-build/irregular_shadow.ppm
test -s .task-build/irregular_shadow.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in capture visibility lighting hierarchy; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
