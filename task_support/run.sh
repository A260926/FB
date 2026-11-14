#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in baseline|target) ;; *) exit 2;; esac
mkdir -p .task-build
compiler=${CXX:-c++}
if [[ "$mode" == baseline ]]; then
  "$compiler" -std=c++17 -O2 -Iinclude -I. task_support/baseline.cpp -o .task-build/baseline > .task-build/baseline-build.log 2>&1 || { tail -30 .task-build/baseline-build.log; exit 3; }
  .task-build/baseline
else
  for f in include/PolyVox/TrilinearSurfaceExtractor.h include/PolyVox/TrilinearSurfaceExtractor.inl tests/TrilinearSurfaceTarget.cpp; do
    [[ -s "$f" ]] || { echo "missing or empty: $f" >&2; exit 2; }
  done
  "$compiler" -std=c++17 -O2 -Iinclude -I. task_support/driver.cpp task_support/production_smoke.cpp tests/TrilinearSurfaceTarget.cpp -o .task-build/target > .task-build/target-build.log 2>&1 || { tail -30 .task-build/target-build.log; exit 3; }
  output=$(mktemp)
  trap 'rm -f "$output"' EXIT
  .task-build/target > "$output" 2>&1 || { code=$?; cat "$output"; exit "$code"; }
  cat "$output"
  grep -Eq '^PRODUCTION_SMOKE vertices=[1-9][0-9]* triangles=[1-9][0-9]*$' "$output"
  for group in face interior seams orientation invalid; do
    grep -Eq "^GROUP $group cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output"
fi
