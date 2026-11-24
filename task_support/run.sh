#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/irradiance_cache.cpp; fi
mkdir -p .task-build
sources=(Cpp/Source/Test.cpp Cpp/Source/Maths.cpp Cpp/Source/enkiTS/TaskScheduler.cpp Cpp/Source/enkiTS/TaskScheduler_c.cpp)
if [[ -s Cpp/Source/IrradianceCache.cpp ]]; then sources+=(Cpp/Source/IrradianceCache.cpp); fi
c++ -std=c++17 -O2 -pthread -I . task_support/baseline.cpp "${sources[@]}" -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s Cpp/Source/IrradianceCache.h
test -s Cpp/Source/IrradianceCache.cpp
test -s examples/irradiance_cache.cpp
c++ -std=c++17 -O2 -pthread -I . -x c++ -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++17 -O2 -pthread -I . -include Cpp/Source/IrradianceCache.h -c tests/irradiance_cache.cpp -o .task-build/checks.o
c++ -std=c++17 -O2 -pthread -I . .task-build/checks.o .task-build/driver.o "${sources[@]}" -o .task-build/target
c++ -std=c++17 -O2 -pthread -I . -include Cpp/Source/IrradianceCache.h -c examples/irradiance_cache.cpp -o .task-build/demo.o
c++ -std=c++17 -O2 -pthread -I . .task-build/demo.o "${sources[@]}" -o .task-build/demo
rm -rf .task-build/demo-output
mkdir -p .task-build/demo-output
.task-build/demo .task-build/demo-output
test -s .task-build/demo-output/irradiance.ppm
  output_log=$(mktemp "${TMPDIR:-/tmp}/task-irradiance.XXXXXX")
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in records transport passes; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
