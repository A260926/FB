#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/fuzz/scope_tests.c; fi
mkdir -p .task-build
cmake -S . -B .task-build/engine -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Debug -DJERRY_CMDLINE=OFF -DJERRY_EXT=ON -DJERRY_PORT=ON -DENABLE_AMALGAM=OFF -DUNITTESTS=OFF -DDOCTESTS=OFF -DJERRY_GLOBAL_HEAP_SIZE=4096 -DCMAKE_C_FLAGS="-O0 -g -fsanitize=address,undefined" -DEXTERNAL_COMPILE_FLAGS="-Wno-enum-enum-conversion;-Wno-unterminated-string-initialization" > .task-build/configure.log 2>&1
cmake --build .task-build/engine --clean-first -j4 > .task-build/build.log 2>&1
cc -std=c99 -g -fsanitize=address,undefined -Ijerry-core/include -Ijerry-ext/include task_support/baseline.c .task-build/engine/lib/libjerry-ext.a .task-build/engine/lib/libjerry-core.a .task-build/engine/lib/libjerry-port.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s tests/fuzz/scope_model.c
test -s tests/fuzz/scope_model.h
test -s tests/fuzz/scope_replay.c
test -s tests/fuzz/scope_replay.h
test -s tests/fuzz/scope_fuzz.c
test -s tests/fuzz/scope_demo.c
cc -std=c99 -g -fsanitize=address,undefined -I. -Ijerry-core/include -Ijerry-ext/include -include tests/fuzz/scope_model.h -include tests/fuzz/scope_replay.h tests/fuzz/scope_model.c tests/fuzz/scope_replay.c tests/fuzz/scope_fuzz.c task_support/api_consumer.c .task-build/engine/lib/libjerry-ext.a .task-build/engine/lib/libjerry-core.a .task-build/engine/lib/libjerry-port.a -lm -o .task-build/api-consumer
.task-build/api-consumer
cc -std=c99 -g -fsanitize=address,undefined -I. -Ijerry-core/include -Ijerry-ext/include -include tests/fuzz/scope_model.h -include tests/fuzz/scope_replay.h tests/fuzz/scope_model.c tests/fuzz/scope_replay.c tests/fuzz/scope_fuzz.c tests/fuzz/scope_demo.c .task-build/engine/lib/libjerry-ext.a .task-build/engine/lib/libjerry-core.a .task-build/engine/lib/libjerry-port.a -lm -o .task-build/demo
demo_log=$(mktemp)
.task-build/demo >"$demo_log"
cat "$demo_log"
grep -q '^SCOPE_DEMO_OK$' "$demo_log"
cc -std=c99 -g -fsanitize=address,undefined -I. -Ijerry-core/include -Ijerry-ext/include -include tests/fuzz/scope_model.h -include tests/fuzz/scope_replay.h tests/fuzz/scope_model.c tests/fuzz/scope_replay.c tests/fuzz/scope_fuzz.c tests/fuzz/scope_tests.c task_support/driver.c .task-build/engine/lib/libjerry-ext.a .task-build/engine/lib/libjerry-core.a .task-build/engine/lib/libjerry-port.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in scopes exceptions replay diagnostics; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
