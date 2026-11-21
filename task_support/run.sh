#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/periodic.cpp; fi
mkdir -p .task-build
make -s -j4 -f task_support/upstream.mk .task-build/baseline
if [[ $mode == target ]]; then
  test -s Sources/BSplineLib/ParameterSpaces/periodic_axis.hpp
test -s Sources/BSplineLib/ParameterSpaces/periodic_axis.cpp
test -s examples/periodic_obj.cpp
c++ -O1 -std=c++20 -ISources -I. -include Sources/BSplineLib/ParameterSpaces/periodic_axis.hpp -c Sources/BSplineLib/ParameterSpaces/periodic_axis.cpp -o .task-build/periodic_axis.o
c++ -O1 -std=c++20 -ISources -I. -x c++ task_support/driver.c -x none tests/periodic.cpp .task-build/periodic_axis.o .task-build/libbspline.a -o .task-build/target
c++ -O1 -std=c++20 -ISources -I. examples/periodic_obj.cpp .task-build/periodic_axis.o .task-build/libbspline.a -o .task-build/example
rm -f .task-build/periodic.obj
.task-build/example .task-build/periodic.obj
test -s .task-build/periodic.obj
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in orbit derivatives roundtrip; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
