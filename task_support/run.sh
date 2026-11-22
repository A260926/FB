#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/latency_ramp.cpp; fi
if [[ $mode == target ]]; then
  test -s srtcore/latency_curve.cpp
  test -s srtcore/latency_curve.h
  rm -f .task-build/srt/CMakeFiles/srt_virtual.dir/srtcore/latency_curve.cpp.o
fi
mkdir -p .task-build
cmake -S . -B .task-build/srt -DENABLE_ENCRYPTION=OFF -DENABLE_APPS=OFF -DENABLE_TESTING=OFF -DENABLE_UNITTESTS=OFF -DENABLE_EXAMPLES=OFF -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DENABLE_BONDING=OFF -DENABLE_CXX11=ON -DENABLE_STDCXX_SYNC=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >.task-build/configure.log 2>&1 || { tail -30 .task-build/configure.log; exit 1; }
cmake --build .task-build/srt --target srt_static -j2 >.task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
c++ -std=c++11 -DENABLE_STDCXX_SYNC=1 -I. -Isrtcore -Icommon -Ihaicrypt -I.task-build/srt task_support/baseline.cpp .task-build/srt/libsrt.a -pthread -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s .task-build/srt/CMakeFiles/srt_virtual.dir/srtcore/latency_curve.cpp.o
test -s examples/latency_ramp.cpp
rm -f .task-build/latency-example
c++ -std=c++11 -DENABLE_STDCXX_SYNC=1 -include srtcore/latency_curve.h -I. -Isrtcore -Icommon -Ihaicrypt -I.task-build/srt examples/latency_ramp.cpp .task-build/srt/CMakeFiles/srt_virtual.dir/srtcore/latency_curve.cpp.o .task-build/srt/libsrt.a -pthread -o .task-build/latency-example
.task-build/latency-example
c++ -std=c++11 -x c++ -I. -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++11 -DENABLE_STDCXX_SYNC=1 -include srtcore/latency_curve.h -I. -Isrtcore -Icommon -Ihaicrypt -I.task-build/srt task_tests/latency_ramp.cpp .task-build/driver.o .task-build/srt/CMakeFiles/srt_virtual.dir/srtcore/latency_curve.cpp.o .task-build/srt/libsrt.a -pthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in curve delivery integration; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
