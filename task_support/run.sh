#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_support/row_cases.cc; fi
mkdir -p .task-build
cmake -S . -B .task-build/cmake -DBUILD_TESTS=OFF -DBUILD_APPS=OFF -DENABLE_ASSEMBLY=OFF -DHIGH_BITDEPTH=ON -DXVC_TARGET_ARCH="$(uname -m)" -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build .task-build/cmake --target xvc_enc_lib xvc_dec_lib -j 4
c++ -std=c++11 -O1 -Isrc -Itask_support task_support/baseline.cc .task-build/cmake/src/libxvcenc.a .task-build/cmake/src/libxvcdec.a -pthread -o .task-build/baseline

if [[ $mode == target ]]; then
  test -s examples/row_restart.cc
c++ -std=c++11 -O1 -Isrc -Itask_support -include src/xvc_enc_lib/xvcenc.h task_support/api_probe.cc .task-build/cmake/src/libxvcenc.a .task-build/cmake/src/libxvcdec.a -pthread -o .task-build/api_probe
c++ -std=c++11 -O1 -Isrc -I. -x c++ task_support/driver.c task_support/row_cases.cc -x none .task-build/cmake/src/libxvcenc.a .task-build/cmake/src/libxvcdec.a -pthread -o .task-build/target
c++ -std=c++11 -O1 -Isrc -I. examples/row_restart.cc .task-build/cmake/src/libxvcenc.a .task-build/cmake/src/libxvcdec.a -pthread -o .task-build/row-example
rm -f .task-build/api-normal.xvc .task-build/api-normal.yuv
.task-build/api_probe
test -s .task-build/api-normal.xvc
test "$(wc -c < .task-build/api-normal.yuv | tr -d ' ')" = 122880
rm -f .task-build/row-restart.xvc .task-build/row-restart.yuv .task-build/example.log
(cd .task-build && ./row-example > example.log)
cat .task-build/example.log
grep -Fx 'ROW_RESTART_EXAMPLE frames=5' .task-build/example.log
test -s .task-build/row-restart.xvc
test "$(wc -c < .task-build/row-restart.yuv | tr -d ' ')" = 122880

  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in entropy neighbors reconstruction malformed compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode != target ]]; then
  rm -f .task-build/baseline.xvc .task-build/baseline.yuv
  .task-build/baseline
fi
