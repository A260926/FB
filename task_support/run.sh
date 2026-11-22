#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/ltr_cases.c; fi
cmake -S . -B .task-build/kvz -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=OFF -DBUILD_KVAZAAR_BINARY=OFF -DUSE_CRYPTO=OFF -DGIT_SUBMODULE=OFF -DCMAKE_BUILD_TYPE=Release -DKVZ_BIT_DEPTHS=8
cmake --build .task-build/kvz --target kvazaar -j 4
if [[ $mode == target ]]; then
  test -s task_support/require_ltr_api.c
  cc -std=c11 -Werror=implicit-function-declaration -DKVZ_STATIC_LIB -I src task_support/require_ltr_api.c .task-build/kvz/libkvazaar.a -lm -lpthread -o .task-build/required-ltr-api
  .task-build/required-ltr-api
fi
cc -std=c11 -DKVZ_STATIC_LIB -I src task_support/baseline.c .task-build/kvz/libkvazaar.a -lm -lpthread -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s examples/ltr_example.c
cc -std=c11 -DKVZ_STATIC_LIB -I . -I src tests/ltr_cases.c task_support/driver.c .task-build/kvz/libkvazaar.a -lm -lpthread -o .task-build/target
cc -std=c11 -DKVZ_STATIC_LIB -I src examples/ltr_example.c .task-build/kvz/libkvazaar.a -lm -lpthread -o .task-build/ltr-example
rm -f .task-build/ltr-example.h265
.task-build/ltr-example .task-build/ltr-example.h265
test -s .task-build/ltr-example.h265
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in slots syntax pipeline compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
