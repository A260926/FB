#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  test -s src/GEL/HMesh/HolePatch.h
  test -s src/GEL/HMesh/HolePatch.cpp
  test -s src/GEL/HMesh/AttributedManifold.h
  test -s src/GEL/HMesh/AttributedManifold.cpp
  c++ -std=c++20 -pthread -I./src -iquote . -x c++ -fsyntax-only src/GEL/HMesh/HolePatch.h
  c++ -std=c++20 -pthread -I./src -iquote . -x c++ -fsyntax-only src/GEL/HMesh/HolePatch.cpp
  c++ -std=c++20 -pthread -I./src -iquote . -x c++ -fsyntax-only src/GEL/HMesh/AttributedManifold.h
  c++ -std=c++20 -pthread -I./src -iquote . -x c++ -fsyntax-only src/GEL/HMesh/AttributedManifold.cpp
fi
if [[ $mode == target ]]; then test -s tests/fair_hole_target.cpp; fi
make -s -f task_support/Makefile -j4 .task-build/baseline
if [[ $mode == target ]]; then
  make -s -f task_support/Makefile target example
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in fairing topology channels_io; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
