#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/target.cpp; fi
mkdir -p .task-build/obj
find rasterizer external/stb_image -name '*.cpp' -type f | LC_ALL=C sort > .task-build/sources
while IFS= read -r src; do
  obj=".task-build/obj/$src.o"
  mkdir -p "$(dirname "$obj")"
  c++ -std=c++20 -O1 -w -I. -Irasterizer -Iexternal -include task_support/prelude.h -c "$src" -o "$obj"
done < .task-build/sources
c++ -std=c++20 -O1 -w -I. -Irasterizer -Iexternal -include task_support/prelude.h checks/baseline.cpp $(find .task-build/obj -name '*.o' -type f | LC_ALL=C sort) -o .task-build/baseline

if [[ $mode == target ]]; then
  c++ -std=c++20 -O1 -w -I. -Irasterizer -Iexternal -include task_support/prelude.h checks/target.cpp task_support/driver.c $(find .task-build/obj -name '*.o' -type f | LC_ALL=C sort) -o .task-build/target
test -s examples/face_texture.cpp
c++ -std=c++20 -O1 -w -I. -Irasterizer -Iexternal -include task_support/prelude.h examples/face_texture.cpp $(find .task-build/obj -name '*.o' -type f | LC_ALL=C sort) -o .task-build/example
mkdir -p .task-build/face-output
.task-build/example .task-build/face-output

  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in topology gutters rendering; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
