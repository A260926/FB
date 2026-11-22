#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/encode/roundtrip.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s include/aribcaption/encoder.hpp
test -s src/encoder/encoder.cpp
test -s examples/caption_encode.cpp
c++ -std=c++17 -O1 -I. -Iinclude -Isrc -I.task-build/cmake/include -include include/aribcaption/encoder.hpp -c src/encoder/encoder.cpp -o .task-build/encoder.o
c++ -std=c++17 -O1 -I. -Iinclude -I.task-build/cmake/include examples/caption_encode.cpp .task-build/encoder.o .task-build/cmake/libaribcaption.a -o .task-build/caption_encode
rm -f .task-build/encoded.pes
.task-build/caption_encode .task-build/encoded.pes
test -s .task-build/encoded.pes
.task-build/caption_encode --decode .task-build/encoded.pes
c++ -std=c++17 -O1 -I. -Iinclude -I.task-build/cmake/include -x c++ -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++17 -O1 -I. -Iinclude -I.task-build/cmake/include test/encode/roundtrip.cpp .task-build/driver.o .task-build/encoder.o .task-build/cmake/libaribcaption.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in mapping dictionary rollback wire; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
