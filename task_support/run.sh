#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/fixed_seek.cpp; fi
if [[ $mode == target ]]; then
  test -s libs/eely/include/eely/clip/clip_impl_fixed_seek.h
  test -s libs/eely/include/eely/clip/clip_player_fixed_seek.h
  test -s libs/eely/src/eely/clip/clip_impl_fixed_seek.cpp
  test -s libs/eely/src/eely/clip/clip_player_fixed_seek.cpp
  test -s examples/fixed_seek.cpp
  c++ -std=c++23 -DEELY_DEBUG -Ilibs/eely/include -Iexternal/gsl/include -Iexternal/acl/include -I. -fsyntax-only -x c++ -include eely/clip/clip_impl_fixed_seek.h -include eely/clip/clip_player_fixed_seek.h /dev/null
  c++ -std=c++23 -DEELY_DEBUG -Ilibs/eely/include -Iexternal/gsl/include -Iexternal/acl/include -I. -fsyntax-only libs/eely/src/eely/clip/clip_impl_fixed_seek.cpp libs/eely/src/eely/clip/clip_player_fixed_seek.cpp
fi
sh task_support/build.sh
if [[ $mode == target ]]; then
  test -s libs/eely/include/eely/clip/clip_impl_fixed_seek.h
test -s libs/eely/include/eely/clip/clip_player_fixed_seek.h
test -s libs/eely/src/eely/clip/clip_impl_fixed_seek.cpp
test -s libs/eely/src/eely/clip/clip_player_fixed_seek.cpp
test -s examples/fixed_seek.cpp
c++ -std=c++23 -O1 -DEELY_DEBUG -Ilibs/eely/include -Iexternal/gsl/include -Iexternal/acl/include -I. -include eely/clip/clip_impl_fixed_seek.h -include eely/clip/clip_player_fixed_seek.h task_support/driver.c tests/fixed_seek.cpp $(cat .task-build/object-list) -o .task-build/target
c++ -std=c++23 -O1 -DEELY_DEBUG -Ilibs/eely/include -Iexternal/gsl/include -Iexternal/acl/include -I. -include eely/clip/clip_impl_fixed_seek.h -include eely/clip/clip_player_fixed_seek.h examples/fixed_seek.cpp $(cat .task-build/object-list) -o .task-build/fixed_seek_example
.task-build/fixed_seek_example
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in checkpoints seek_values pipeline; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
