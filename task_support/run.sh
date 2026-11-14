#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/decode/user_macro.cpp; fi
mkdir -p .task-build
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_DISABLE_FIND_PACKAGE_Freetype=ON -DCMAKE_DISABLE_FIND_PACKAGE_Fontconfig=ON > .task-build/configure.log 2>&1 || { tail -30 .task-build/configure.log; exit 1; }
task=task_baseline
if [[ $mode == target ]]; then task=task_target; elif [[ $mode != baseline ]]; then exit 2; fi
cmake --build .task-build --target "$task" -j 4 > .task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
if [[ $mode == baseline ]]; then exec .task-build/task_baseline; fi
set +e
.task-build/task_target > .task-build/target.log 2>&1
status=$?
set -e
cat .task-build/target.log
[[ $status == 0 ]] || exit "$status"
for group in definitions execution lifecycle; do
 grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" .task-build/target.log
done
grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' .task-build/target.log
