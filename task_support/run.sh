#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/gaussian_medium.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s include/GaussianMedium.h
test -s src/GaussianMedium.cpp
test -s examples/gaussian_medium.cpp
test -s .task-build/obj/GaussianMedium.o
objects=()
while IFS= read -r object; do objects+=("$object"); done < .task-build/objects.list
test "${#objects[@]}" -gt 0
c++ -std=c++17 -O0 -pthread -I. -Iinclude -include include/GaussianMedium.h -x c++ task_support/driver.c task_tests/gaussian_medium.cpp -x none "${objects[@]}" -o .task-build/target
c++ -std=c++17 -O0 -pthread -I. -Iinclude examples/gaussian_medium.cpp "${objects[@]}" -o .task-build/example
rm -f .task-build/gaussian.ppm .task-build/error.ppm .task-build/transmittance.ppm
.task-build/example
test -s .task-build/gaussian.ppm
test -s .task-build/error.ppm
test -s .task-build/transmittance.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in optical emission transport; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
