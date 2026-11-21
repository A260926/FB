#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/subsurface_cases.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O1 -DSTB_IMAGE_STATIC -Isrc -Isrc/TheRestOfYourLife task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/TheRestOfYourLife/subsurface.h
test -s src/TheRestOfYourLife/subsurface.cpp
test -s examples/subsurface.cpp
c++ -std=c++17 -x c++ -I. -c task_support/driver.c -o .task-build/driver.o
printf '%s\n' '#include "../src/TheRestOfYourLife/subsurface.cpp"' '#include "../checks/subsurface_cases.cpp"' > .task-build/target-unity.cpp
c++ -std=c++17 -O1 -DSTB_IMAGE_STATIC -Isrc -Isrc/TheRestOfYourLife -I. -include src/TheRestOfYourLife/rtweekend.h -include src/TheRestOfYourLife/subsurface.h .task-build/target-unity.cpp .task-build/driver.o -o .task-build/target
printf '%s\n' '#include "../src/TheRestOfYourLife/subsurface.cpp"' '#include "../examples/subsurface.cpp"' > .task-build/example-unity.cpp
c++ -std=c++17 -O1 -DSTB_IMAGE_STATIC -Isrc -Isrc/TheRestOfYourLife -I. -include src/TheRestOfYourLife/rtweekend.h .task-build/example-unity.cpp -o .task-build/example
rm -f .task-build/wax.ppm
.task-build/example .task-build
test -s .task-build/wax.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in distance boundary transport; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
