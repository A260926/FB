#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/page_shield_contract.cc; fi
task_work=$(mktemp -d "${TMPDIR:-/tmp}/snmalloc-run.XXXXXX")
printf 'TASK_BUILD_DIR=%s\n' "$task_work"
clang++ -std=c++20 -pthread -DSNMALLOC_USE_WAIT_ON_ADDRESS=0 -DSNMALLOC_CHECK_CLIENT -Isrc task_support/baseline.cc -o "$task_work/baseline"
if [[ $mode == target ]]; then
  test -s src/snmalloc/page_shield.h
test -s src/page_shield.cc
test -s examples/page_shield.cc
clang++ -std=c++20 -pthread -DSNMALLOC_USE_WAIT_ON_ADDRESS=0 -DSNMALLOC_CHECK_CLIENT -Isrc -DSNMALLOC_ENABLE_PAGE_SHIELD -include src/snmalloc/page_shield.h -c src/page_shield.cc -o "$task_work/page-shield.o"
clang++ -std=c++20 -pthread -DSNMALLOC_USE_WAIT_ON_ADDRESS=0 -DSNMALLOC_CHECK_CLIENT -Isrc -DSNMALLOC_ENABLE_PAGE_SHIELD -include src/snmalloc/page_shield.h task_support/api_consumer.cc "$task_work/page-shield.o" -o "$task_work/api-consumer"
"$task_work/api-consumer"
clang++ -std=c++20 -pthread -DSNMALLOC_USE_WAIT_ON_ADDRESS=0 -DSNMALLOC_CHECK_CLIENT -Isrc -DSNMALLOC_ENABLE_PAGE_SHIELD -include src/snmalloc/page_shield.h examples/page_shield.cc "$task_work/page-shield.o" -o "$task_work/example"
"$task_work/example" > "$task_work/example.log"
cat "$task_work/example.log"
grep -qx SNMALLOC_PAGE_SHIELD_EXAMPLE_OK "$task_work/example.log"
clang++ -std=c++20 -pthread -DSNMALLOC_USE_WAIT_ON_ADDRESS=0 -DSNMALLOC_CHECK_CLIENT -Isrc -DSNMALLOC_ENABLE_PAGE_SHIELD -include src/snmalloc/page_shield.h task_support/baseline.cc "$task_work/page-shield.o" -o "$task_work/enabled-healthy"
"$task_work/enabled-healthy"
clang++ -std=c++20 -pthread -DSNMALLOC_USE_WAIT_ON_ADDRESS=0 -DSNMALLOC_CHECK_CLIENT -Isrc -DSNMALLOC_ENABLE_PAGE_SHIELD -include src/snmalloc/page_shield.h -I. task_support/driver.c tests/page_shield_contract.cc "$task_work/page-shield.o" -o "$task_work/target"
  output_log="$task_work/target.log"
  set +e
  "$task_work/target" >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in geometry recycling handoff; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
"$task_work/baseline"
