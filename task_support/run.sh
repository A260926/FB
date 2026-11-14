#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
export CCACHE_DISABLE=1 SCCACHE_DISABLE=1
if [[ $mode == target ]]; then test -s tests/rex2_cases.c; fi
mkdir -p .task-build
cc -std=c11 -O0 -D_GNU_SOURCE -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD -I. -Iinclude -Isrc -Idependencies/zycore/include src/*.c dependencies/zycore/src/*.c dependencies/zycore/src/API/*.c task_support/baseline.c -pthread -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c11 -O0 -D_GNU_SOURCE -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD -I. -Iinclude -Isrc -Idependencies/zycore/include src/*.c dependencies/zycore/src/*.c dependencies/zycore/src/API/*.c task_support/driver.c tests/rex2_cases.c -pthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in prefixes registers addressing; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline; elif [[ $mode != target ]]; then exit 2; fi
