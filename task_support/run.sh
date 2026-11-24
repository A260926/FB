#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
test "$mode" = baseline || test "$mode" = target
if [[ $mode == target ]]; then
  for f in tests/oss-fuzz/session_model.h tests/oss-fuzz/session_model.c tests/oss-fuzz/session_model_target.c tests/oss-fuzz/session_model_replay.c; do test -s "$f"; done
fi
mkdir -p .task-build
work=$(mktemp -d .task-build/run.XXXXXX)
san='-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake -S . -B "$work/build" -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="$san -fsanitize-coverage=trace-pc-guard" -DENABLE_DTLS=OFF -DENABLE_OSCORE=OFF -DENABLE_DOCS=OFF -DENABLE_EXAMPLES=OFF -DENABLE_TESTS=OFF -DENABLE_THREAD_SAFE=OFF -DBUILD_SHARED_LIBS=OFF
cmake --build "$work/build" --clean-first -j 4
clang $san -std=gnu11 -c task_support/coverage.c -o "$work/coverage.o"
common=(-I. -Iinclude -I"$work/build" -I"$work/build/include")
clang $san -std=gnu11 "${common[@]}" task_support/baseline.c "$work/build/libcoap-3.a" "$work/coverage.o" -o "$work/baseline"
if [[ $mode == target ]]; then
  clang $san -std=gnu11 "${common[@]}" -include tests/oss-fuzz/session_model.h -c tests/oss-fuzz/session_model.c -o "$work/model.o"
  clang $san -std=gnu11 "${common[@]}" task_support/api_probe.c "$work/model.o" "$work/build/libcoap-3.a" "$work/coverage.o" -o "$work/api"
  "$work/api" "$work/plan-a" "$work/plan-b" "$work/api-report"
  clang $san -std=gnu11 "${common[@]}" tests/oss-fuzz/session_model_replay.c "$work/model.o" "$work/build/libcoap-3.a" "$work/coverage.o" -o "$work/replay"
  "$work/replay" "$work/plan-a" > "$work/report-a"
  "$work/replay" "$work/plan-b" > "$work/report-b"
  test -s "$work/report-a"
  cmp "$work/api-report" "$work/report-a"
  cmp "$work/report-a" "$work/report-b"
  clang $san -std=gnu11 "${common[@]}" tests/oss-fuzz/session_model_target.c task_support/fuzz_driver.c "$work/model.o" "$work/build/libcoap-3.a" "$work/coverage.o" -o "$work/fuzz-entry"
  "$work/fuzz-entry" "$work/plan-a" "$work/plan-b"
  "$work/baseline" --normal
  printf 'TARGET_COMPLETE\n'
else
  "$work/baseline"
fi
