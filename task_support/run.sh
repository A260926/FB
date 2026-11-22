#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/frame_store.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s matroska/KaxFrameStore.h
test -s src/KaxFrameStore.cpp
test -s test/frame_store_example.cpp
c++ -std=c++17 -O1 -I. -Ivendor/libebml -Ivendor/utfcpp/source -Itask_support/include -include matroska/KaxFrameStore.h task_support/api_consumer.cpp .task-build/libmedia.a -o .task-build/api-consumer
.task-build/api-consumer
c++ -std=c++17 -O1 -I. -Ivendor/libebml -Ivendor/utfcpp/source -Itask_support/include -include matroska/KaxFrameStore.h test/frame_store_example.cpp .task-build/libmedia.a -o .task-build/example
rm -f .task-build/frame-store-example.mkv
.task-build/example
test -s .task-build/frame-store-example.mkv
c++ -std=c++17 task_support/check_example.cpp -o .task-build/check-example
.task-build/check-example .task-build/frame-store-example.mkv
c++ -std=c++17 -O1 -I. -Ivendor/libebml -Ivendor/utfcpp/source -Itask_support/include -include matroska/KaxFrameStore.h -x c++ task_support/driver.c test/frame_store.cpp -x none .task-build/libmedia.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in transaction streaming lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
c++ -std=c++17 -O1 -I. -Ivendor/libebml -Ivendor/utfcpp/source -Itask_support/include task_support/baseline.cpp .task-build/libmedia.a -o .task-build/baseline
.task-build/baseline
