#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/thb_contracts.cpp; fi
if [[ $mode == target ]]; then
 test -s src/LNLib/include/ThbSurface.h
 test -s src/LNLib/Geometry/Surface/ThbSurface.cpp
 test -s examples/thb_sculpt.cpp
 cmake -S task_support -B .task-build/cmake -DCMAKE_BUILD_TYPE=Release -DTASK_TARGET=ON
else
 cmake -S task_support -B .task-build/cmake -DCMAKE_BUILD_TYPE=Release -DTASK_TARGET=OFF
fi
cmake --build .task-build/cmake --target baseline -j 4
if [[ $mode == target ]]; then
  test -s src/LNLib/include/ThbSurface.h
test -s src/LNLib/Geometry/Surface/ThbSurface.cpp
test -s examples/thb_sculpt.cpp
find .task-build/cmake -type f \( -name 'ThbSurface.cpp.o' -o -name 'NurbsSurface.cpp.o' -o -name 'thb_contracts.cpp.o' -o -name 'thb_sculpt.cpp.o' \) -delete
rm -f .task-build/cmake/target .task-build/cmake/thb_example .task-build/target
cmake --build .task-build/cmake --target target thb_example -j 4
# Keep a copied artifact for parity, but execute the linker-produced binary
# directly: a `cp`'d executable can carry a provenance extended attribute that
# a restricted sandbox refuses to run, whereas the build output itself is fine.
cp .task-build/cmake/target .task-build/target 2>/dev/null || true
rm -f .task-build/thb.obj
.task-build/cmake/thb_example .task-build/thb.obj
test -s .task-build/thb.obj
  output_log=$(mktemp 2>/dev/null || echo ".task-build/target-run.$$")
  trap 'rm -f "$output_log"' EXIT
  set +e
  # A restricted sandbox can intermittently kill a freshly spawned executable
  # with a signal (status >= 128).  Retry only those signal deaths; assertion
  # failures still return the ordinary non-zero test status and fail at once.
  status=1
  for attempt in 1 2 3; do
    .task-build/cmake/target >"$output_log" 2>&1
    status=$?
    [[ $status -ge 128 ]] || break
  done
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in truncation transfer rendering; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/cmake/baseline
