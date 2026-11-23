#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/clock_slew.cpp; fi
bash task_support/build.sh
incs=(-I. -IliveMedia/include -Igroupsock/include -IUsageEnvironment/include -IBasicUsageEnvironment/include)
libs=(.task-build/libliveMedia.a .task-build/libgroupsock.a .task-build/libBasicUsageEnvironment.a .task-build/libUsageEnvironment.a)
c++ -std=c++20 -DNO_OPENSSL=1 "${incs[@]}" task_support/baseline.cpp "${libs[@]}" -o .task-build/baseline
if [[ $mode == target ]]; then
  objects=(.task-build/liveMedia/*.o .task-build/groupsock/*.o .task-build/BasicUsageEnvironment/*.o .task-build/UsageEnvironment/*.o)
test -s examples/clock-slew.cpp
rm -f .task-build/clock-slew-example
c++ -std=c++20 -DNO_OPENSSL=1 "${incs[@]}" examples/clock-slew.cpp "${objects[@]}" -o .task-build/clock-slew-example
.task-build/clock-slew-example
c++ -std=c++20 -DNO_OPENSSL=1 "${incs[@]}" -x c++ task_support/driver.c task_tests/clock_slew.cpp -x none "${objects[@]}" -o .task-build/target
  output_log=$(mktemp 2>/dev/null || echo .task-build/target-output.log)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in segments fragment reports; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
