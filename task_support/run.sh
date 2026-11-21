#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/fog_adjoint.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s nanovdb/nanovdb/tools/FogAdjoint.h
test -s nanovdb/nanovdb/tools/FogAdjoint.cc
test -s examples/fit_fog.cpp
c++ -std=c++17 -O1 -I nanovdb -I . -x c++ -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++17 -O1 -I nanovdb -I . -include nanovdb/nanovdb/tools/FogAdjoint.h -c tests/fog_adjoint.cpp -o .task-build/cases.o
c++ .task-build/driver.o .task-build/cases.o .task-build/FogAdjoint.cc.o .task-build/libfog.a -o .task-build/target
c++ -std=c++17 -O1 -I nanovdb -I . examples/fit_fog.cpp .task-build/FogAdjoint.cc.o .task-build/libfog.a -o .task-build/fit-fog
mkdir -p .task-build/fog-fit
rm -f .task-build/fog-fit/before.ppm .task-build/fog-fit/after.ppm .task-build/fog-fit/loss.txt
.task-build/fit-fog
test -s .task-build/fog-fit/before.ppm
test -s .task-build/fog-fit/after.ppm
test -s .task-build/fog-fit/loss.txt
  output_log=$(mktemp .task-build/target-output.XXXXXX)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in primal adjoint snapshot; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
