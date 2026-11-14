#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in baseline|target) ;; *) exit 2;; esac
if [[ "$mode" == target ]]; then
  for f in include/ozz/animation/offline/skeleton_reducer.h src/animation/offline/skeleton_reducer.cc test/animation/offline/skeleton_reducer_target.cpp; do
    [[ -s "$f" ]] || { echo "missing or empty: $f" >&2; exit 2; }
  done
  grep -q 'skeleton_reducer.cc' src/animation/offline/CMakeLists.txt || { echo 'reducer must be in offline library' >&2; exit 2; }
fi
build=".task-build-$mode"
cmake -S task_support -B "$build" -DTASK_MODE="$mode" -DCMAKE_BUILD_TYPE=Release > "$build-config.log" 2>&1 || { tail -30 "$build-config.log"; exit 3; }
cmake --build "$build" --target task_runner -j 4 > "$build-build.log" 2>&1 || { tail -30 "$build-build.log"; exit 4; }
if [[ "$mode" == target ]]; then
  output=$(mktemp)
  trap 'rm -f "$output"' EXIT
  "$build/task_runner" > "$output" 2>&1 || { code=$?; cat "$output"; exit "$code"; }
  cat "$output"
  for group in reduction shear mapping runtime invalid; do
    grep -Eq "^GROUP $group cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output"
else
  "$build/task_runner"
fi
