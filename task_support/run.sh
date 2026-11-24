#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_support/edited_ts_cases.cpp; fi
mkdir -p .task-build
if [[ $mode == target ]]; then
 test -s Source/C++/Core/Ap4EditedTs.cpp
 test -s Source/C++/Core/Ap4EditedTs.h
 test -s Source/C++/Apps/EditedTs/EditedTs.cpp
fi
cmake -S . -B .task-build/library -DBUILD_APPS=OFF -DBUILD_SHARED_LIBRARY=OFF -DCMAKE_OSX_ARCHITECTURES="$(uname -m)"
cmake --build .task-build/library --target ap4 -j4
task_includes=(-I. -ISource/C++/Core -ISource/C++/Codecs -ISource/C++/Crypto -ISource/C++/MetaData)
c++ -std=c++11 task_support/fixture.cpp -o .task-build/fixture
rm -f .task-build/simple.mp4
.task-build/fixture .task-build/simple.mp4
test -s .task-build/simple.mp4
c++ -std=c++11 "${task_includes[@]}" task_support/baseline.cpp .task-build/library/libap4.a -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++11 "${task_includes[@]}" task_support/api_probe.cpp .task-build/library/libap4.a -o .task-build/api-probe
.task-build/api-probe
c++ -std=c++11 "${task_includes[@]}" Source/C++/Apps/EditedTs/EditedTs.cpp .task-build/library/libap4.a -o .task-build/edited-ts
rm -f .task-build/simple.ts
.task-build/edited-ts .task-build/simple.mp4 .task-build/simple.ts 100000 3600
test -s .task-build/simple.ts
c++ -std=c++11 task_support/output_check.cpp -o .task-build/output-check
.task-build/output-check .task-build/simple.ts
c++ -std=c++11 "${task_includes[@]}" task_support/driver.c task_support/edited_ts_cases.cpp .task-build/library/libap4.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in timeline pcr failure compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == target ]]; then .task-build/baseline .task-build/simple.mp4 normal; else .task-build/baseline .task-build/simple.mp4; fi
