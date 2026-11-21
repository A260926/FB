#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/compound_lens.cpp; fi
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >.task-configure.log 2>&1
if [[ $mode == target && -d .task-build/CMakeFiles/task_cpu.dir ]]; then
  find .task-build/CMakeFiles/task_cpu.dir -name 'yocto_lens.cpp.o' -delete
fi
cmake --build .task-build --target task_baseline -j 2 >.task-build.log 2>&1
if [[ $mode == target ]]; then
  test -s libs/yocto/yocto_lens.h
test -s libs/yocto/yocto_lens.cpp
test -s examples/compound_lens.cpp
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >>.task-configure.log 2>&1
find .task-build/CMakeFiles/target.dir -name '*.o' -delete
find .task-build/CMakeFiles/compound_lens_example.dir -name '*.o' -delete
cmake --build .task-build --target target compound_lens_example -j 2 >>.task-build.log 2>&1
rm -rf .task-build/lens-output
mkdir .task-build/lens-output
.task-build/compound_lens_example .task-build/lens-output
test -s .task-build/lens-output/irradiance.pfm
test -s .task-build/lens-output/weight.pfm
test -s .task-build/lens-output/valid.pgm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in optics focus film; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/task_baseline
