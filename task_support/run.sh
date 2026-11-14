#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/fvar_checks.cpp; fi
mkdir -p .task-build
core_sources="src/OpenMesh/Core/Mesh/ArrayKernel.cc src/OpenMesh/Core/Mesh/PolyConnectivity.cc src/OpenMesh/Core/Mesh/TriConnectivity.cc src/OpenMesh/Core/Mesh/BaseKernel.cc src/OpenMesh/Core/Utils/BaseProperty.cc src/OpenMesh/Core/System/omstream.cc"
extra_sources=$(find src/OpenMesh -name "*.cpp" -type f -print)
${CXX:-c++} -std=c++11 -O0 -iquote . -Isrc task_support/baseline.cpp $core_sources $extra_sources -o .task-build/baseline
if [[ $mode == target ]]; then
  ${CXX:-c++} -std=c++11 -O0 -iquote . -Isrc -x c++ task_support/driver.c tests/fvar_checks.cpp $core_sources $extra_sources -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in catmull loop seams lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
