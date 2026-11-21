#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/target.cpp; fi
bash checks/build.sh
if [[ $mode == target ]]; then
  test -s rasterizer/view_texture.h
test -s rasterizer/view_texture.cpp
"${CXX:-c++}" -std=c++20 -O1 -I. -Irasterizer -include algorithm -include limits -include rasterizer/view_texture.h -c rasterizer/view_texture.cpp -o .task-build/view_texture.o
test -s examples/view_texture.cpp
bash checks/link.sh .task-build/example examples/view_texture.cpp .task-build/view_texture.o
rm -f .task-build/view_texture.ppm
.task-build/example .task-build/view_texture.ppm
test -s .task-build/view_texture.ppm
bash checks/link.sh .task-build/target checks/target.cpp task_support/driver.c .task-build/view_texture.o
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in visibility cut projection; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then bash checks/baseline.sh; fi
