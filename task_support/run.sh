#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/sky_sh_target.cpp; fi
mkdir -p .task-build
c++ -std=c++14 -O2 -include cstdint -I. SunSky.cpp tests/sky_baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s SunSkySH.cpp
test -s SunSkySH.hpp
test -s sky_sh_basis.inc
c++ -std=c++14 -O2 -include cstdint -I. -x c++ SunSky.cpp SunSkySH.cpp tests/sky_sh_target.cpp task_support/driver.c -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in projection convolution shader; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
