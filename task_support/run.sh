#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/target.cpp; fi
mkdir -p .task-build
printf '*\n' > .task-build/.gitignore
cmake_bin=$(command -v cmake || true)
if [[ -z "$cmake_bin" ]]; then cmake_bin=/opt/homebrew/bin/cmake; fi
test -x "$cmake_bin"
"$cmake_bin" -S . -B .task-build/gmsh -DDEFAULT=0 -DENABLE_MESH=1 -DENABLE_PARSER=1 -DENABLE_BUILD_LIB=1 -DENABLE_EIGEN=1 -DENABLE_SYSTEM_CONTRIB=0 -DENABLE_PRIVATE_API=1 -DGMSH_RELEASE=1 -DCMAKE_BUILD_TYPE=Release
"$cmake_bin" --build .task-build/gmsh --target lib --parallel 4
c++ -std=c++17 -Iapi task_support/baseline.cpp .task-build/gmsh/libgmsh.a -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -x c++ -I. -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++17 -I. -Iapi -Isrc/common -Isrc/geo -Isrc/numeric -Isrc/mesh -Isrc/solver -I.task-build/gmsh/src/common task_tests/target.cpp .task-build/driver.o .task-build/gmsh/libgmsh.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in import_roles conversion geometry_io; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline .task-build/normal
