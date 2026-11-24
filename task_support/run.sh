#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/nested_motion.cpp; fi
mkdir -p .task-build
c++ -std=c++11 -O0 -I. -c minipbrt.cpp -o .task-build/minipbrt.o
c++ -std=c++11 -O0 -I. task_support/baseline.cpp .task-build/minipbrt.o -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s minipbrt_scene.h
test -s minipbrt_scene.cpp
test -s examples/nested_motion.cpp
c++ -std=c++11 -O0 -I. -include minipbrt_scene.h -c minipbrt_scene.cpp -o .task-build/minipbrt_scene.o
c++ -std=c++11 -O0 -I. -include minipbrt_scene.h task_support/driver.c tests/nested_motion.cpp .task-build/minipbrt.o .task-build/minipbrt_scene.o -o .task-build/target
c++ -std=c++11 -O0 -I. -include minipbrt_scene.h examples/nested_motion.cpp .task-build/minipbrt.o .task-build/minipbrt_scene.o -o .task-build/example
.task-build/example
  output_log=$(mktemp "${TMPDIR:-/tmp}/minipbrt-target.XXXXXX")
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in loading sampling bounds; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
