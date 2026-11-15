#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/trim_mesh_contracts.cpp; fi
if [[ $mode == target ]]; then
 test -s src/LNLib/include/TrimmedSurfaceMesh.h
 test -s src/LNLib/Geometry/Surface/TrimmedSurfaceMesh.cpp
 printf '#include "src/LNLib/include/TrimmedSurfaceMesh.h"\n' | c++ -std=c++17 -I. -Isrc/LNLib/include -Ithird_party/eigen -x c++ -fsyntax-only -
fi
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build --parallel 4
if [[ $mode == target ]]; then
  c++ -std=c++17 -I. -Isrc/LNLib/include -Ithird_party/eigen task_support/driver.c tests/trim_mesh_contracts.cpp .task-build/libkernel.a -o .task-build/target
  test -s examples/trim_mesh.cpp
  c++ -std=c++17 -Isrc/LNLib/include -Ithird_party/eigen examples/trim_mesh.cpp .task-build/libkernel.a -o .task-build/trim_mesh
  rm -f .task-build/trim_mesh.obj
.task-build/trim_mesh .task-build/trim_mesh.obj
  test -s .task-build/trim_mesh.obj
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in domain seam refinement; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
