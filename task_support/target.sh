#!/bin/bash
set -euo pipefail
test -s tests/clone_cow.c
test -s example/clone-cow-cpu.c
for variant in plain threaded; do
  build_dir=".task-build/$variant"
  cc -std=c99 -Werror=implicit-function-declaration -Werror=int-conversion -I"$build_dir/src" task_support/api_probe.c "$build_dir/libanim.a" -pthread -lm -o "$build_dir/api_probe"
  "$build_dir/api_probe"
  cc -std=c99 -D_POSIX_C_SOURCE=200809L -I. -I"$build_dir/src" task_support/driver.c tests/clone_cow.c "$build_dir/libanim.a" -pthread -lm -o "$build_dir/tests"
  cc -std=c99 -D_POSIX_C_SOURCE=200809L -I"$build_dir/src" example/clone-cow-cpu.c "$build_dir/libanim.a" -pthread -lm -o "$build_dir/example"
  rm -f "$build_dir/example.log"
  "$build_dir/example" > "$build_dir/example.log"
  cat "$build_dir/example.log"
  grep -qx CLONE_COW_DEMO_OK "$build_dir/example.log"
done
set +e
.task-build/threaded/tests > .task-build/threaded/tests.log 2>&1
test_status=$?
set -e
cat .task-build/threaded/tests.log
[[ $test_status == 0 ]] || exit "$test_status"
for group in sharing playback lifetime; do
  grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" .task-build/threaded/tests.log
done
grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' .task-build/threaded/tests.log
cp .task-build/plain/tests .task-build/target
