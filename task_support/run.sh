#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
 test -s examples/replay_fit.cpp
 test -s src/integrators/replay.h
 test -s src/integrators/replay.cpp
 c++ -std=c++11 -DNDEBUG -DPBRT_PROBES_NONE -Isrc -Isrc/core -I. -x c++ -fsyntax-only src/integrators/replay.h
 c++ -std=c++11 -DNDEBUG -DPBRT_PROBES_NONE -Isrc -Isrc/core -I. -x c++ -fsyntax-only src/integrators/replay.cpp
fi
if [[ $mode == target ]]; then test -s task_tests/replay.cpp; fi
case $(uname -s) in Darwin) export TASK_LINK_FLAGS=-Wl,-dead_strip ;; *) export TASK_LINK_FLAGS=-Wl,--gc-sections ;; esac
bash task_support/build.sh
if [[ $mode == target ]]; then
  c++ -std=c++11 -DNDEBUG -DPBRT_PROBES_NONE -Isrc -Isrc/core -I. task_support/driver.c task_tests/replay.cpp .task-build/libproduction.a $TASK_LINK_FLAGS -pthread -o .task-build/target
c++ -std=c++11 -DNDEBUG -DPBRT_PROBES_NONE -Isrc -Isrc/core -I. examples/replay_fit.cpp .task-build/libproduction.a $TASK_LINK_FLAGS -pthread -o .task-build/replay-example-bin
mkdir -p .task-build/replay-example
rm -f .task-build/replay-example/fit.ppm
rm -f .task-build/replay-example/loss.txt
rm -f .task-build/replay-example/parameters.txt
.task-build/replay-example-bin .task-build/replay-example
test -s .task-build/replay-example/fit.ppm
test -s .task-build/replay-example/loss.txt
test -s .task-build/replay-example/parameters.txt
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in primal adjoint lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
