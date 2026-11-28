#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/volume_skin_target.cpp; fi
rm -rf .task-build
mkdir .task-build
for src in src/cal3d/*.cpp; do
  case "$src" in */PCH.cpp|*/normalutil.cpp|*/compactmeshwriter.cpp|*/volumeskin.cpp) continue;; esac
  c++ -std=c++17 -O0 -DUSE_CAL3D_WITH_CPP_11 -DIMVU_NO_INTRINSICS -Isrc -Ivendor/rapidxml -I. -c "$src" -o ".task-build/$(basename "$src" .cpp).o"
done
ar rcs .task-build/libcal3d.a .task-build/*.o
c++ -std=c++17 -O0 -DUSE_CAL3D_WITH_CPP_11 -DIMVU_NO_INTRINSICS -Isrc -Ivendor/rapidxml -I. task_support/baseline.cpp .task-build/libcal3d.a -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/cal3d/volumeskin.h
test -s src/cal3d/volumeskin.cpp
test -s examples/volume_skin.cpp
c++ -std=c++17 -O0 -DUSE_CAL3D_WITH_CPP_11 -DIMVU_NO_INTRINSICS -Isrc -Ivendor/rapidxml -I. -include src/cal3d/volumeskin.h -c src/cal3d/volumeskin.cpp -o .task-build/volumeskin.o
c++ -std=c++17 -O0 -DUSE_CAL3D_WITH_CPP_11 -DIMVU_NO_INTRINSICS -Isrc -Ivendor/rapidxml -I. task_support/volume-consumer.cpp .task-build/volumeskin.o .task-build/libcal3d.a -o .task-build/consumer
.task-build/consumer
c++ -std=c++17 -O0 -DUSE_CAL3D_WITH_CPP_11 -DIMVU_NO_INTRINSICS -Isrc -Ivendor/rapidxml -I. examples/volume_skin.cpp .task-build/volumeskin.o .task-build/libcal3d.a -o .task-build/example
.task-build/example
c++ -std=c++17 -O0 -DUSE_CAL3D_WITH_CPP_11 -DIMVU_NO_INTRINSICS -Isrc -Ivendor/rapidxml -I. test/volume_skin_target.cpp task_support/driver.c .task-build/volumeskin.o .task-build/libcal3d.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in cage solver pipeline; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
