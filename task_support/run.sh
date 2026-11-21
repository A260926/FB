#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/shader_vm.c; fi
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Release >.task-configure.log 2>&1
if [[ $mode == target && -d .task-build/CMakeFiles/task_cpu.dir ]]; then
 find .task-build/CMakeFiles/task_cpu.dir -name 'shader.c.o' -delete
 find .task-build/CMakeFiles/task_cpu.dir -name 'shader_vm.c.o' -delete
fi
cmake --build .task-build --target task_baseline -j 2 >.task-build.log 2>&1
if [[ $mode == target ]]; then
  test -s include/TGL/glshader.h
test -s src/shader.c
test -s src/shader_vm.c
test -s examples/shader_vm.c
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Release >>.task-configure.log 2>&1
find .task-build/CMakeFiles/target.dir -name '*.o' -delete
find .task-build/CMakeFiles/shader_vm_example.dir -name '*.o' -delete
cmake --build .task-build --target target shader_vm_example -j 2 >>.task-build.log 2>&1
rm -rf .task-build/shader-output
mkdir .task-build/shader-output
.task-build/shader_vm_example .task-build/shader-output
test -s .task-build/shader-output/surface.ppm
test -s .task-build/shader-output/trace.csv
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in verify control render; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/task_baseline
