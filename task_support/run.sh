#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/Bevel_task_test.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  compiler=${CXX:-c++}; if [[ $(uname -s) == Darwin ]]; then task_link=(-Wl,-dead_strip); else task_link=(-Wl,--gc-sections); fi
"$compiler" -std=c++23 -O0 -g0 -DNDEBUG -I. "${task_link[@]}" -x c++ task_support/driver.c test/Bevel_task_test.cpp libHh/MeshBevel.cpp -x none .task-build/Mesh.o .task-build/GMesh.o .task-build/MeshOp.o .task-build/Geometry.o .task-build/GeomOp.o .task-build/Polygon.o .task-build/Hh.o .task-build/Hh_init.o .task-build/FileIO.o .task-build/Stat.o .task-build/Timer.o .task-build/Random.o .task-build/A3dStream.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in bevel_geometry junction_topology attributes_replay compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
