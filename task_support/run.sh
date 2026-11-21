#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/decompose_target.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O1 -g0 -I. -c task_support/cgltf_impl.cpp -o .task-build/cgltf.o
c++ -std=c++17 -O1 -g0 -I. task_support/baseline.cpp .task-build/cgltf.o -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s examples/cgltf_skin_decompose.h
  test -s examples/skin_decompose_solver.h
  test -s examples/cgltf_skin_decompose.cpp
  test -s examples/skin_decompose_solver.cpp
  test -s examples/decompose_example.cpp
  c++ -std=c++17 -O1 -g0 -I. -include cgltf.h -include examples/cgltf_skin_decompose.h -include examples/skin_decompose_solver.h -x c++ task_support/driver.c -x none test/decompose_target.cpp examples/cgltf_skin_decompose.cpp examples/skin_decompose_solver.cpp .task-build/cgltf.o -o .task-build/target
  c++ -std=c++17 -O1 -g0 -I. -include cgltf.h -include examples/cgltf_skin_decompose.h -include examples/skin_decompose_solver.h examples/decompose_example.cpp examples/cgltf_skin_decompose.cpp examples/skin_decompose_solver.cpp .task-build/cgltf.o -o .task-build/example
  rm -f .task-build/decomposed.gltf
  .task-build/example .task-build/decomposed.gltf
  test -s .task-build/decomposed.gltf
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in optimization asset contracts; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
rm -f .task-build/baseline-roundtrip.gltf
.task-build/baseline .task-build/baseline-roundtrip.gltf
test -s .task-build/baseline-roundtrip.gltf
