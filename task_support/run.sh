#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/wavefront_target.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O1 -pthread -DNO_DOUBLE_PRECISION_SUPPORT -I. task_support/bvh_impl.cpp task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s wavefront.h
test -s wavefront.cpp
test -s examples/wavefront.cpp
build_dir=$(mktemp -d "$PWD/.task-build/wave.XXXXXX")
c++ -std=c++17 -O1 task_support/ppm_check.cpp -o "$build_dir/ppm-check"
for flavor in scalar native; do
  if [[ $flavor == scalar ]]; then task_flavor_define=-DTINYBVH_NO_SIMD; else task_flavor_define=-DTASK_NATIVE_BUILD; fi
  c++ -std=c++17 -O1 -pthread -DNO_DOUBLE_PRECISION_SUPPORT "$task_flavor_define" -I. -include wavefront.h -c task_support/bvh_impl.cpp -o "$build_dir/impl-$flavor.o"
  c++ -std=c++17 -O1 -pthread -DNO_DOUBLE_PRECISION_SUPPORT "$task_flavor_define" -I. -include wavefront.h -c wavefront.cpp -o "$build_dir/wave-$flavor.o"
  c++ -std=c++17 -O1 -pthread -DNO_DOUBLE_PRECISION_SUPPORT "$task_flavor_define" -I. -include wavefront.h "$build_dir/impl-$flavor.o" "$build_dir/wave-$flavor.o" task_support/api_probe.cpp -o "$build_dir/probe-$flavor"
  "$build_dir/probe-$flavor"
  c++ -std=c++17 -O1 -pthread -DNO_DOUBLE_PRECISION_SUPPORT "$task_flavor_define" -I. -include wavefront.h "$build_dir/impl-$flavor.o" "$build_dir/wave-$flavor.o" examples/wavefront.cpp -o "$build_dir/example-$flavor"
  "$build_dir/example-$flavor" "$build_dir/example-$flavor.ppm"
  "$build_dir/ppm-check" "$build_dir/example-$flavor.ppm"
  c++ -std=c++17 -O1 -pthread -DNO_DOUBLE_PRECISION_SUPPORT "$task_flavor_define" -I. -include wavefront.h "$build_dir/impl-$flavor.o" "$build_dir/wave-$flavor.o" task_support/driver.c tests/wavefront_target.cpp -o "$build_dir/target-$flavor"
  "$build_dir/target-$flavor"
done
cp "$build_dir/target-native" .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in masks resume opacity; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
