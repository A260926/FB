#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/proof_contract.cpp; fi
mkdir -p .task-build/generated
cmake -S . -B .task-build/cmake -DFLATBUFFERS_BUILD_TESTS=OFF -DFLATBUFFERS_BUILD_FLATLIB=OFF -DFLATBUFFERS_BUILD_FLATHASH=OFF -DFLATBUFFERS_BUILD_GRPCTEST=OFF -DFLATBUFFERS_INSTALL=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build/cmake --target flatc --clean-first -j 4
rm -f .task-build/generated/proof_generated.h
.task-build/cmake/flatc --cpp -o .task-build/generated task_support/proof.fbs
test -s .task-build/generated/proof_generated.h
c++ -std=c++17 -O0 -Iinclude -I.task-build/generated -Itask_support task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s samples/proof_verification.cpp
test -d tests/proof_schema
mkdir -p .task-build/generated/cases
schema_count=0
while IFS= read -r p; do
  schema_count=$((schema_count + 1))
  generated_name=$(basename "$p" .fbs)_generated.h
  rm -f ".task-build/generated/cases/$generated_name"
  .task-build/cmake/flatc --cpp -I tests/proof_schema -o .task-build/generated/cases "$p"
  test -s ".task-build/generated/cases/$generated_name"
done < <(find tests/proof_schema -type f -name '*.fbs')
test "$schema_count" -gt 0
c++ -std=c++17 -O0 -Iinclude -I.task-build/generated task_support/api_consumer.cpp -o .task-build/api_consumer
.task-build/api_consumer
c++ -std=c++17 -O0 -I. -Iinclude -I.task-build/generated -I.task-build/generated/cases task_support/driver.c tests/proof_contract.cpp -o .task-build/target
c++ -std=c++17 -O0 -I. -Iinclude -I.task-build/generated -I.task-build/generated/cases samples/proof_verification.cpp -o .task-build/example
.task-build/example
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in sharing depth nested reports; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
