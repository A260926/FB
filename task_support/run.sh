#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/periodic_target.c; fi
mkdir -p .task-build
cmake -S . -B .task-build/mmg -DBUILD=MMGS -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DBUILD_SHARED_LIBS=OFF -DUSE_SCOTCH=OFF -DUSE_ELAS=OFF -DUSE_VTK=OFF > .task-build/configure.log 2>&1 || { tail -30 .task-build/configure.log; exit 1; }
cmake --build .task-build/mmg --target libmmgs_a -j 4 > .task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
cc -std=c99 -O2 -I.task-build/mmg/include -c task_support/baseline.c -o .task-build/baseline.o
c++ .task-build/baseline.o .task-build/mmg/lib/libmmgs.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/mmgs/periodic_s.c
test -s libexamples/mmgs/periodic_headless.c
cc -std=c99 -O2 -I. -I.task-build/mmg/include -c task_support/driver.c -o .task-build/driver.o
cc -std=c99 -O2 -I. -I.task-build/mmg/include -c tests/periodic_target.c -o .task-build/target.o
c++ .task-build/driver.o .task-build/target.o .task-build/mmg/CMakeFiles/libmmgs_a.dir/src/mmgs/periodic_s.c.o .task-build/mmg/lib/libmmgs.a -lm -o .task-build/target
cc -std=c99 -O2 -I. -I.task-build/mmg/include -c libexamples/mmgs/periodic_headless.c -o .task-build/example.o
c++ .task-build/example.o .task-build/mmg/CMakeFiles/libmmgs_a.dir/src/mmgs/periodic_s.c.o .task-build/mmg/lib/libmmgs.a -lm -o .task-build/example
rm -f .task-build/periodic.mesh
.task-build/example .task-build/periodic.mesh
test -s .task-build/periodic.mesh
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in paired_edits rejection lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
