#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/unit/scene_basis_cases.cpp; fi
mkdir -p .task-build
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s code/PostProcessing/SceneBasisProcess.h
test -s code/PostProcessing/SceneBasisProcess.cpp
test -s samples/SceneBasis/main.cpp
c++ -std=c++17 -O0 -I.task-build/core/include -Iinclude -Icode -I. -include code/PostProcessing/SceneBasisProcess.h -c code/PostProcessing/SceneBasisProcess.cpp -o .task-build/scene_basis.o
c++ -std=c++17 -O0 -I.task-build/core/include -Iinclude -Icode -I. -include code/PostProcessing/SceneBasisProcess.h -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++17 -O0 -I.task-build/core/include -Iinclude -Icode -I. -include code/PostProcessing/SceneBasisProcess.h test/unit/scene_basis_cases.cpp .task-build/driver.o .task-build/scene_basis.o .task-build/core/lib/libassimp.a .task-build/core/contrib/zlib/libzlibstatic.a -o .task-build/target
c++ -std=c++17 -O0 -I.task-build/core/include -Iinclude -Icode -I. -include code/PostProcessing/SceneBasisProcess.h samples/SceneBasis/main.cpp .task-build/scene_basis.o .task-build/core/lib/libassimp.a .task-build/core/contrib/zlib/libzlibstatic.a -o .task-build/example
.task-build/example
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in geometry animation integration; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
