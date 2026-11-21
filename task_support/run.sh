#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/frame_callbacks.cpp; fi
mkdir -p .task-build
find .task-build -name '*.o' -delete
for unit in src/cal3d/*.cpp; do
  case "$unit" in src/cal3d/calxmlbindings.cpp) continue;; esac
  c++ -std=c++11 -O0 -w -Wno-c++11-narrowing -Isrc -I. -c "$unit" -o ".task-build/$(basename "$unit" .cpp).o"
done
ar rcs .task-build/libcal3d.a .task-build/*.o
c++ -std=c++11 -O0 -w -Wno-c++11-narrowing -Isrc -I. checks/baseline.cpp .task-build/*.o -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/cal3d/framecallbacks.h
test -s src/cal3d/framecallbacks.cpp
test -s examples/frame_callbacks.cpp
c++ -std=c++11 -O0 -Isrc -I. -include src/cal3d/framecallbacks.h task_support/driver.c test/frame_callbacks.cpp .task-build/*.o -o .task-build/target
c++ -std=c++11 -O0 -Isrc -I. -include src/cal3d/framecallbacks.h examples/frame_callbacks.cpp .task-build/*.o -o .task-build/demo
.task-build/demo
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in committed_pose mutation_order registry_clock isolation; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
