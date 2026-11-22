#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/receive_restart.cpp; fi
if [[ $mode == target ]]; then
  test -s src/receive_restart.cc
  test -s src/receive_restart.hh
  rm -f .task-build/uvgrtp/CMakeFiles/uvgrtp.dir/src/receive_restart.cc.o
fi
mkdir -p .task-build
cmake -G "Unix Makefiles" -S . -B .task-build/uvgrtp -DUVGRTP_DISABLE_CRYPTO=ON -DUVGRTP_DISABLE_TESTS=ON -DUVGRTP_DISABLE_EXAMPLES=ON -DUVGRTP_DISABLE_INSTALL=ON -DUVGRTP_DOWNLOAD_CRYPTO=OFF -DUVGRTP_BUILD_SHARED=OFF -DCMAKE_BUILD_TYPE=Release >.task-build/configure.log 2>&1 || { tail -30 .task-build/configure.log; exit 1; }
cmake --build .task-build/uvgrtp --target uvgrtp -j2 >.task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
defs=$(sed -n 's/^CXX_DEFINES = //p' .task-build/uvgrtp/CMakeFiles/uvgrtp.dir/flags.make)
c++ $defs -std=c++17 -I. -Iinclude -Isrc -I.task-build/uvgrtp/include task_support/baseline.cpp .task-build/uvgrtp/libuvgrtp.a -pthread -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s .task-build/uvgrtp/CMakeFiles/uvgrtp.dir/src/receive_restart.cc.o
test -s examples/receive_restart.cpp
rm -f .task-build/restart-example
c++ $defs -std=c++17 -include src/receive_restart.hh -I. -Iinclude -Isrc -I.task-build/uvgrtp/include examples/receive_restart.cpp .task-build/uvgrtp/CMakeFiles/uvgrtp.dir/src/receive_restart.cc.o .task-build/uvgrtp/libuvgrtp.a -pthread -o .task-build/restart-example
.task-build/restart-example
c++ -std=c++17 -x c++ -I. -c task_support/driver.c -o .task-build/driver.o
c++ $defs -std=c++17 -include src/receive_restart.hh -I. -Iinclude -Isrc -I.task-build/uvgrtp/include task_tests/receive_restart.cpp .task-build/driver.o .task-build/uvgrtp/CMakeFiles/uvgrtp.dir/src/receive_restart.cc.o .task-build/uvgrtp/libuvgrtp.a -pthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in admission media reports; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
