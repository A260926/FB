#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/ordered_reader.cpp; fi
mkdir -p .task-build
cmake -S task_support/cmake -B .task-build/libs -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build .task-build/libs --target ordered_baseline -j 4
if [[ $mode == target ]]; then
  test -s matroska/KaxOrderedReader.h
test -s src/KaxOrderedIndex.cpp
test -s src/KaxOrderedReader.cpp
test -s examples/ordered_reader.cpp
c++ -std=c++17 -I. -Ithird_party/libebml -I.task-build/libs/ebml -I.task-build/libs/matroska -include matroska/KaxOrderedReader.h -c src/KaxOrderedIndex.cpp -o .task-build/index.o
c++ -std=c++17 -I. -Ithird_party/libebml -I.task-build/libs/ebml -I.task-build/libs/matroska -include matroska/KaxOrderedReader.h -c src/KaxOrderedReader.cpp -o .task-build/reader.o
c++ -std=c++17 -I. -Ithird_party/libebml -I.task-build/libs/ebml -I.task-build/libs/matroska task_support/api_consumer.cpp .task-build/index.o .task-build/reader.o .task-build/libs/matroska/libmatroska.a .task-build/libs/ebml/libebml.a -o .task-build/consumer
rm -f .task-build/ordered-input.mkv .task-build/example.txt
.task-build/consumer
c++ -std=c++17 -I. -Ithird_party/libebml -I.task-build/libs/ebml -I.task-build/libs/matroska -x c++ -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++17 -I. -Ithird_party/libebml -I.task-build/libs/ebml -I.task-build/libs/matroska test/ordered_reader.cpp .task-build/driver.o .task-build/index.o .task-build/reader.o .task-build/libs/matroska/libmatroska.a .task-build/libs/ebml/libebml.a -o .task-build/target
c++ -std=c++17 -I. -Ithird_party/libebml -I.task-build/libs/ebml -I.task-build/libs/matroska examples/ordered_reader.cpp .task-build/index.o .task-build/reader.o .task-build/libs/matroska/libmatroska.a .task-build/libs/ebml/libebml.a -o .task-build/example
.task-build/example .task-build/example.txt 0 91 71 .task-build/ordered-input.mkv
test -s .task-build/example.txt
.task-build/consumer --verify .task-build/example.txt
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in linking seek io; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/libs/ordered_baseline
