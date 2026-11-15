#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  test -s vcg/complex/algorithms/udim_tiles.h
  c++ -std=c++17 -I. -Ieigenlib -x c++ -fsyntax-only vcg/complex/algorithms/udim_tiles.h
fi
if [[ $mode == target ]]; then test -s tests/udim_tiles.cpp; test -s examples/udim_tiles.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O0 -I. -Ieigenlib task_support/baseline.cpp wrap/ply/plylib.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  extra_sources="$(find vcg/complex/algorithms -type f -name 'udim*.cpp' -print)"
c++ -std=c++17 -O0 -I. -Ieigenlib -x c++ task_support/driver.c tests/udim_tiles.cpp wrap/ply/plylib.cpp $extra_sources -o .task-build/target
c++ -std=c++17 -O0 -I. -Ieigenlib examples/udim_tiles.cpp wrap/ply/plylib.cpp $extra_sources -o .task-build/example; .task-build/example
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in tiles topology persistence; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
