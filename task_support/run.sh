#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/theora_edit.c; fi
mkdir -p .task-build
cmake -S . -B .task-build/ogg -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF
cmake --build .task-build/ogg -j 4
cc -std=c99 -Iinclude -I.task-build/ogg/include -Itask_support task_support/baseline.c .task-build/ogg/libogg.a -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s include/ogg/theora_edit.h
test -s src/theora_index.c
test -s src/theora_edit.c
test -s examples/theora_edit.c
cc -std=c99 -Iinclude -I.task-build/ogg/include -I. -include include/ogg/theora_edit.h -c src/theora_index.c -o .task-build/theora_index.o
cc -std=c99 -Iinclude -I.task-build/ogg/include -I. -include include/ogg/theora_edit.h -c src/theora_edit.c -o .task-build/theora_edit.o
cc -std=c99 -Iinclude -I.task-build/ogg/include -Itask_support task_support/api_consumer.c .task-build/theora_index.o .task-build/theora_edit.o .task-build/ogg/libogg.a -o .task-build/consumer
rm -f .task-build/consumer.ogv .task-build/example.ogv
.task-build/consumer
cc -std=c99 -Iinclude -I.task-build/ogg/include -I. task_support/driver.c tests/theora_edit.c .task-build/theora_index.o .task-build/theora_edit.o .task-build/ogg/libogg.a -o .task-build/target
cc -std=c99 -Iinclude -I.task-build/ogg/include examples/theora_edit.c .task-build/theora_index.o .task-build/theora_edit.o .task-build/ogg/libogg.a -o .task-build/example
.task-build/example tests/fixtures/frames.ogv .task-build/example.ogv 0 0 2
test -s .task-build/example.ogv
.task-build/consumer --verify .task-build/example.ogv
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in anchors montage errors; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
